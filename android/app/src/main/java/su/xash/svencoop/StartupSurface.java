package su.xash.svencoop;

import android.app.Activity;
import android.content.Context;
import android.graphics.Bitmap;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.View;
import org.libsdl.app.SDLSurface;

/** Defer surface creation, never SDL's SurfaceHolder callbacks. */
final class StartupSurface extends SDLSurface {
    private static final long BLACK_CHECK_DELAY_MS = 3000;
    private static final long BLACK_CHECK_TIMEOUT_MS = 4000;
    private static final int BLACK_CHECK_SIZE = 48;
    private static final int BLACK_CHECK_MAX_RECOVERIES = 2;

    private int lastRotation = -1;
    private int stableFrames;
    private int lastWidth, lastHeight;
    private boolean released;
    private boolean scheduled;
    private boolean checkDone;
    private int retries;
    private int recoveries;

    StartupSurface(Context context) {
        super(context);
        // INVISIBLE still participates in layout, but has no native surface.
        // Delaying surfaceChanged alone leaves Android's surface transaction
        // and SDL's native window initialization on different timelines.
        setVisibility(View.INVISIBLE);
    }

    private final Runnable checkSurface = new Runnable() {
        @Override public void run() {
            scheduled = false;
            if (released) return;
            Activity activity = (Activity)getContext();
            boolean multiWindow = Build.VERSION.SDK_INT >= 24 && activity.isInMultiWindowMode();
            int rotation = mDisplay.getRotation();
            if (!hasWindowFocus() || (!multiWindow && getWidth() < getHeight())
                    || getWidth() <= 0 || getHeight() <= 0 || isLayoutRequested()
                    || rotation != lastRotation || getWidth() != lastWidth || getHeight() != lastHeight) {
                stableFrames = 0;
                lastRotation = rotation;
                lastWidth = getWidth();
                lastHeight = getHeight();
            } else {
                stableFrames++;
            }
            if (stableFrames < 3) {
                scheduleCheck();
                return;
            }
            released = true;
            Log.i("XashActivity", "Creating SDL surface on settled layout "
                    + getWidth() + "x" + getHeight() + " rotation=" + rotation);
            // Android now dispatches surfaceCreated/surfaceChanged normally,
            // including the native window's final rotation and buffer geometry.
            setVisibility(View.VISIBLE);
            scheduleBlackCheck();
        }
    };

    /**
     * One-shot black-presentation watchdog. On some drivers the first EGL
     * present queue comes up wedged: native renders into nothing (black
     * screen, silence, ANR input starvation) while engine logs look healthy,
     * and only a video re-init (e.g. map load) clears it. A few seconds after
     * startup, screenshot the composed SurfaceView output: if it is pure
     * black, force SDL to rebind EGL ("sdl refresh"); if still black, restart
     * the activity rather than leaving a dead screen. Runs on the UI thread,
     * independent of the (possibly wedged) GL thread.
     */
    private final Runnable blackCheckTimeout = new Runnable() {
        @Override public void run() {
            if (checkDone || !released) return;
            Log.w("XashActivity", "Black-presentation check timed out, treating as wedged");
            checkDone = true;
            recoverFromBlack();
        }
    };

    private final Runnable blackCheck = new Runnable() {
        @Override public void run() {
            if (checkDone || !released) return;
            if (!hasWindowFocus()) {
                // Backgrounded: nothing visible to judge, and recovery would
                // only disrupt a user who switched away. Skip quietly.
                Log.i("XashActivity", "No window focus; skipping black-presentation check");
                checkDone = true;
                return;
            }
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.N) {
                Log.i("XashActivity", "Black-presentation check needs API 24+; skipping");
                checkDone = true;
                return;
            }
            SurfaceHolder holder = getHolder();
            if (holder == null || holder.getSurface() == null || !holder.getSurface().isValid()) {
                if (retries++ < 1) {
                    Log.i("XashActivity", "Surface not ready for black-presentation check; retrying once");
                    postDelayed(blackCheck, 2000);
                } else {
                    Log.i("XashActivity", "Surface still not ready; skipping black-presentation check");
                    checkDone = true;
                }
                return;
            }
            final Bitmap bitmap = Bitmap.createBitmap(BLACK_CHECK_SIZE, BLACK_CHECK_SIZE, Bitmap.Config.ARGB_8888);
            if (!requestPixelCopy(holder.getSurface(), bitmap)) {
                Log.i("XashActivity", "Black-presentation copy unavailable; skipping check");
                bitmap.recycle();
                checkDone = true;
                return;
            }
            postDelayed(blackCheckTimeout, BLACK_CHECK_TIMEOUT_MS);
        }
    };

    /**
     * Screenshots the surface via android.graphics.PixelCopy using reflection,
     * so this file still compiles against SDKs predating API 24 (the caller
     * already guards by SDK_INT, and anything missing degrades to "skip").
     * Returns false when the copy could not even be started.
     */
    private boolean requestPixelCopy(Surface surface, final Bitmap bitmap) {
        try {
            final Class<?> pixelCopyClass = Class.forName("android.graphics.PixelCopy");
            final Class<?> listenerClass =
                    Class.forName("android.graphics.PixelCopy$OnPixelCopyFinishedListener");
            final int success = pixelCopyClass.getField("SUCCESS").getInt(null);
            Object listener = java.lang.reflect.Proxy.newProxyInstance(
                    getClass().getClassLoader(),
                    new Class<?>[]{ listenerClass },
                    new java.lang.reflect.InvocationHandler() {
                        @Override public Object invoke(Object proxy, java.lang.reflect.Method method, Object[] args) {
                            if (method != null && "onPixelCopyFinished".equals(method.getName())) {
                                onPixelCopyResult(args != null && args.length > 0 && args[0] instanceof Integer
                                        ? (Integer) args[0] : Integer.MIN_VALUE, success, bitmap);
                            }
                            return null;
                        }
                    });
            pixelCopyClass
                    .getMethod("request", Surface.class, Bitmap.class, listenerClass, Handler.class)
                    .invoke(null, surface, bitmap, listener, new Handler(Looper.getMainLooper()));
            return true;
        } catch (Exception e) {
            Log.i("XashActivity", "PixelCopy unavailable (" + e.getClass().getSimpleName() + "); skipping check");
            return false;
        }
    }

    private void onPixelCopyResult(int copyResult, int success, Bitmap bitmap) {
        removeCallbacks(blackCheckTimeout);
        if (checkDone) {
            bitmap.recycle();
            return;
        }
        checkDone = true;
        if (copyResult != success) {
            Log.w("XashActivity", "Black-presentation copy failed (" + copyResult + "), treating as wedged");
            bitmap.recycle();
            recoverFromBlack();
            return;
        }
        boolean allBlack = true;
        for (int y = 0; y < bitmap.getHeight() && allBlack; y++) {
            for (int x = 0; x < bitmap.getWidth(); x++) {
                if ((bitmap.getPixel(x, y) & 0x00FFFFFF) != 0) {
                    allBlack = false;
                    break;
                }
            }
        }
        bitmap.recycle();
        if (!allBlack) {
            Log.i("XashActivity", "Black-presentation check passed");
            return;
        }
        Log.w("XashActivity", "Black presentation detected, recovering");
        recoverFromBlack();
    }

    private void scheduleBlackCheck() {
        if (checkDone) return;
        postDelayed(blackCheck, BLACK_CHECK_DELAY_MS);
    }

    private void recoverFromBlack() {
        if (recoveries >= BLACK_CHECK_MAX_RECOVERIES) {
            Log.e("XashActivity", "Black presentation persists, giving up automatic recovery");
            return;
        }
        recoveries++;
        if (recoveries == 1) {
            // "SDL refresh": destroy/recreate the Surface so SDL rebinds EGL.
            Log.w("XashActivity", "Toggling surface visibility to rebind EGL (attempt 1/2)");
            setVisibility(View.GONE);
            postDelayed(new Runnable() {
                @Override public void run() {
                    if (!released) return;
                    setVisibility(View.VISIBLE);
                    checkDone = false;
                    postDelayed(blackCheck, BLACK_CHECK_DELAY_MS);
                }
            }, 500);
            return;
        }
        Activity activity = (Activity)getContext();
        Log.w("XashActivity", "Recreating activity to clear black presentation (attempt 2/2)");
        activity.recreate();
    }

    private void scheduleCheck() {
        if (!scheduled) {
            scheduled = true;
            postOnAnimation(checkSurface);
        }
    }

    @Override protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        if (!released) scheduleCheck();
    }

    @Override protected void onDetachedFromWindow() {
        removeCallbacks(checkSurface);
        removeCallbacks(blackCheck);
        removeCallbacks(blackCheckTimeout);
        scheduled = false;
        stableFrames = 0;
        lastRotation = -1;
        super.onDetachedFromWindow();
    }
}
