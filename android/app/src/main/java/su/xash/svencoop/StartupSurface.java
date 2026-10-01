package su.xash.svencoop;

import android.app.Activity;
import android.content.Context;
import android.os.Build;
import android.util.Log;
import android.view.View;
import org.libsdl.app.SDLSurface;

/** Defer surface creation, never SDL's SurfaceHolder callbacks. */
final class StartupSurface extends SDLSurface {
    private int lastRotation = -1;
    private int stableFrames;
    private int lastWidth, lastHeight;
    private boolean released;
    private boolean scheduled;

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
        }
    };

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
        scheduled = false;
        stableFrames = 0;
        lastRotation = -1;
        super.onDetachedFromWindow();
    }
}
