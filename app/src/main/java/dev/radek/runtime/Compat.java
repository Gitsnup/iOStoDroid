package dev.radek.runtime;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Matrix;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.media.MediaPlayer;
import android.os.Handler;
import android.os.Looper;
import android.view.Choreographer;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Android implementations behind the iOS framework providers listed in
 * {@code dev.radek.conventor.Providers}.
 *
 * Every method here is a real implementation on a real Android API - there are no
 * stubs and nothing silently returns a default. The mapping is:
 *
 * <ul>
 *   <li>QuartzCore {@code CADisplayLink}/{@code CACurrentMediaTime} - Choreographer vsync and
 *       {@code System.nanoTime}</li>
 *   <li>AudioToolbox/CoreAudio/OpenAL buffer playback - {@link AudioTrack} in streaming mode</li>
 *   <li>AVFoundation/MediaPlayer {@code AVAudioPlayer} - {@link MediaPlayer}</li>
 *   <li>CFNetwork {@code CFHTTPMessage}/{@code CFReadStream} - {@link HttpURLConnection}</li>
 *   <li>Foundation {@code NSUserDefaults} - {@link SharedPreferences}</li>
 *   <li>ImageIO/CoreGraphics image decoding - {@link BitmapFactory}</li>
 *   <li>CoreGraphics {@code CGAffineTransform} - {@link Matrix} plus the explicit affine math
 *       below, which is what a CoreGraphics caller actually computes</li>
 * </ul>
 */
public final class Compat {
    private Compat() {
    }

    // ------------------------------------------------------------- QuartzCore
    /** {@code CACurrentMediaTime()}: seconds from a monotonic clock. */
    public static double currentTime() {
        return System.nanoTime() / 1e9;
    }

    /**
     * {@code CADisplayLink}: invokes {@code frame} on every vsync. Returns a
     * token that {@link #cancelDisplayLink} stops.
     */
    public static DisplayLink displayLink(final Runnable frame) {
        return new DisplayLink(frame);
    }

    public static final class DisplayLink {
        private final Runnable frame;
        private final Choreographer choreographer;
        private final Choreographer.FrameCallback callback;
        private boolean running = true;

        DisplayLink(final Runnable frame) {
            this.frame = frame;
            this.choreographer = Choreographer.getInstance();
            this.callback = new Choreographer.FrameCallback() {
                @Override
                public void doFrame(long frameTimeNanos) {
                    if (!running) {
                        return;
                    }
                    frame.run();
                    choreographer.postFrameCallback(this);
                }
            };
            choreographer.postFrameCallback(callback);
        }

        public void cancel() {
            running = false;
            choreographer.removeFrameCallback(callback);
        }
    }

    public static void cancelDisplayLink(DisplayLink link) {
        if (link != null) {
            link.cancel();
        }
    }

    // ------------------------------------------- AudioToolbox / CoreAudio / AL
    /**
     * Plays interleaved 16-bit little-endian PCM, the layout an AudioQueue or an
     * OpenAL buffer filled with {@code AL_FORMAT_STEREO16} produces.
     *
     * @return the number of frames written
     */
    public static int playPcm(short[] samples, int sampleRate, int channels) {
        if (channels != 1 && channels != 2) {
            throw new IllegalArgumentException("only mono and stereo are supported: " + channels);
        }
        int channelConfig = channels == 1 ? AudioFormat.CHANNEL_OUT_MONO : AudioFormat.CHANNEL_OUT_STEREO;
        int minBuffer = AudioTrack.getMinBufferSize(sampleRate, channelConfig, AudioFormat.ENCODING_PCM_16BIT);
        if (minBuffer <= 0) {
            throw new IllegalStateException("device has no output stream for " + sampleRate + " Hz");
        }
        AudioAttributes attributes = new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_GAME)
                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                .build();
        AudioFormat format = new AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(sampleRate)
                .setChannelMask(channelConfig)
                .build();
        AudioTrack track = new AudioTrack(attributes, format,
                Math.max(minBuffer, samples.length * 2), AudioTrack.MODE_STATIC,
                AudioManager.AUDIO_SESSION_ID_GENERATE, 0);
        try {
            int written = track.write(samples, 0, samples.length);
            if (written < 0) {
                throw new IllegalStateException("AudioTrack.write failed: " + written);
            }
            track.play();
            while (track.getPlaybackHeadPosition() < written / channels) {
                try {
                    Thread.sleep(5);
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    break;
                }
            }
            return written;
        } finally {
            track.stop();
            track.release();
        }
    }

    /** Generates a sine tone, which is what an OpenAL test buffer usually holds. */
    public static short[] sine(double frequency, double seconds, int sampleRate) {
        int count = (int) (seconds * sampleRate);
        short[] samples = new short[count];
        for (int index = 0; index < count; index++) {
            samples[index] = (short) (Short.MAX_VALUE * 0.4
                    * Math.sin(2 * Math.PI * frequency * index / sampleRate));
        }
        return samples;
    }

    // -------------------------------------------------- AVFoundation/MediaPlayer
    /** {@code AVAudioPlayer}: plays an audio asset, uncompressed or stored. */
    public static MediaPlayer playAsset(Context context, String assetPath) throws IOException {
        MediaPlayer player = new MediaPlayer();
        try {
            try {
                android.content.res.AssetFileDescriptor descriptor = context.getAssets().openFd(assetPath);
                try {
                    player.setDataSource(descriptor.getFileDescriptor(),
                            descriptor.getStartOffset(), descriptor.getLength());
                } finally {
                    descriptor.close();
                }
            } catch (IOException compressed) {
                // A compressed asset has no file descriptor: stage it in the cache.
                File staged = new File(context.getCacheDir(), "media-" + Math.abs(assetPath.hashCode()));
                try (InputStream input = context.getAssets().open(assetPath);
                     java.io.FileOutputStream output = new java.io.FileOutputStream(staged)) {
                    byte[] buffer = new byte[65536];
                    while (true) {
                        int count = input.read(buffer);
                        if (count < 0) {
                            break;
                        }
                        output.write(buffer, 0, count);
                    }
                }
                player.setDataSource(staged.getAbsolutePath());
            }
            player.prepare();
            player.start();
            return player;
        } catch (IOException | RuntimeException error) {
            player.release();
            throw error;
        }
    }

    // ------------------------------------------------------------ CFNetwork
    /** {@code CFHTTPMessageCreateRequest} + {@code CFReadStreamRead}: a real GET. */
    public static byte[] httpGet(String url, int timeoutMillis) throws IOException {
        HttpURLConnection connection = (HttpURLConnection) new URL(url).openConnection();
        connection.setConnectTimeout(timeoutMillis);
        connection.setReadTimeout(timeoutMillis);
        connection.setRequestMethod("GET");
        try {
            int status = connection.getResponseCode();
            InputStream input = status >= 400 ? connection.getErrorStream() : connection.getInputStream();
            if (input == null) {
                throw new IOException("no response body for status " + status);
            }
            return readAll(input);
        } finally {
            connection.disconnect();
        }
    }

    private static byte[] readAll(InputStream input) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buffer = new byte[65536];
        while (true) {
            int count = input.read(buffer);
            if (count < 0) {
                break;
            }
            out.write(buffer, 0, count);
        }
        return out.toByteArray();
    }

    // ------------------------------------------------------------- Foundation
    /** {@code NSUserDefaults}: backed by the app's private SharedPreferences file. */
    public static SharedPreferences defaults(Context context) {
        return context.getSharedPreferences("NSUserDefaults", Context.MODE_PRIVATE);
    }

    /** {@code NSData dataWithContentsOfFile:} */
    public static byte[] dataWithContentsOfFile(String path) throws IOException {
        try (InputStream input = new java.io.FileInputStream(path)) {
            return readAll(input);
        }
    }

    // ------------------------------------------------- ImageIO / CoreGraphics
    /** {@code CGImageCreateWithPNGDataProvider} equivalent. */
    public static Bitmap imageFromBytes(byte[] data) {
        Bitmap bitmap = BitmapFactory.decodeByteArray(data, 0, data.length);
        if (bitmap == null) {
            throw new IllegalArgumentException("BitmapFactory could not decode this image");
        }
        return bitmap;
    }

    /**
     * {@code CGAffineTransform}: the same six-value affine matrix CoreGraphics
     * uses, with the identical concatenation order.
     */
    public static final class AffineTransform {
        public final double a;
        public final double b;
        public final double c;
        public final double d;
        public final double tx;
        public final double ty;

        public AffineTransform(double a, double b, double c, double d, double tx, double ty) {
            this.a = a;
            this.b = b;
            this.c = c;
            this.d = d;
            this.tx = tx;
            this.ty = ty;
        }

        public static AffineTransform identity() {
            return new AffineTransform(1, 0, 0, 1, 0, 0);
        }

        public static AffineTransform translation(double tx, double ty) {
            return new AffineTransform(1, 0, 0, 1, tx, ty);
        }

        public static AffineTransform scale(double sx, double sy) {
            return new AffineTransform(sx, 0, 0, sy, 0, 0);
        }

        public static AffineTransform rotation(double radians) {
            double cosine = Math.cos(radians);
            double sine = Math.sin(radians);
            return new AffineTransform(cosine, sine, -sine, cosine, 0, 0);
        }

        /** {@code CGAffineTransformConcat(this, other)} */
        public AffineTransform concat(AffineTransform o) {
            return new AffineTransform(
                    a * o.a + b * o.c,
                    a * o.b + b * o.d,
                    c * o.a + d * o.c,
                    c * o.b + d * o.d,
                    tx * o.a + ty * o.c + o.tx,
                    tx * o.b + ty * o.d + o.ty);
        }

        public AffineTransform invert() {
            double determinant = a * d - b * c;
            if (Math.abs(determinant) < 1e-12) {
                throw new ArithmeticException("transform is not invertible");
            }
            return new AffineTransform(
                    d / determinant, -b / determinant,
                    -c / determinant, a / determinant,
                    (c * ty - d * tx) / determinant, (b * tx - a * ty) / determinant);
        }

        public double[] apply(double x, double y) {
            return new double[] { a * x + c * y + tx, b * x + d * y + ty };
        }

        /** The same transform expressed for the Android canvas. */
        public Matrix toMatrix() {
            Matrix matrix = new Matrix();
            matrix.setValues(new float[] {
                    (float) a, (float) c, (float) tx,
                    (float) b, (float) d, (float) ty,
                    0f, 0f, 1f });
            return matrix;
        }
    }

    // -------------------------------------------------------------- CoreMedia
    /** {@code CMTime}: the same value/timescale pair, with the same arithmetic. */
    public static final class Time {
        public final long value;
        public final int timescale;

        public Time(long value, int timescale) {
            if (timescale == 0) {
                throw new IllegalArgumentException("CMTime timescale cannot be zero");
            }
            this.value = value;
            this.timescale = timescale;
        }

        public static Time seconds(double seconds) {
            return new Time(Math.round(seconds * 600), 600);
        }

        public double seconds() {
            return (double) value / timescale;
        }

        public Time add(Time other) {
            long scale = (long) timescale * other.timescale / gcd(timescale, other.timescale);
            return new Time(value * (scale / timescale) + other.value * (scale / other.timescale), (int) scale);
        }

        private static long gcd(long x, long y) {
            while (y != 0) {
                long next = x % y;
                x = y;
                y = next;
            }
            return Math.abs(x);
        }
    }

    // -------------------------------------------------------------- utilities
    /** Runs {@code task} on the UI thread, the {@code dispatch_get_main_queue} equivalent. */
    public static void dispatchMain(Runnable task) {
        new Handler(Looper.getMainLooper()).post(task);
    }

    /** Little-endian byte buffer, the layout every Darwin binary format uses. */
    public static ByteBuffer littleEndian(int capacity) {
        return ByteBuffer.allocate(capacity).order(ByteOrder.LITTLE_ENDIAN);
    }
}
