package su.xash.svencoop.util;

import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.media.audiofx.AcousticEchoCanceler;
import android.media.audiofx.AudioEffect;
import android.media.audiofx.AutomaticGainControl;
import android.media.audiofx.NoiseSuppressor;
import android.util.Log;
import org.libsdl.app.SDLAudioManager;
import java.util.ArrayList;

public final class VoiceAudioCapture extends SDLAudioManager {
    private static final String TAG = "VoiceAudioCapture";
    private static final ArrayList<AudioEffect> effects = new ArrayList<>();

    private static void configureEffect(AudioEffect effect, boolean enabled) {
        if (effect == null) return;
        effects.add(effect);
        if (effect.setEnabled(enabled) != AudioEffect.SUCCESS) {
            Log.w(TAG, "Voice effect could not be configured");
        }
    }

    private static void configureEffects(int session) {
        try {
            if (NoiseSuppressor.isAvailable()) configureEffect(NoiseSuppressor.create(session), true);
        } catch (RuntimeException error) {
            Log.w(TAG, "Noise suppression is unavailable", error);
        }
        try {
            if (AcousticEchoCanceler.isAvailable()) configureEffect(AcousticEchoCanceler.create(session), true);
        } catch (RuntimeException error) {
            Log.w(TAG, "Echo cancellation is unavailable", error);
        }
        try {
            if (AutomaticGainControl.isAvailable()) configureEffect(AutomaticGainControl.create(session), false);
        } catch (RuntimeException error) {
            Log.w(TAG, "Hardware gain control could not be disabled", error);
        }
    }

    public static synchronized boolean prepare(int sampleRate, int frames) {
        releaseCapture();
        if (sampleRate <= 0 || frames <= 0 || frames > 4096) return false;
        try {
            int minimum = AudioRecord.getMinBufferSize(sampleRate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
            if (minimum <= 0) return false;
            mAudioRecord = new AudioRecord(MediaRecorder.AudioSource.VOICE_COMMUNICATION, sampleRate,
                    AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, Math.max(minimum, frames * 8));
            if (mAudioRecord.getState() != AudioRecord.STATE_INITIALIZED) {
                releaseCapture();
                return false;
            }
            configureEffects(mAudioRecord.getAudioSessionId());
            mAudioRecord.startRecording();
            if (mAudioRecord.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) {
                releaseCapture();
                return false;
            }
            Log.i(TAG, "VoIP microphone enabled at " + mAudioRecord.getSampleRate() + " Hz");
            return true;
        } catch (RuntimeException error) {
            Log.w(TAG, "VoIP capture unavailable; falling back to SDL capture", error);
            releaseCapture();
            return false;
        }
    }

    public static synchronized void releaseCapture() {
        for (AudioEffect effect : effects) {
            try {
                effect.release();
            } catch (RuntimeException error) {
                Log.w(TAG, "Could not release voice effect", error);
            }
        }
        effects.clear();
        if (mAudioRecord != null) {
            try {
                if (mAudioRecord.getRecordingState() == AudioRecord.RECORDSTATE_RECORDING) mAudioRecord.stop();
            } catch (RuntimeException error) {
                Log.w(TAG, "Could not stop microphone", error);
            } finally {
                try {
                    mAudioRecord.release();
                } catch (RuntimeException error) {
                    Log.w(TAG, "Could not release microphone", error);
                } finally {
                    mAudioRecord = null;
                }
            }
        }
    }
}
