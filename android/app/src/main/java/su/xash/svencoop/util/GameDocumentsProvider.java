package su.xash.svencoop.util;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.Locale;

import su.xash.svencoop.R;

public class GameDocumentsProvider extends DocumentsProvider {
    private static final String ROOT_ID = "files:";
    private static final String[] ROOT_COLUMNS = {
        Root.COLUMN_ROOT_ID, Root.COLUMN_DOCUMENT_ID, Root.COLUMN_TITLE,
        Root.COLUMN_FLAGS, Root.COLUMN_ICON, Root.COLUMN_AVAILABLE_BYTES
    };
    private static final String[] DOCUMENT_COLUMNS = {
        Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE,
        Document.COLUMN_FLAGS, Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED
    };

    @Override
    public boolean onCreate() {
        return true;
    }

    private File rootDirectory() throws FileNotFoundException {
        File root = getContext().getExternalFilesDir(null);
        if (root == null) throw new FileNotFoundException("Game storage is unavailable");
        try {
            return root.getCanonicalFile();
        } catch (IOException error) {
            throw new FileNotFoundException(error.getMessage());
        }
    }

    private boolean inside(File root, File file) {
        return file.equals(root) || file.getPath().startsWith(root.getPath() + File.separator);
    }

    private File resolve(String documentId) throws FileNotFoundException {
        if (!documentId.startsWith(ROOT_ID)) throw new FileNotFoundException("Invalid document ID");
        File root = rootDirectory();
        try {
            File requested = new File(root, documentId.substring(ROOT_ID.length()));
            File file = requested.getCanonicalFile();
            if (!requested.getAbsoluteFile().equals(file)) throw new FileNotFoundException("Invalid document path");
            if (!inside(root, file) || !file.exists()) throw new FileNotFoundException("Invalid document path");
            return file;
        } catch (IOException error) {
            throw new FileNotFoundException(error.getMessage());
        }
    }

    private String documentId(File file) throws FileNotFoundException {
        File root = rootDirectory();
        try {
            File canonical = file.getCanonicalFile();
            if (!file.getAbsoluteFile().equals(canonical)) throw new FileNotFoundException("Invalid document path");
            file = canonical;
            if (!inside(root, file)) throw new FileNotFoundException("Outside game storage");
            return ROOT_ID + (root.equals(file) ? "" : file.getPath().substring(root.getPath().length() + 1));
        } catch (IOException error) {
            throw new FileNotFoundException(error.getMessage());
        }
    }

    private String mimeType(File file) {
        if (file.isDirectory()) return Document.MIME_TYPE_DIR;
        String name = file.getName();
        String extension = name.substring(name.lastIndexOf('.') + 1).toLowerCase(Locale.ROOT);
        if (extension.equals("log") || extension.equals("cfg")) return "text/plain";
        String type = MimeTypeMap.getSingleton().getMimeTypeFromExtension(extension);
        return type != null ? type : "application/octet-stream";
    }

    private void includeFile(MatrixCursor cursor, File file) throws FileNotFoundException {
        String id = documentId(file);
        int flags = 0;
        if (file.canWrite()) {
            flags |= file.isDirectory() ? Document.FLAG_DIR_SUPPORTS_CREATE : Document.FLAG_SUPPORTS_WRITE;
            if (!ROOT_ID.equals(id)) flags |= Document.FLAG_SUPPORTS_DELETE | Document.FLAG_SUPPORTS_RENAME;
        }
        cursor.newRow()
            .add(Document.COLUMN_DOCUMENT_ID, id)
            .add(Document.COLUMN_DISPLAY_NAME, ROOT_ID.equals(id) ? getContext().getString(R.string.app_name) : file.getName())
            .add(Document.COLUMN_MIME_TYPE, mimeType(file))
            .add(Document.COLUMN_FLAGS, flags)
            .add(Document.COLUMN_SIZE, file.isFile() ? file.length() : null)
            .add(Document.COLUMN_LAST_MODIFIED, file.lastModified());
    }

    @Override
    public Cursor queryRoots(String[] projection) {
        MatrixCursor cursor = new MatrixCursor(projection != null ? projection : ROOT_COLUMNS);
        try {
            File root = rootDirectory();
            cursor.newRow()
                .add(Root.COLUMN_ROOT_ID, ROOT_ID)
                .add(Root.COLUMN_DOCUMENT_ID, ROOT_ID)
                .add(Root.COLUMN_TITLE, getContext().getString(R.string.app_name))
                .add(Root.COLUMN_FLAGS, Root.FLAG_LOCAL_ONLY | Root.FLAG_SUPPORTS_CREATE | Root.FLAG_SUPPORTS_IS_CHILD)
                .add(Root.COLUMN_ICON, R.mipmap.ic_launcher)
                .add(Root.COLUMN_AVAILABLE_BYTES, root.getUsableSpace());
        } catch (FileNotFoundException ignored) {
        }
        return cursor;
    }

    @Override
    public Cursor queryDocument(String documentId, String[] projection) throws FileNotFoundException {
        MatrixCursor cursor = new MatrixCursor(projection != null ? projection : DOCUMENT_COLUMNS);
        includeFile(cursor, resolve(documentId));
        return cursor;
    }

    @Override
    public Cursor queryChildDocuments(String parentDocumentId, String[] projection, String sortOrder) throws FileNotFoundException {
        MatrixCursor cursor = new MatrixCursor(projection != null ? projection : DOCUMENT_COLUMNS);
        File[] children = resolve(parentDocumentId).listFiles();
        if (children == null) throw new FileNotFoundException("Directory is unavailable");
        for (File child : children) {
            try {
                includeFile(cursor, child);
            } catch (FileNotFoundException ignored) {
            }
        }
        cursor.setNotificationUri(getContext().getContentResolver(),
            DocumentsContract.buildChildDocumentsUri(getContext().getPackageName() + ".documents", parentDocumentId));
        return cursor;
    }

    @Override
    public boolean isChildDocument(String parentDocumentId, String documentId) {
        try {
            File parent = resolve(parentDocumentId);
            File child = resolve(documentId);
            return !parent.equals(child) && inside(parent, child);
        } catch (FileNotFoundException error) {
            return false;
        }
    }

    @Override
    public ParcelFileDescriptor openDocument(String documentId, String mode, CancellationSignal signal) throws FileNotFoundException {
        if (signal != null) signal.throwIfCanceled();
        File file = resolve(documentId);
        if (!file.isFile()) throw new FileNotFoundException("Not a file");
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.parseMode(mode));
    }

    private File destination(File parent, String displayName) throws FileNotFoundException {
        if (displayName.isEmpty() || displayName.equals(".") || displayName.equals("..")
            || displayName.indexOf('/') >= 0 || displayName.indexOf('\\') >= 0 || displayName.indexOf('\0') >= 0)
            throw new FileNotFoundException("Invalid file name");
        File target = new File(parent, displayName);
        if (target.exists()) throw new FileNotFoundException("File already exists");
        return target;
    }

    private void notifyDirectory(File directory) throws FileNotFoundException {
        getContext().getContentResolver().notifyChange(DocumentsContract.buildChildDocumentsUri(
            getContext().getPackageName() + ".documents", documentId(directory)), null);
    }

    @Override
    public String createDocument(String parentDocumentId, String mimeType, String displayName) throws FileNotFoundException {
        File parent = resolve(parentDocumentId);
        File target = destination(parent, displayName);
        try {
            boolean created = Document.MIME_TYPE_DIR.equals(mimeType) ? target.mkdir() : target.createNewFile();
            if (!created) throw new FileNotFoundException("Unable to create document");
            notifyDirectory(parent);
            return documentId(target);
        } catch (IOException error) {
            throw new FileNotFoundException(error.getMessage());
        }
    }

    @Override
    public String renameDocument(String documentId, String displayName) throws FileNotFoundException {
        File source = resolve(documentId);
        if (source.equals(rootDirectory())) throw new FileNotFoundException("Cannot rename root");
        File target = destination(source.getParentFile(), displayName);
        if (!source.renameTo(target)) throw new FileNotFoundException("Unable to rename document");
        notifyDirectory(target.getParentFile());
        return documentId(target);
    }

    private void deleteTree(File file) throws FileNotFoundException {
        documentId(file);
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) deleteTree(child);
        }
        if (!file.delete()) throw new FileNotFoundException("Unable to delete document");
    }

    @Override
    public void deleteDocument(String documentId) throws FileNotFoundException {
        File file = resolve(documentId);
        if (file.equals(rootDirectory())) throw new FileNotFoundException("Cannot delete root");
        deleteTree(file);
        notifyDirectory(file.getParentFile());
    }
}
