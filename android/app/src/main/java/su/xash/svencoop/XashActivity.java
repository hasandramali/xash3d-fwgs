package su.xash.svencoop;

import android.annotation.SuppressLint;
import android.content.pm.ActivityInfo;
import android.content.res.AssetManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.preference.PreferenceManager;
import android.content.SharedPreferences;
import android.provider.Settings.Secure;
import android.util.Log;
import android.util.DisplayMetrics;
import android.view.KeyEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

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

    // Black-screen guard: on some devices (MIUI rotation animation,
    // MainActivity portrait -> XashActivity landscape transition) the first
    // SurfaceView surface still carries portrait geometry when the SDL thread
    // boots. SDL2/Android binds the EGL surface to that stale window and
    // never recreates it on resize, so the compositor rejects every frame
    // (BLAST "rejecting buffer ... 986x2176 transform=7") while audio and
    // input keep working. Gate the SDL startup on a settled landscape
    // surface, and if a mid-boot rotation still slipped through, force one
    // surface destroy/create cycle (GONE->VISIBLE) once the rotation has
    // settled. Both are startup-only and fail-open.
    private final Handler mStartupHandler = new Handler(Looper.getMainLooper());
    private boolean mSdlStarted = false;
    private boolean mSurfaceRecovered = false;
    private int mResumeGateTries = 0;
    private static final int RESUME_GATE_MAX_TRIES = 20; // ~2s fail-open
    private static final long RESUME_GATE_INTERVAL_MS = 100;
    private static final long SURFACE_SETTLE_MS = 150;
    private int mLastSurfaceW = 0;
    private int mLastSurfaceH = 0;
    private long mLastSurfaceChangeMs = 0;
    private int mFirstSurfaceW = 0;
    private int mFirstSurfaceH = 0;
    private boolean mSawPortraitSurface = false;

    private final SurfaceHolder.Callback mStartupSurfaceCallback = new SurfaceHolder.Callback() {
        @Override
        public void surfaceCreated(SurfaceHolder holder) {
        }

        @Override
        public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            if (mSurfaceRecovered) {
                return; // boot window over, stop tracking
            }
            if (mFirstSurfaceW == 0 && mFirstSurfaceH == 0) {
                mFirstSurfaceW = width;
                mFirstSurfaceH = height;
            }
            if (width <= height) {
                mSawPortraitSurface = true;
            }
            mLastSurfaceW = width;
            mLastSurfaceH = height;
            mLastSurfaceChangeMs = android.os.SystemClock.uptimeMillis();
        }

        @Override
        public void surfaceDestroyed(SurfaceHolder holder) {
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Request landscape before the window is first traversed so the very
        // first surface is born landscape whenever the system honors it.
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        super.onCreate(savedInstanceState);

        ensurePreferences();

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }

        SurfaceView surfaceView = findSurfaceView(getWindow().getDecorView());
        if (surfaceView != null) {
            surfaceView.getHolder().addCallback(mStartupSurfaceCallback);
        }

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
        if (!mSdlStarted && !isLandscapeSurfaceSettled()) {
            if (mResumeGateTries < RESUME_GATE_MAX_TRIES) {
                mResumeGateTries++;
                mStartupHandler.postDelayed(new Runnable() {
                    @Override
                    public void run() {
                        onResume();
                    }
                }, RESUME_GATE_INTERVAL_MS);
                return;
            }
            Log.w(TAG, "landscape surface did not settle in time; starting SDL anyway (stale-surface recovery will handle it)");
        }
        mSdlStarted = true;
        super.onResume();
        applyFixedSurfaceSize();
        // Late enough that native video init is done even on slow devices,
        // early enough that the user is still in the menu.
        mStartupHandler.postDelayed(new Runnable() {
            @Override
            public void run() {
                maybeRecoverStaleSurface();
            }
        }, 2500);
    }

    @Override
    protected void onPause() {
        mStartupHandler.removeCallbacksAndMessages(null);
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        mStartupHandler.removeCallbacksAndMessages(null);
        SurfaceView surfaceView = findSurfaceView(getWindow().getDecorView());
        if (surfaceView != null) {
            try {
                surfaceView.getHolder().removeCallback(mStartupSurfaceCallback);
            } catch (Throwable ignored) {
            }
        }
        super.onDestroy();
        System.exit(0);
    }

    private boolean isLandscapeSurfaceSettled() {
        if (mLastSurfaceW <= 0 || mLastSurfaceH <= 0) {
            return false; // no surface yet
        }
        if (mLastSurfaceW <= mLastSurfaceH) {
            return false; // still portrait: rotation in flight
        }
        int rotation = getWindowManager().getDefaultDisplay().getRotation();
        if (rotation != Surface.ROTATION_90 && rotation != Surface.ROTATION_270) {
            return false;
        }
        return android.os.SystemClock.uptimeMillis() - mLastSurfaceChangeMs >= SURFACE_SETTLE_MS;
    }

    private void maybeRecoverStaleSurface() {
        if (mSurfaceRecovered) {
            return;
        }
        mSurfaceRecovered = true;

        boolean rotatedDuringBoot = mSawPortraitSurface
            || (mFirstSurfaceW > 0 && (mFirstSurfaceW != mLastSurfaceW || mFirstSurfaceH != mLastSurfaceH));
        if (!rotatedDuringBoot) {
            return; // clean boot, nothing to do
        }

        final SurfaceView surfaceView = findSurfaceView(getWindow().getDecorView());
        if (surfaceView == null) {
            return;
        }

        // A surface destroy/create cycle forces SDL2/Android to drop the
        // stale (portrait) EGL surface and bind a fresh landscape one. The
        // EGL context (and all textures) survive; this is the same path as a
        // normal runtime rotation, just triggered once at startup.
        Log.w(TAG, "mid-boot rotation detected (" + mFirstSurfaceW + "x" + mFirstSurfaceH
            + " -> " + mLastSurfaceW + "x" + mLastSurfaceH + "); recreating surface once to fix stale EGL");
        surfaceView.setVisibility(View.GONE);
        mStartupHandler.postDelayed(new Runnable() {
            @Override
            public void run() {
                surfaceView.setVisibility(View.VISIBLE);
            }
        }, 250);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            applyFixedSurfaceSize();
        }
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
