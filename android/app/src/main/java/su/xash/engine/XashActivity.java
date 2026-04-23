package su.xash.engine;

import android.annotation.SuppressLint;
import android.content.pm.ActivityInfo;
import android.content.res.AssetManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.preference.PreferenceManager;
import android.content.SharedPreferences;
import android.provider.Settings.Secure;
import android.util.Log;
import android.view.KeyEvent;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import su.xash.engine.util.AndroidBug5497Workaround;

import java.io.File;
import java.util.Arrays;
import java.util.List;

public class XashActivity extends SDLActivity {
    private boolean mUseVolumeKeys;
    private String mPackageName;
    private static final String TAG = "XashActivity";
    private SharedPreferences mPreferences;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        mPreferences = PreferenceManager.getDefaultSharedPreferences(this);

        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }

        AndroidBug5497Workaround.assistActivity(this);
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        System.exit(0);
    }

    @Override
    protected String[] getLibraries() {
        return new String[]{"SDL2", "xash"};
    }

    @SuppressLint("HardwareIds")
    private String getAndroidID() {
        return Secure.getString(getContentResolver(), Secure.ANDROID_ID);
    }

    @SuppressLint("ApplySharedPref")
    private void saveAndroidID(String id) {
        getSharedPreferences("xash_preferences", MODE_PRIVATE).edit().putString("xash_id", id).commit();
    }

    private String loadAndroidID() {
        return getSharedPreferences("xash_preferences", MODE_PRIVATE).getString("xash_id", "");
    }

    @Override
    public String getCallingPackage() {
        if (mPackageName != null) {
            return mPackageName;
        }
        return super.getCallingPackage();
    }

    private AssetManager getAssets(boolean isEngine) {
        AssetManager am = null;
        if (isEngine) {
            am = getAssets();
        } else {
            try {
                am = getPackageManager().getResourcesForApplication(getCallingPackage()).getAssets();
            } catch (Exception e) {
                Log.e(TAG, "Unable to load mod assets!");
                e.printStackTrace();
            }
        }
        return am;
    }

    private String[] getAssetsList(boolean isEngine, String path) {
        AssetManager am = getAssets(isEngine);
        try {
            return am.list(path);
        } catch (Exception e) {
            e.printStackTrace();
        }
        return new String[]{};
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (SDLActivity.mBrokenLibraries) {
            return false;
        }
        int keyCode = event.getKeyCode();
        if (!mUseVolumeKeys) {
            if (keyCode == KeyEvent.KEYCODE_VOLUME_DOWN || keyCode == KeyEvent.KEYCODE_VOLUME_UP || keyCode == KeyEvent.KEYCODE_CAMERA || keyCode == KeyEvent.KEYCODE_ZOOM_IN || keyCode == KeyEvent.KEYCODE_ZOOM_OUT) {
                return false;
            }
        }
        return getWindow().superDispatchKeyEvent(event);
    }

    private String getGlobalArguments() {
        String globalArgs = mPreferences.getString("global_arguments", "");
        if (globalArgs != null && !globalArgs.trim().isEmpty()) {
            return globalArgs.trim();
        }
        return "";
    }

    private String combineArguments(String originalArgs, String globalArgs) {
        if (globalArgs.isEmpty()) {
            return originalArgs;
        }
        
        if (originalArgs == null || originalArgs.trim().isEmpty()) {
            return globalArgs;
        }
        
        return originalArgs.trim() + " " + globalArgs;
    }

    private String findBestBasedir(String gamedir) {
        File internalDir = new File(getExternalFilesDir(null).getAbsolutePath() + "/" + gamedir);
        if (internalDir.exists() && internalDir.isDirectory()) {
            Log.d(TAG, "Game found in internal storage: " + internalDir.getAbsolutePath());
            return getExternalFilesDir(null).getAbsolutePath();
        }
        
        File externalDir = new File(Environment.getExternalStorageDirectory().getAbsolutePath() + "/xash/" + gamedir);
        if (externalDir.exists() && externalDir.isDirectory()) {
            Log.d(TAG, "Game found in external storage: " + externalDir.getAbsolutePath());
            return Environment.getExternalStorageDirectory().getAbsolutePath() + "/xash";
        }
        
        boolean useInternalStorage = mPreferences.getBoolean("storage_toggle", false);
        if (useInternalStorage) {
            Log.d(TAG, "Game not found, using internal storage as default");
            return getExternalFilesDir(null).getAbsolutePath();
        } else {
            Log.d(TAG, "Game not found, using external storage as default");
            return Environment.getExternalStorageDirectory().getAbsolutePath() + "/xash";
        }
    }

    @Override
    protected String[] getArguments() {
        String gamedir = getIntent().getStringExtra("gamedir");
        if (gamedir == null) gamedir = "valve";
        
        String basedir = findBestBasedir(gamedir);
        nativeSetenv("XASH3D_BASEDIR", basedir);
        nativeSetenv("XASH3D_GAME", gamedir);
        
        Log.d(TAG, "Using basedir: " + basedir + " for game: " + gamedir);

        String gamelibdir = getIntent().getStringExtra("gamelibdir");
        if (gamelibdir != null) nativeSetenv("XASH3D_GAMELIBDIR", gamelibdir);

        String pakfile = getIntent().getStringExtra("pakfile");
        if (pakfile != null) nativeSetenv("XASH3D_EXTRAS_PAK2", pakfile);

        mUseVolumeKeys = getIntent().getBooleanExtra("usevolume", false);
        mPackageName = getIntent().getStringExtra("package");

        String[] env = getIntent().getStringArrayExtra("env");
        if (env != null) {
            for (int i = 0; i < env.length; i += 2)
                nativeSetenv(env[i], env[i + 1]);
        }

        String argv = getIntent().getStringExtra("argv");
        if (argv == null) argv = "-console -log";

        String globalArgs = getGlobalArguments();
        if (!globalArgs.isEmpty()) {
            Log.d(TAG, "Global arguments found: " + globalArgs);
            argv = combineArguments(argv, globalArgs);
        }

        if (!argv.contains("-game") && !gamedir.equals("valve")) {
            argv += " -game " + gamedir;
            Log.d(TAG, "Added -game parameter to argv: " + argv);
        }

        if (argv.indexOf(" -dll ") < 0 && gamelibdir == null) {
            final List<String> mobile_hacks_gamedirs = Arrays.asList(new String[]{
                "aom", "bdlands", "biglolly", "bshift", "caseclosed",
                "hl_urbicide", "induction", "redempt", "secret",
                "sewer_beta", "tot", "vendetta" });

            if (mobile_hacks_gamedirs.contains(gamedir))
                argv += " -dll @hl";
        }

        Log.d(TAG, "Final argv: " + argv);
        return argv.split(" ");
    }
}
