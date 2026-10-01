package su.xash.svencoop;

import android.app.Activity;
import android.content.Context;
import android.os.Build;
import android.util.Log;
import android.view.SurfaceHolder;
import org.libsdl.app.SDLSurface;

/** Start SDL only after the fullscreen surface and rotation have settled. */
final class StartupSurface extends SDLSurface {
    private SurfaceHolder pendingHolder;
    private int pendingFormat, pendingWidth, pendingHeight;
    private int lastRotation = -1;
    private int stableFrames;
    private boolean started;
    private boolean scheduled;

    StartupSurface(Context context) {
        super(context);
    }

    private final Runnable checkSurface = new Runnable() {
        @Override public void run() {
            scheduled = false;
            SurfaceHolder holder = pendingHolder;
            if (holder == null || !holder.getSurface().isValid()) return;

            Activity activity = (Activity)getContext();
            boolean multiWindow = Build.VERSION.SDK_INT >= 24 && activity.isInMultiWindowMode();
            int rotation = mDisplay.getRotation();
            // Focus and a valid landscape layout are prerequisites, not timers.
            // Check across display frames so EGL cannot latch the launcher's
            // portrait/pre-fullscreen buffer transform on the SDL thread.
            if (!hasWindowFocus() || (!multiWindow && getWidth() < getHeight())
                    || getWidth() <= 0 || getHeight() <= 0 || isLayoutRequested()
                    || rotation != lastRotation) {
                stableFrames = 0;
                lastRotation = rotation;
            } else {
                stableFrames++;
            }
            if (stableFrames < 3) {
                scheduleCheck();
                return;
            }
            pendingHolder = null;
            Log.i("XashActivity", "Starting SDL on settled surface "
                    + pendingWidth + "x" + pendingHeight + " rotation=" + rotation);
            StartupSurface.super.surfaceChanged(holder, pendingFormat, pendingWidth, pendingHeight);
            started = mIsSurfaceReady;
        }
    };

    private void scheduleCheck() {
        if (!scheduled) {
            scheduled = true;
            postOnAnimation(checkSurface);
        }
    }

    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        if (started) {
            super.surfaceChanged(holder, format, width, height);
            return;
        }
        pendingHolder = holder;
        pendingFormat = format;
        pendingWidth = width;
        pendingHeight = height;
        stableFrames = 0;
        scheduleCheck();
    }

    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        removeCallbacks(checkSurface);
        scheduled = false;
        pendingHolder = null;
        stableFrames = 0;
        lastRotation = -1;
        // Once SDL has started, retain its normal pause/resume/context lifecycle.
        super.surfaceDestroyed(holder);
    }
}
