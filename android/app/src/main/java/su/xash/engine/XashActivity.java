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
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
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
        mPreferences = PreferenceManager.getDefaultSharedPreferences(this);

        super.onCreate(savedInstanceState);

        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }

        AndroidBug5497Workaround.assistActivity(this);

        // Apply surface size after SDL creates the view but before it initializes the surface
        applyResolutionIfNeeded();
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

        // Handle resolution settings
        argv = addResolutionSettings(argv);

        Log.d(TAG, "Final argv: " + argv);
        return argv.split(" ");
    }

    private String addResolutionSettings(String argv) {
        // Read resolution settings from global preferences (app_preferences)
        boolean resolutionEnabled = mPreferences.getBoolean("resolution_enabled", false);
        if (!resolutionEnabled) {
            Log.d(TAG, "Resolution settings: using default");
            return argv;
        }

        // Custom resolution is always used when enabled (no scale mode anymore)
        boolean resolutionCustom = true;
        int width, height;

        // Read custom width/height from preferences
        width = mPreferences.getInt("resolution_width", 854);
        height = mPreferences.getInt("resolution_height", 480);
        Log.d(TAG, "Resolution settings: " + width + "x" + height);

        // Enforce minimum resolution
        if (width < 320) width = 320;
        if (height < 240) height = 240;

        // Add resolution to command line arguments
        if (!argv.contains("-width ")) {
            argv += " -width " + width;
        }
        if (!argv.contains("-height ")) {
            argv += " -height " + height;
        }

        Log.d(TAG, "Final resolution: " + width + "x" + height);
        return argv;
    }

    // Apply custom resolution by setting surface holder size
    // This must be called BEFORE SDL initializes the surface
    private void applyResolutionIfNeeded() {
        boolean resolutionEnabled = mPreferences.getBoolean("resolution_enabled", false);
        if (!resolutionEnabled) {
            Log.d(TAG, "Resolution: using device default");
            return;
        }

        int width = mPreferences.getInt("resolution_width", 854);
        int height = mPreferences.getInt("resolution_height", 480);

        if (width < 320) width = 320;
        if (height < 240) height = 240;

        Log.d(TAG, "Resolution: setting surface size to " + width + "x" + height);

        try {
            // Find SDL's SurfaceView using reflection
            ViewGroup layout = (ViewGroup) getWindow().getDecorView().findViewById(android.R.id.content);
            if (layout != null && layout.getChildCount() > 0) {
                View child = layout.getChildAt(0);
                if (child instanceof SurfaceView) {
                    SurfaceView surfaceView = (SurfaceView) child;
                    SurfaceHolder holder = surfaceView.getHolder();
                    holder.setFixedSize(width, height);
                    Log.d(TAG, "Resolution: SurfaceHolder.setFixedSize(" + width + ", " + height + ") applied");
                } else {
                    Log.w(TAG, "Resolution: Could not find SurfaceView, found: " + child.getClass().getName());
                }
            } else {
                // Try to find SurfaceView in the entire view hierarchy
                SurfaceView surfaceView = findSurfaceView(layout);
                if (surfaceView != null) {
                    SurfaceHolder holder = surfaceView.getHolder();
                    holder.setFixedSize(width, height);
                    Log.d(TAG, "Resolution: SurfaceHolder.setFixedSize(" + width + ", " + height + ") applied (found in hierarchy)");
                } else {
                    Log.w(TAG, "Resolution: Could not find any SurfaceView in view hierarchy");
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "Resolution: Error applying surface size", e);
        }
    }

    private SurfaceView findSurfaceView(ViewGroup parent) {
        if (parent == null) return null;

        for (int i = 0; i < parent.getChildCount(); i++) {
            View child = parent.getChildAt(i);
            if (child instanceof SurfaceView) {
                return (SurfaceView) child;
            }
            if (child instanceof ViewGroup) {
                SurfaceView result = findSurfaceView((ViewGroup) child);
                if (result != null) return result;
            }
        }
        return null;
    }
}
