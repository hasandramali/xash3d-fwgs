package su.xash.svencoop;

import android.annotation.SuppressLint;
import android.content.pm.ActivityInfo;
import android.content.Context;
import android.content.res.AssetManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Environment;
import android.preference.PreferenceManager;
import android.content.SharedPreferences;
import android.provider.Settings.Secure;
import android.util.Log;
import android.util.DisplayMetrics;
import android.view.KeyEvent;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import su.xash.svencoop.util.SoftKeyboardPan;

import java.io.File;
import java.util.Arrays;
import java.util.List;

public class XashActivity extends SDLActivity {
    private boolean mUseVolumeKeys;
    private String mPackageName;
    private static final String TAG = "XashActivity";
    private static final int MIN_SURFACE_WIDTH = 320;
    private static final int MIN_SURFACE_HEIGHT = 200;
    private SharedPreferences mPreferences;
    private String mCachedArgv;
    private int mFixedSurfaceWidth;
    private int mFixedSurfaceHeight;
    private boolean mStretchFixedSurface;
    private int mAppliedSurfaceWidth = -1;
    private int mAppliedSurfaceHeight = -1;
    private boolean mAppliedSurfaceStretch = false;

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new StartupSurface(context);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        ensurePreferences();

        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            WindowManager.LayoutParams attributes = getWindow().getAttributes();
            attributes.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(attributes);
        }

        // SDLActivity queues windowed style in super.onCreate. Follow it on the
        // same UI queue, before StartupSurface releases the native thread.
        // Do not call SDL's setWindowStyle here: it waits for surfaceChanged
        // and would block this UI thread for its 500 ms timeout.
        new Handler(Looper.getMainLooper()).post(() -> {
            getWindow().getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_FORCE_NOT_FULLSCREEN);
            mFullscreenModeActive = true;
        });

        parseFixedResolution(getFinalArgv());
        applyFixedSurfaceSize();

        if (getBooleanPreference("keyboard_pans_screen", true)) {
            SoftKeyboardPan.assistActivity(this);
        } else {
            getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        applyFixedSurfaceSize();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            applyFixedSurfaceSize();
        }
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
            String[] list = am.list(path);
            return list != null ? list : new String[]{};
        } catch (Exception e) {
            e.printStackTrace();
        }
        return new String[]{};
    }

    // Safe-area insets as fractions of the window: left, top, right, bottom.
    // Called from native code on the engine thread; only reads view state,
    // never throws.
    @SuppressLint("NewApi")
    private float[] getWindowInsets() {
        float[] insets = new float[]{ 0, 0, 0, 0 };
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT_WATCH) {
                View decor = getWindow().getDecorView();
                if (decor != null) {
                    android.view.WindowInsets wi = decor.getRootWindowInsets();
                    int w = decor.getWidth(), h = decor.getHeight();
                    if (wi != null && w > 0 && h > 0) {
                        insets[0] = (float) wi.getSystemWindowInsetLeft() / w;
                        insets[1] = (float) wi.getSystemWindowInsetTop() / h;
                        insets[2] = (float) wi.getSystemWindowInsetRight() / w;
                        insets[3] = (float) wi.getSystemWindowInsetBottom() / h;
                    }
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "getWindowInsets failed, using zeros");
        }
        return insets;
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
        ensurePreferences();
        String globalArgs = mPreferences.getString("global_arguments", "");
        if (globalArgs != null && !globalArgs.trim().isEmpty()) {
            return globalArgs.trim();
        }
        return "";
    }

    private void ensurePreferences() {
        if (mPreferences == null) {
            mPreferences = PreferenceManager.getDefaultSharedPreferences(this);
        }
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
        // Single active storage root only: never probe the other side.
        // Probing both roots doubles slow-storage I/O and can silently
        // pick content from the inactive location.
        boolean useInternalStorage = mPreferences.getBoolean("storage_toggle", true);
        if (useInternalStorage) {
            File filesDir = getExternalFilesDir(null);
            String internalBase = (filesDir != null) ? filesDir.getAbsolutePath()
                    : "/storage/emulated/0/Android/data/su.xash.svencoop/files";
            Log.d(TAG, "Using internal storage basedir: " + internalBase + " for game: " + gamedir);
            return internalBase;
        } else {
            String externalBase = Environment.getExternalStorageDirectory().getAbsolutePath() + "/xash";
            Log.d(TAG, "Using external storage basedir: " + externalBase + " for game: " + gamedir);
            return externalBase;
        }
    }

    private String getFinalArgv() {
        if (mCachedArgv != null) {
            return mCachedArgv;
        }

        ensurePreferences();
        setStretchResolutionEnvironment();

        String rodir = getFilesDir().getAbsolutePath() + "/gamelibs";
        nativeSetenv("XASH3D_RODIR", rodir);
        Log.i(TAG, "XASH3D_RODIR = " + rodir);

        String gamedir = getIntent().getStringExtra("gamedir");
        if (gamedir == null) gamedir = "svencoop";
        
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

        // Background map (renderer kick): keeps the renderer fed from the first
        // seconds so a wedged first present (black screen) cannot stick on
        // affected devices. The engine embeds _xashnull (map + load cfg)
        // and extracts both on first use (BootMap_Ensure), then drops back
        // to the menu by itself once the background settles -- so the file
        // is always available and no device probing is needed. Placed first
        // so user +commands still win.
        if (argv.indexOf("map_background") < 0) {
            argv = "+map_background _xashnull " + argv;
        }

        String globalArgs = getGlobalArguments();
        if (!globalArgs.isEmpty()) {
            Log.d(TAG, "Global arguments found: " + globalArgs);
            argv = combineArguments(argv, globalArgs);
        }

        if (!argv.contains("-game") && !gamedir.equals("valve")) {
            argv += " -game " + gamedir;
            Log.d(TAG, "Added -game parameter to argv: " + argv);
        }

        String resolutionArgs = getRenderResolutionArguments(argv);
        if (!resolutionArgs.isEmpty()) {
            argv += " " + resolutionArgs;
        }

        if (getBooleanPreference("stretch_resolution", false) && !hasArgument(argv, "-stretch_resolution")) {
            argv += " -stretch_resolution";
        }

        if (getBooleanPreference("fix_font", false) && !hasArgument(argv, "-fixfont")) {
            argv += " -fixfont";
        }

        if (!getBooleanPreference("keyboard_pans_screen", true) && !hasArgument(argv, "-noresize")) {
            argv += " -noresize";
        }

        if (argv.indexOf(" -dll ") < 0 && gamelibdir == null) {
            final List<String> mobile_hacks_gamedirs = Arrays.asList(new String[]{
                "aom", "bdlands", "biglolly", "bshift", "caseclosed",
                "hl_urbicide", "induction", "redempt", "secret",
                "sewer_beta", "tot", "vendetta" });

            if (mobile_hacks_gamedirs.contains(gamedir))
                argv += " -dll @hl";
        }

        // Steam auth: use the local ticket broker when a Steam session is available.
        // NOTE: no -insecure argv is ever added: VAC-capable servers are the target.
        if (hasStoredSteamSession()) {
            su.xash.svencoop.model.SteamAuthManager.get(this).ensureSessionAsync();
            if (!argv.contains("cl_ticket_generator")) {
                argv += " +set cl_ticket_generator \"steam\"";
            }
        }

        Log.d(TAG, "Final argv: " + argv);
        mCachedArgv = argv.trim();
        return mCachedArgv;
    }

    private String getRenderResolutionArguments(String argv) {
        if (hasArgument(argv, "-width") || hasArgument(argv, "-height")) {
            return "";
        }

        String resolution = mPreferences.getString("render_resolution", "");
        if (resolution == null || resolution.trim().isEmpty()) {
            return "";
        }

        String[] parts = resolution.trim().split("[xX,\\s]+");
        if (parts.length < 2) {
            Log.w(TAG, "Invalid render resolution: " + resolution);
            return "";
        }

        try {
            int width = Integer.parseInt(parts[0]);
            int height = Integer.parseInt(parts[1]);

            if (width < MIN_SURFACE_WIDTH || height < MIN_SURFACE_HEIGHT) {
                Log.w(TAG, "Render resolution is too small: " + resolution);
                return "";
            }

            return "-width " + width + " -height " + height;
        } catch (NumberFormatException e) {
            Log.w(TAG, "Invalid render resolution: " + resolution);
            return "";
        }
    }

    private void setStretchResolutionEnvironment() {
        boolean stretch = getBooleanPreference("stretch_resolution", false);
        nativeSetenv("XASH3D_STRETCH_RESOLUTION", stretch ? "1" : "0");

        if (!stretch) {
            return;
        }

        DisplayMetrics metrics = new DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(metrics);

        int nativeWidth = Math.max(metrics.widthPixels, metrics.heightPixels);
        int nativeHeight = Math.min(metrics.widthPixels, metrics.heightPixels);

        if (nativeWidth >= MIN_SURFACE_WIDTH && nativeHeight >= MIN_SURFACE_HEIGHT) {
            nativeSetenv("XASH3D_NATIVE_WIDTH", String.valueOf(nativeWidth));
            nativeSetenv("XASH3D_NATIVE_HEIGHT", String.valueOf(nativeHeight));
            Log.d(TAG, "Using native stretch size: " + nativeWidth + "x" + nativeHeight);
        }
    }

    private boolean hasArgument(String argv, String name) {
        if (argv == null || argv.isEmpty()) {
            return false;
        }

        String[] args = argv.trim().split("\\s+");
        for (String arg : args) {
            if (name.equals(arg)) {
                return true;
            }
        }

        return false;
    }

    private void parseFixedResolution(String argv) {
        mFixedSurfaceWidth = 0;
        mFixedSurfaceHeight = 0;

        if (argv == null || argv.isEmpty()) {
            return;
        }

        String[] args = argv.trim().split("\\s+");
        int width = getIntArgument(args, "-width");
        int height = getIntArgument(args, "-height");

        if (width >= MIN_SURFACE_WIDTH && height >= MIN_SURFACE_HEIGHT) {
            mFixedSurfaceWidth = width;
            mFixedSurfaceHeight = height;
            Log.d(TAG, "Using fixed Android surface size: " + width + "x" + height);
        }
    }

    private int getIntArgument(String[] args, String name) {
        for (int i = 0; i < args.length - 1; i++) {
            if (name.equals(args[i])) {
                try {
                    return Integer.parseInt(args[i + 1]);
                } catch (NumberFormatException e) {
                    Log.w(TAG, "Invalid " + name + " value: " + args[i + 1]);
                    return 0;
                }
            }
        }

        return 0;
    }

    private void applyFixedSurfaceSize() {
        if (mFixedSurfaceWidth <= 0 || mFixedSurfaceHeight <= 0) {
            return;
        }

        ensurePreferences();
        mStretchFixedSurface = getBooleanPreference("stretch_resolution", false);

        // The holder keeps a fixed size across surface destroy/create cycles
        // and the stretch path below is idempotent already: skip redundant
        // applications so onResume/onWindowFocusChanged can't churn the
        // surface mid-startup. On some drivers a resize storm wedges the
        // BLAST queue behind a stale (e.g. portrait) buffer and SDL stays
        // black while audio keeps running.
        if (mFixedSurfaceWidth == mAppliedSurfaceWidth
            && mFixedSurfaceHeight == mAppliedSurfaceHeight
            && mStretchFixedSurface == mAppliedSurfaceStretch) {
            return;
        }

        SurfaceView surfaceView = findSurfaceView(getWindow().getDecorView());
        if (surfaceView == null) {
            Log.w(TAG, "SDL surface not found; fixed resolution will be applied later if possible");
            return;
        }

        if (mStretchFixedSurface) {
            stretchSurfaceToScreen(surfaceView);
        } else {
            surfaceView.getHolder().setFixedSize(mFixedSurfaceWidth, mFixedSurfaceHeight);
        }

        mAppliedSurfaceWidth = mFixedSurfaceWidth;
        mAppliedSurfaceHeight = mFixedSurfaceHeight;
        mAppliedSurfaceStretch = mStretchFixedSurface;
    }

    private void stretchSurfaceToScreen(SurfaceView surfaceView) {
        View view = surfaceView;
        while (view != null) {
            ViewGroup.LayoutParams params = view.getLayoutParams();
            if (params != null && (params.width != ViewGroup.LayoutParams.MATCH_PARENT || params.height != ViewGroup.LayoutParams.MATCH_PARENT)) {
                params.width = ViewGroup.LayoutParams.MATCH_PARENT;
                params.height = ViewGroup.LayoutParams.MATCH_PARENT;
                view.setLayoutParams(params);
            }

            if (!(view.getParent() instanceof View)) {
                break;
            }

            view = (View)view.getParent();
        }

        ViewGroup.LayoutParams params = surfaceView.getLayoutParams();
        if (params != null && (params.width != ViewGroup.LayoutParams.MATCH_PARENT || params.height != ViewGroup.LayoutParams.MATCH_PARENT)) {
            params.width = ViewGroup.LayoutParams.MATCH_PARENT;
            params.height = ViewGroup.LayoutParams.MATCH_PARENT;
            surfaceView.setLayoutParams(params);
        }

        surfaceView.setPivotX(0.0f);
        surfaceView.setPivotY(0.0f);
        surfaceView.setScaleX(1.0f);
        surfaceView.setScaleY(1.0f);
        surfaceView.requestLayout();
    }

    private SurfaceView findSurfaceView(View view) {
        if (view instanceof SurfaceView) {
            return (SurfaceView)view;
        }

        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup)view;
            for (int i = 0; i < group.getChildCount(); i++) {
                SurfaceView surface = findSurfaceView(group.getChildAt(i));
                if (surface != null) {
                    return surface;
                }
            }
        }

        return null;
    }

    private boolean getBooleanPreference(String key, boolean defaultValue) {
        if (mPreferences != null && mPreferences.contains(key)) {
            return mPreferences.getBoolean(key, defaultValue);
        }

        SharedPreferences appPreferences = getSharedPreferences("app_preferences", MODE_PRIVATE);
        if (appPreferences.contains(key)) {
            return appPreferences.getBoolean(key, defaultValue);
        }

        return defaultValue;
    }

    private boolean hasStoredSteamSession() {
        SharedPreferences authPrefs = getSharedPreferences("steam_auth", MODE_PRIVATE);
        String loginKey = authPrefs.getString("login_key", "");
        long steamId = authPrefs.getLong("steam_id", 0L);
        return !loginKey.isEmpty() && steamId != 0L;
    }

    @Override
    protected String[] getArguments() {
        String argv = getFinalArgv();
        parseFixedResolution(argv);
        return argv.isEmpty() ? new String[]{} : argv.split("\\s+");
    }
}
