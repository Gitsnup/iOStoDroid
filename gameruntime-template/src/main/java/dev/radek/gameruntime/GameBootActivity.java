package dev.radek.gameruntime;

import android.app.Activity;
import android.content.res.AssetManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;
import java.util.zip.CRC32;
import java.util.zip.DataFormatException;
import java.util.zip.Inflater;

/**
 * On-device launcher for a game-runtime boot-attempt APK (contract "game-runtime-v1").
 *
 * The activity renders the game's recovered splash screen (supporting standard PNG/JPEG,
 * Apple CgBI-encoded launch images, and sprite-sheet splash descriptors such as
 * SPLASHES.png + SPLASHES.dat) in a visual viewport while staging the packaged 32-bit
 * ARM Mach-O executable and bundle resources on a worker thread and calling the native
 * compatibility runtime once through JNI.
 */
public final class GameBootActivity extends Activity {
    private static final String METADATA_ASSET = "gameboot.json";
    private static final String EXECUTABLE_ASSET = "gameboot/main-executable.bin";
    private static final String BUNDLE_ASSET_PREFIX = "bundle";
    private static final long MAX_METADATA_BYTES = 16L * 1024 * 1024;
    private static final long MAX_EXECUTABLE_BYTES = 256L * 1024 * 1024;
    private static final long MAX_SINGLE_ASSET_BYTES = 1024L * 1024 * 1024;
    private static final int MAX_SPLASH_IMAGE_BYTES = 16 * 1024 * 1024;
    private static final int MAX_SPLASH_DIMENSION = 4096;
    private static final int MAX_CGBI_INFLATED_BYTES = 16 * 1024 * 1024;
    private static final int DEFAULT_VIEWPORT_WIDTH = 480;
    private static final int DEFAULT_VIEWPORT_HEIGHT = 320;
    private static final byte[] PNG_SIGNATURE = new byte[] {
        (byte) 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
    };
    private static final byte[] CGBI_CHUNK_TYPE = new byte[] { 0x43, 0x67, 0x42, 0x49 };

    private static volatile boolean nativeLoaded;
    private static volatile String nativeLoadError = "";

    static {
        try {
            // Explicit dependency order avoids OEM linkers that do not search the
            // app's own lib directory when resolving a transitive DT_NEEDED.
            System.loadLibrary("unicorn");
            System.loadLibrary("compat_runtime_v1");
            nativeLoaded = true;
        } catch (Throwable error) {
            nativeLoaded = false;
            nativeLoadError = summarizeNativeError(error);
        }
    }

    private static native String runGameBootAttempt(String executablePath, String bundleRootPath);

    private TextView statusView;
    private TextView reportView;
    private ImageView splashImageView;
    private TextView splashCaptionView;
    private final List<SplashFrame> activeSplashFrames = new ArrayList<>();
    private int currentSplashIndex = 0;

    static final class BootSummary {
        final String headline;
        final String status;
        final String details;
        final boolean failedClosed;

        BootSummary(String headline, String status, String details, boolean failedClosed) {
            this.headline = headline;
            this.status = status;
            this.details = details;
            this.failedClosed = failedClosed;
        }
    }

    /** Descriptor for a single sprite region inside a game splash sprite sheet (e.g. SPLASHES.dat). */
    public static final class SplashSpriteEntry {
        public final String name;
        public final int x;
        public final int y;
        public final int width;
        public final int height;
        public final int pivotX;
        public final int pivotY;

        public SplashSpriteEntry(
                String name,
                int x,
                int y,
                int width,
                int height,
                int pivotX,
                int pivotY) {
            this.name = name;
            this.x = x;
            this.y = y;
            this.width = width;
            this.height = height;
            this.pivotX = pivotX;
            this.pivotY = pivotY;
        }
    }

    /** Parsed representation of a binary sprite-sheet descriptor such as SPLASHES.dat. */
    public static final class SplashSheetDescriptor {
        public final String sheetName;
        public final List<SplashSpriteEntry> entries;

        public SplashSheetDescriptor(String sheetName, List<SplashSpriteEntry> entries) {
            this.sheetName = sheetName;
            this.entries = Collections.unmodifiableList(new ArrayList<>(entries));
        }
    }

    private static final class SplashFrame {
        final String label;
        final String sourcePath;
        final Bitmap bitmap;
        final int width;
        final int height;

        SplashFrame(String label, String sourcePath, Bitmap bitmap, int width, int height) {
            this.label = label;
            this.sourcePath = sourcePath;
            this.bitmap = bitmap;
            this.width = width;
            this.height = height;
        }
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().setStatusBarColor(Color.rgb(11, 16, 29));
        getWindow().setNavigationBarColor(Color.rgb(11, 16, 29));

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(Color.rgb(11, 16, 29));

        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setPadding(dp(20), dp(28), dp(20), dp(28));

        TextView titleView = new TextView(this);
        titleView.setText("Game-runtime boot attempt");
        titleView.setTextSize(22f);
        titleView.setTypeface(Typeface.DEFAULT_BOLD);
        titleView.setTextColor(Color.WHITE);
        column.addView(titleView);

        TextView subtitleView = new TextView(this);
        subtitleView.setText(
                "Renders the recovered bundle splash viewport and executes the packaged 32-bit ARM "
                        + "Mach-O binary until the first unimplemented import.");
        subtitleView.setTextSize(13f);
        subtitleView.setTextColor(Color.rgb(160, 178, 199));
        subtitleView.setPadding(0, dp(6), 0, dp(14));
        column.addView(subtitleView);

        LinearLayout splashCard = new LinearLayout(this);
        splashCard.setOrientation(LinearLayout.VERTICAL);
        GradientDrawable cardBg = new GradientDrawable();
        cardBg.setColor(Color.rgb(17, 25, 43));
        cardBg.setCornerRadius(dp(12));
        cardBg.setStroke(dp(1), Color.rgb(38, 56, 89));
        splashCard.setBackground(cardBg);
        splashCard.setPadding(dp(12), dp(12), dp(12), dp(12));

        TextView splashTitle = new TextView(this);
        splashTitle.setText("GAME SPLASH SCREEN VIEWPORT");
        splashTitle.setTextSize(11f);
        splashTitle.setTypeface(Typeface.DEFAULT_BOLD);
        splashTitle.setTextColor(Color.rgb(122, 184, 255));
        splashTitle.setPadding(0, 0, 0, dp(8));
        splashCard.addView(splashTitle);

        FrameLayout viewportContainer = new FrameLayout(this);
        GradientDrawable viewportBg = new GradientDrawable();
        viewportBg.setColor(Color.rgb(6, 9, 16));
        viewportBg.setCornerRadius(dp(8));
        viewportBg.setStroke(dp(1), Color.rgb(28, 42, 68));
        viewportContainer.setBackground(viewportBg);
        viewportContainer.setMinimumHeight(dp(210));

        splashImageView = new ImageView(this);
        splashImageView.setAdjustViewBounds(true);
        splashImageView.setMaxHeight(dp(260));
        splashImageView.setMinimumHeight(dp(200));
        splashImageView.setScaleType(ImageView.ScaleType.FIT_CENTER);
        FrameLayout.LayoutParams imageParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.CENTER);
        viewportContainer.addView(splashImageView, imageParams);

        viewportContainer.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                cycleSplashFrame();
            }
        });
        splashCard.addView(
                viewportContainer,
                new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT));

        splashCaptionView = new TextView(this);
        splashCaptionView.setText("Loading bundle splash screen assets…");
        splashCaptionView.setTextSize(11.5f);
        splashCaptionView.setTextColor(Color.rgb(160, 178, 199));
        splashCaptionView.setPadding(0, dp(8), 0, 0);
        splashCard.addView(splashCaptionView);

        LinearLayout.LayoutParams splashCardParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        splashCardParams.bottomMargin = dp(14);
        column.addView(splashCard, splashCardParams);

        statusView = new TextView(this);
        statusView.setText("Staging bundle and starting guest boot attempt…");
        statusView.setTextSize(14f);
        statusView.setTypeface(Typeface.DEFAULT_BOLD);
        statusView.setTextColor(Color.rgb(255, 209, 102));
        statusView.setPadding(0, 0, 0, dp(12));
        column.addView(statusView);

        reportView = new TextView(this);
        reportView.setTextSize(12f);
        reportView.setTypeface(Typeface.MONOSPACE);
        reportView.setTextColor(Color.rgb(222, 231, 245));
        reportView.setBackgroundColor(Color.rgb(18, 26, 44));
        reportView.setPadding(dp(14), dp(14), dp(14), dp(14));
        column.addView(
                reportView,
                new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT));

        scroll.addView(column);
        setContentView(scroll);

        // Immediately load a preliminary splash frame from packaged assets if available
        // so the user sees the game visual before full bundle extraction completes.
        preloadSplashFromAssets();

        Thread worker = new Thread(this::runBootAttemptOnWorker, "radek-gameboot");
        worker.setDaemon(true);
        worker.start();
    }

    private void preloadSplashFromAssets() {
        try {
            List<SplashFrame> preloaded = discoverSplashFramesFromAssets(getAssets());
            if (!preloaded.isEmpty()) {
                installSplashFrames(preloaded, true);
            }
        } catch (Throwable ignored) {
            // Worker thread will perform full bundle discovery after staging.
        }
    }

    private void installSplashFrames(List<SplashFrame> frames, boolean playBootSequence) {
        if (frames == null || frames.isEmpty()) return;
        activeSplashFrames.clear();
        activeSplashFrames.addAll(frames);
        currentSplashIndex = 0;
        showSplashFrame(0);
        if (playBootSequence && activeSplashFrames.size() >= 3) {
            // If the sprite sheet provides studio/publisher splashes alongside the main
            // game splash (e.g. Rovio -> Clickgamer -> Angry Birds), briefly transition
            // through them and settle on the primary game splash at index 0.
            final int total = activeSplashFrames.size();
            showSplashFrame(Math.min(1, total - 1));
            splashImageView.postDelayed(new Runnable() {
                @Override
                public void run() {
                    if (activeSplashFrames.size() >= 3) {
                        showSplashFrame(2);
                    }
                }
            }, 350L);
            splashImageView.postDelayed(new Runnable() {
                @Override
                public void run() {
                    if (!activeSplashFrames.isEmpty()) {
                        showSplashFrame(0);
                    }
                }
            }, 750L);
        }
    }

    private void showSplashFrame(int index) {
        if (activeSplashFrames.isEmpty()) return;
        currentSplashIndex = ((index % activeSplashFrames.size()) + activeSplashFrames.size())
                % activeSplashFrames.size();
        SplashFrame frame = activeSplashFrames.get(currentSplashIndex);
        splashImageView.setImageBitmap(frame.bitmap);
        String cycleHint = activeSplashFrames.size() > 1
                ? " · Frame " + (currentSplashIndex + 1) + "/" + activeSplashFrames.size()
                        + " (tap viewport to cycle)"
                : "";
        splashCaptionView.setText(
                "Rendered splash: " + frame.sourcePath + " [" + frame.label + " "
                        + frame.width + "×" + frame.height + "]" + cycleHint);
    }

    private void cycleSplashFrame() {
        if (activeSplashFrames.size() <= 1) return;
        showSplashFrame(currentSplashIndex + 1);
    }

    private void runBootAttemptOnWorker() {
        try {
            JSONObject manifest = readManifest();
            String appName = manifest.optString("applicationName", "Game-runtime boot attempt");
            if (!nativeLoaded) {
                BootSummary summary = new BootSummary(
                        appName,
                        "BOOT FAILED CLOSED: native compatibility runtime could not be loaded",
                        "Native load error: " + nativeLoadError + "\n\n"
                                + formatManifestHeader(manifest),
                        true);
                publish(summary);
                return;
            }
            File workRoot = new File(getCacheDir(), "gameboot-runtime");
            deleteRecursively(workRoot);
            File bundleDir = new File(workRoot, "bundle");
            if (!bundleDir.mkdirs() && !bundleDir.isDirectory()) {
                throw new IOException("Could not create the staged bundle directory");
            }
            File executable = new File(workRoot, "main-executable.bin");
            extractAssetToFile(EXECUTABLE_ASSET, executable, MAX_EXECUTABLE_BYTES);
            stageBundleAssets(manifest, bundleDir);

            final List<SplashFrame> discoveredFrames = discoverSplashFramesFromBundle(bundleDir);
            if (!discoveredFrames.isEmpty()) {
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        if (activeSplashFrames.isEmpty()
                                || discoveredFrames.size() > activeSplashFrames.size()) {
                            installSplashFrames(discoveredFrames, activeSplashFrames.isEmpty());
                        }
                    }
                });
            }

            String rawJson = runGameBootAttempt(
                    executable.getAbsolutePath(),
                    bundleDir.getAbsolutePath());
            BootSummary summary = summarizeBootReport(appName, manifest, rawJson);
            publish(summary);
        } catch (Throwable error) {
            BootSummary summary = new BootSummary(
                    "Game-runtime boot attempt",
                    "BOOT FAILED CLOSED: " + summarizeNativeError(error),
                    "The boot attempt stopped before guest execution completed.\n"
                            + "Reason: " + summarizeNativeError(error),
                    true);
            publish(summary);
        }
    }

    private void publish(BootSummary summary) {
        runOnUiThread(() -> {
            setTitle(summary.headline);
            statusView.setText(summary.status);
            statusView.setTextColor(
                    summary.failedClosed
                            ? Color.rgb(255, 138, 128)
                            : Color.rgb(92, 227, 181));
            reportView.setText(summary.details);
        });
    }

    /**
     * Parses a binary sprite-sheet descriptor (`SPLASHES.dat` format: big-endian length-prefixed
     * texture filename, big-endian entry count, followed by length-prefixed sprite names and
     * 6 big-endian uint16 coordinates: x, y, width, height, pivotX, pivotY).
     */
    public static SplashSheetDescriptor parseSplashSheetDescriptor(byte[] data) {
        if (data == null || data.length < 6) return null;
        int offset = 0;
        int sheetNameLen = readU16Be(data, offset);
        offset += 2;
        if (sheetNameLen <= 0 || sheetNameLen > 256 || offset + sheetNameLen + 2 > data.length) {
            return null;
        }
        String sheetName = new String(data, offset, sheetNameLen, StandardCharsets.US_ASCII);
        offset += sheetNameLen;
        int entryCount = readU16Be(data, offset);
        offset += 2;
        if (entryCount <= 0 || entryCount > 512) return null;

        List<SplashSpriteEntry> entries = new ArrayList<>(entryCount);
        for (int i = 0; i < entryCount; i++) {
            if (offset + 2 > data.length) return null;
            int nameLen = readU16Be(data, offset);
            offset += 2;
            if (nameLen <= 0 || nameLen > 256 || offset + nameLen + 12 > data.length) {
                return null;
            }
            String name = new String(data, offset, nameLen, StandardCharsets.US_ASCII);
            offset += nameLen;
            int x = readU16Be(data, offset);
            int y = readU16Be(data, offset + 2);
            int width = readU16Be(data, offset + 4);
            int height = readU16Be(data, offset + 6);
            int pivotX = readU16Be(data, offset + 8);
            int pivotY = readU16Be(data, offset + 10);
            offset += 12;
            if (width <= 0 || height <= 0 || width > MAX_SPLASH_DIMENSION || height > MAX_SPLASH_DIMENSION) {
                return null;
            }
            entries.add(new SplashSpriteEntry(name, x, y, width, height, pivotX, pivotY));
        }
        return new SplashSheetDescriptor(sheetName, entries);
    }

    /** Returns true if the byte array is an Apple CgBI PNG image. */
    public static boolean isCgbiPng(byte[] data) {
        if (data == null || data.length < 33 || data.length > MAX_SPLASH_IMAGE_BYTES) return false;
        for (int i = 0; i < PNG_SIGNATURE.length; i++) {
            if (data[i] != PNG_SIGNATURE[i]) return false;
        }
        int limit = Math.min(data.length, 512) - CGBI_CHUNK_TYPE.length;
        for (int i = 8; i <= limit; i++) {
            boolean match = true;
            for (int j = 0; j < CGBI_CHUNK_TYPE.length; j++) {
                if (data[i + j] != CGBI_CHUNK_TYPE[j]) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
        return false;
    }

    /**
     * Decodes an Apple CgBI 8-bit RGB/RGBA PNG into straight-alpha ARGB_8888 pixels.
     * Writes {width, height} into outDimensions (length >= 2).
     */
    public static int[] decodeCgbiRgbaPixels(byte[] data, int[] outDimensions) {
        if (!isCgbiPng(data) || outDimensions == null || outDimensions.length < 2) {
            throw new IllegalArgumentException("Invalid CgBI PNG input");
        }
        int p = PNG_SIGNATURE.length;
        int width = 0;
        int height = 0;
        int depth = 0;
        int colorType = -1;
        boolean sawHeader = false;
        boolean sawEnd = false;
        ByteArrayOutputStream compressed = new ByteArrayOutputStream();

        while (p < data.length) {
            if (data.length - p < 12) throw new IllegalArgumentException("Truncated PNG chunk");
            long chunkLenLong = readU32Be(data, p);
            if (chunkLenLong < 0 || chunkLenLong > data.length - p - 12L) {
                throw new IllegalArgumentException("PNG chunk out of bounds");
            }
            int chunkLen = (int) chunkLenLong;
            int typeOffset = p + 4;
            int payloadOffset = p + 8;
            int crcOffset = payloadOffset + chunkLen;
            String type = new String(data, typeOffset, 4, StandardCharsets.US_ASCII);
            if (!"CgBI".equals(type)) {
                CRC32 crc = new CRC32();
                crc.update(data, typeOffset, 4 + chunkLen);
                if (crc.getValue() != readU32Be(data, crcOffset)) {
                    throw new IllegalArgumentException("Invalid PNG chunk CRC for " + type);
                }
            }
            if ("IHDR".equals(type)) {
                if (sawHeader || chunkLen != 13) throw new IllegalArgumentException("Invalid IHDR");
                width = (int) readU32Be(data, payloadOffset);
                height = (int) readU32Be(data, payloadOffset + 4);
                depth = data[payloadOffset + 8] & 0xff;
                colorType = data[payloadOffset + 9] & 0xff;
                int interlace = data[payloadOffset + 12] & 0xff;
                if (width <= 0 || height <= 0 || width > MAX_SPLASH_DIMENSION || height > MAX_SPLASH_DIMENSION) {
                    throw new IllegalArgumentException("Invalid CgBI dimensions");
                }
                if (depth != 8 || (colorType != 2 && colorType != 6) || interlace != 0) {
                    throw new IllegalArgumentException("Unsupported CgBI color format");
                }
                sawHeader = true;
            } else if ("IDAT".equals(type)) {
                if (!sawHeader || sawEnd) throw new IllegalArgumentException("Unexpected IDAT");
                compressed.write(data, payloadOffset, chunkLen);
            } else if ("IEND".equals(type)) {
                sawEnd = true;
                break;
            }
            p = crcOffset + 4;
        }
        if (!sawHeader || !sawEnd || compressed.size() == 0) {
            throw new IllegalArgumentException("Incomplete CgBI image");
        }
        int channels = (colorType == 6) ? 4 : 3;
        long strideLong = (long) width * channels;
        long expectedLong = (long) height * (strideLong + 1L);
        if (expectedLong <= 0 || expectedLong > MAX_CGBI_INFLATED_BYTES) {
            throw new IllegalArgumentException("CgBI image exceeds inflation limit");
        }
        byte[] rows = inflateCgbiPayload(compressed.toByteArray(), (int) expectedLong);
        int stride = (int) strideLong;
        byte[] previous = new byte[stride];
        byte[] row = new byte[stride];
        int[] colors = new int[width * height];
        int src = 0;
        for (int y = 0; y < height; y++) {
            int filter = rows[src++] & 0xff;
            for (int x = 0; x < stride; x++) {
                int raw = rows[src++] & 0xff;
                int left = (x >= channels) ? (row[x - channels] & 0xff) : 0;
                int above = previous[x] & 0xff;
                int upperLeft = (x >= channels) ? (previous[x - channels] & 0xff) : 0;
                int predictor;
                switch (filter) {
                    case 0: predictor = 0; break;
                    case 1: predictor = left; break;
                    case 2: predictor = above; break;
                    case 3: predictor = (left + above) >>> 1; break;
                    case 4: predictor = paeth(left, above, upperLeft); break;
                    default: throw new IllegalArgumentException("Invalid PNG filter: " + filter);
                }
                row[x] = (byte) ((raw + predictor) & 0xff);
            }
            int rowBase = y * width;
            for (int x = 0; x < width; x++) {
                int idx = x * channels;
                int blue = row[idx] & 0xff;
                int green = row[idx + 1] & 0xff;
                int red = row[idx + 2] & 0xff;
                int alpha = (channels == 4) ? (row[idx + 3] & 0xff) : 255;
                if (alpha > 0 && alpha < 255) {
                    red = Math.min(255, (red * 255 + (alpha >>> 1)) / alpha);
                    green = Math.min(255, (green * 255 + (alpha >>> 1)) / alpha);
                    blue = Math.min(255, (blue * 255 + (alpha >>> 1)) / alpha);
                } else if (alpha == 0) {
                    red = 0;
                    green = 0;
                    blue = 0;
                }
                colors[rowBase + x] = (alpha << 24) | (red << 16) | (green << 8) | blue;
            }
            System.arraycopy(row, 0, previous, 0, stride);
        }
        outDimensions[0] = width;
        outDimensions[1] = height;
        return colors;
    }

    private static byte[] inflateCgbiPayload(byte[] payload, int expected) {
        boolean[] modes = new boolean[] { true, false };
        for (boolean raw : modes) {
            Inflater inflater = new Inflater(raw);
            try {
                inflater.setInput(payload);
                byte[] output = new byte[expected];
                int written = 0;
                while (!inflater.finished() && written < expected) {
                    int count = inflater.inflate(output, written, expected - written);
                    if (count == 0) {
                        if (inflater.needsInput() || inflater.needsDictionary()) break;
                        throw new DataFormatException("Inflater stalled");
                    }
                    written += count;
                }
                if (written == expected) return output;
            } catch (DataFormatException ignored) {
                // Fall back to alternate header mode.
            } finally {
                inflater.end();
            }
        }
        throw new IllegalArgumentException("Could not inflate CgBI IDAT stream");
    }

    private static int paeth(int a, int b, int c) {
        int p = a + b - c;
        int pa = Math.abs(p - a);
        int pb = Math.abs(p - b);
        int pc = Math.abs(p - c);
        if (pa <= pb && pa <= pc) return a;
        return (pb <= pc) ? b : c;
    }

    private static int readU16Be(byte[] data, int offset) {
        return ((data[offset] & 0xff) << 8) | (data[offset + 1] & 0xff);
    }

    private static long readU32Be(byte[] data, int offset) {
        return ((long) (data[offset] & 0xff) << 24)
                | ((long) (data[offset + 1] & 0xff) << 16)
                | ((long) (data[offset + 2] & 0xff) << 8)
                | ((long) (data[offset + 3] & 0xff));
    }

    private static Bitmap decodeBitmapOrCgbi(byte[] bytes) {
        if (bytes == null || bytes.length == 0 || bytes.length > MAX_SPLASH_IMAGE_BYTES) {
            return null;
        }
        if (isCgbiPng(bytes)) {
            try {
                int[] dims = new int[2];
                int[] colors = decodeCgbiRgbaPixels(bytes, dims);
                return Bitmap.createBitmap(colors, dims[0], dims[1], Bitmap.Config.ARGB_8888);
            } catch (Throwable ignored) {
                return null;
            }
        }
        try {
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inPreferredConfig = Bitmap.Config.ARGB_8888;
            return BitmapFactory.decodeByteArray(bytes, 0, bytes.length, options);
        } catch (Throwable ignored) {
            return null;
        }
    }

    private static Bitmap composeOnViewport(Bitmap sprite, int viewportWidth, int viewportHeight) {
        if (sprite == null) return null;
        if (sprite.getWidth() >= viewportWidth && sprite.getHeight() >= viewportHeight) {
            return sprite;
        }
        int targetW = Math.max(viewportWidth, sprite.getWidth());
        int targetH = Math.max(viewportHeight, sprite.getHeight());
        Bitmap canvasBitmap = Bitmap.createBitmap(targetW, targetH, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(canvasBitmap);
        canvas.drawColor(Color.BLACK);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        float left = (targetW - sprite.getWidth()) * 0.5f;
        float top = (targetH - sprite.getHeight()) * 0.5f;
        canvas.drawBitmap(sprite, left, top, paint);
        return canvasBitmap;
    }

    private static List<SplashFrame> extractFramesFromSheet(
            String sheetSourcePath,
            Bitmap sheet,
            SplashSheetDescriptor descriptor) {
        if (sheet == null || descriptor == null || descriptor.entries.isEmpty()) {
            return Collections.emptyList();
        }
        List<SplashSpriteEntry> sorted = new ArrayList<>(descriptor.entries);
        // Put the full-size game title splash (e.g. SPLASH_ANGRY_BIRDS or largest area) first
        // so index 0 is the primary game splash screen.
        Collections.sort(sorted, new Comparator<SplashSpriteEntry>() {
            @Override
            public int compare(SplashSpriteEntry a, SplashSpriteEntry b) {
                int scoreA = scoreSplashEntry(a);
                int scoreB = scoreSplashEntry(b);
                return Integer.compare(scoreB, scoreA);
            }
        });
        List<SplashFrame> result = new ArrayList<>();
        for (SplashSpriteEntry entry : sorted) {
            if (entry.x < 0 || entry.y < 0
                    || entry.x + entry.width > sheet.getWidth()
                    || entry.y + entry.height > sheet.getHeight()) {
                continue;
            }
            Bitmap cropped = Bitmap.createBitmap(sheet, entry.x, entry.y, entry.width, entry.height);
            Bitmap viewportBitmap = composeOnViewport(
                    cropped,
                    DEFAULT_VIEWPORT_WIDTH,
                    DEFAULT_VIEWPORT_HEIGHT);
            result.add(new SplashFrame(
                    entry.name,
                    sheetSourcePath,
                    viewportBitmap,
                    entry.width,
                    entry.height));
        }
        return result;
    }

    private static int scoreSplashEntry(SplashSpriteEntry entry) {
        String upper = entry.name.toUpperCase(Locale.US);
        int bonus = 0;
        if (upper.contains("ANGRY") || upper.contains("GAME") || upper.contains("TITLE")
                || upper.contains("MAIN")) {
            bonus += 1_000_000;
        } else if (upper.contains("ROVIO")) {
            bonus += 500_000;
        }
        return bonus + (entry.width * entry.height);
    }

    private List<SplashFrame> discoverSplashFramesFromAssets(AssetManager assets) {
        List<SplashFrame> frames = new ArrayList<>();
        // 1. Check common sprite-sheet splash locations inside assets/bundle/
        String[] datCandidates = new String[] {
            "bundle/data/SPLASHES.dat",
            "bundle/SPLASHES.dat"
        };
        for (String datAsset : datCandidates) {
            byte[] datBytes = readOptionalAssetBytes(assets, datAsset, 256 * 1024);
            if (datBytes == null) continue;
            SplashSheetDescriptor descriptor = parseSplashSheetDescriptor(datBytes);
            if (descriptor == null) continue;
            String dirPrefix = datAsset.contains("/")
                    ? datAsset.substring(0, datAsset.lastIndexOf('/') + 1)
                    : "";
            String pngAsset = dirPrefix + descriptor.sheetName;
            byte[] pngBytes = readOptionalAssetBytes(assets, pngAsset, MAX_SPLASH_IMAGE_BYTES);
            Bitmap sheet = decodeBitmapOrCgbi(pngBytes);
            if (sheet != null) {
                String relPath = pngAsset.startsWith("bundle/")
                        ? pngAsset.substring("bundle/".length())
                        : pngAsset;
                frames.addAll(extractFramesFromSheet(relPath, sheet, descriptor));
            }
        }
        // 2. Check standard iOS launch/splash image asset names
        String[] launchCandidates = new String[] {
            "bundle/Default-Landscape.png",
            "bundle/Default-Landscape@2x.png",
            "bundle/Default-568h@2x.png",
            "bundle/Default@2x.png",
            "bundle/Default.png",
            "bundle/LaunchImage.png",
            "bundle/Splash.png",
            "bundle/splash.png",
            "bundle/data/MENU.png"
        };
        for (String candidate : launchCandidates) {
            byte[] bytes = readOptionalAssetBytes(assets, candidate, MAX_SPLASH_IMAGE_BYTES);
            Bitmap bmp = decodeBitmapOrCgbi(bytes);
            if (bmp != null) {
                String rel = candidate.startsWith("bundle/")
                        ? candidate.substring("bundle/".length())
                        : candidate;
                frames.add(new SplashFrame(
                        new File(rel).getName(),
                        rel,
                        bmp,
                        bmp.getWidth(),
                        bmp.getHeight()));
            }
        }
        // 3. Fallback to ipa-icon.png if no splash image was found yet
        if (frames.isEmpty()) {
            byte[] iconBytes = readOptionalAssetBytes(assets, "ipa-icon.png", MAX_SPLASH_IMAGE_BYTES);
            Bitmap icon = decodeBitmapOrCgbi(iconBytes);
            if (icon != null) {
                Bitmap composed = composeOnViewport(icon, DEFAULT_VIEWPORT_WIDTH, DEFAULT_VIEWPORT_HEIGHT);
                frames.add(new SplashFrame(
                        "ipa-icon.png",
                        "assets/ipa-icon.png",
                        composed,
                        icon.getWidth(),
                        icon.getHeight()));
            }
        }
        return frames;
    }

    private List<SplashFrame> discoverSplashFramesFromBundle(File bundleDir) {
        List<SplashFrame> frames = new ArrayList<>();
        List<File> allFiles = new ArrayList<>();
        collectFiles(bundleDir, allFiles);

        // 1. Look for any *SPLASH*.dat sprite sheet descriptors in the bundle
        for (File file : allFiles) {
            String lower = file.getName().toLowerCase(Locale.US);
            if (lower.endsWith(".dat") && lower.contains("splash") && file.length() <= 256 * 1024) {
                byte[] datBytes = readFileBytes(file, 256 * 1024);
                SplashSheetDescriptor descriptor = parseSplashSheetDescriptor(datBytes);
                if (descriptor == null) continue;
                File sheetFile = new File(file.getParentFile(), descriptor.sheetName);
                if (!sheetFile.isFile()) continue;
                Bitmap sheet = decodeBitmapOrCgbi(readFileBytes(sheetFile, MAX_SPLASH_IMAGE_BYTES));
                if (sheet != null) {
                    String rel = relativePath(bundleDir, sheetFile);
                    frames.addAll(extractFramesFromSheet(rel, sheet, descriptor));
                }
            }
        }

        // 2. Look for Default*.png / LaunchImage*.png / Splash*.png / MENU.png in bundle
        for (File file : allFiles) {
            String lower = file.getName().toLowerCase(Locale.US);
            if (!(lower.endsWith(".png") || lower.endsWith(".jpg") || lower.endsWith(".jpeg"))) {
                continue;
            }
            if (lower.equals("splashes.png") && !frames.isEmpty()) {
                continue;
            }
            boolean isLaunchOrSplash = lower.startsWith("default")
                    || lower.startsWith("launchimage")
                    || lower.startsWith("splash")
                    || lower.equals("menu.png")
                    || lower.equals("title.png")
                    || lower.equals("loading.png");
            if (!isLaunchOrSplash) continue;
            Bitmap bmp = decodeBitmapOrCgbi(readFileBytes(file, MAX_SPLASH_IMAGE_BYTES));
            if (bmp != null) {
                String rel = relativePath(bundleDir, file);
                frames.add(new SplashFrame(
                        file.getName(),
                        rel,
                        bmp,
                        bmp.getWidth(),
                        bmp.getHeight()));
            }
        }

        // 3. Fallback to assets/ipa-icon.png if bundle had no splash images
        if (frames.isEmpty()) {
            byte[] iconBytes = readOptionalAssetBytes(getAssets(), "ipa-icon.png", MAX_SPLASH_IMAGE_BYTES);
            Bitmap icon = decodeBitmapOrCgbi(iconBytes);
            if (icon != null) {
                Bitmap composed = composeOnViewport(icon, DEFAULT_VIEWPORT_WIDTH, DEFAULT_VIEWPORT_HEIGHT);
                frames.add(new SplashFrame(
                        "ipa-icon.png",
                        "assets/ipa-icon.png",
                        composed,
                        icon.getWidth(),
                        icon.getHeight()));
            }
        }
        return frames;
    }

    private static void collectFiles(File dir, List<File> out) {
        if (dir == null || !dir.exists() || out.size() >= 4096) return;
        File[] children = dir.listFiles();
        if (children == null) return;
        for (File child : children) {
            if (child.isDirectory()) {
                collectFiles(child, out);
            } else if (child.isFile()) {
                out.add(child);
            }
        }
    }

    private static String relativePath(File root, File target) {
        String rootPath = root.getAbsolutePath();
        String targetPath = target.getAbsolutePath();
        if (targetPath.startsWith(rootPath + File.separator)) {
            return targetPath.substring(rootPath.length() + 1).replace(File.separatorChar, '/');
        }
        return target.getName();
    }

    private static byte[] readFileBytes(File file, int maxBytes) {
        if (file == null || !file.isFile() || file.length() <= 0 || file.length() > maxBytes) {
            return null;
        }
        try (InputStream input = new FileInputStream(file)) {
            ByteArrayOutputStream output = new ByteArrayOutputStream((int) file.length());
            byte[] buffer = new byte[16384];
            int total = 0;
            while (true) {
                int count = input.read(buffer);
                if (count < 0) break;
                total += count;
                if (total > maxBytes) return null;
                output.write(buffer, 0, count);
            }
            return output.toByteArray();
        } catch (IOException ignored) {
            return null;
        }
    }

    private static byte[] readOptionalAssetBytes(AssetManager assets, String path, int maxBytes) {
        try (InputStream input = assets.open(path)) {
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            byte[] buffer = new byte[16384];
            int total = 0;
            while (true) {
                int count = input.read(buffer);
                if (count < 0) break;
                total += count;
                if (total > maxBytes) return null;
                output.write(buffer, 0, count);
            }
            return output.toByteArray();
        } catch (IOException ignored) {
            return null;
        }
    }

    static String summarizeNativeError(Throwable error) {
        if (error == null) return "unknown error";
        String message = error.getMessage();
        if (message == null || message.trim().isEmpty()) {
            return error.getClass().getSimpleName();
        }
        return error.getClass().getSimpleName() + ": " + message.trim();
    }

    static BootSummary summarizeBootReport(
            String appName,
            JSONObject manifest,
            String rawReportJson) {
        String headline = (appName == null || appName.trim().isEmpty())
                ? "Game-runtime boot attempt"
                : appName.trim();
        if (rawReportJson == null || rawReportJson.trim().isEmpty()) {
            return new BootSummary(
                    headline,
                    "BOOT FAILED CLOSED: native runtime returned an empty report",
                    formatManifestHeader(manifest),
                    true);
        }
        try {
            JSONObject report = new JSONObject(rawReportJson);
            String runStatus = report.optString("status", "not_runnable");
            JSONObject execution = report.optJSONObject("execution");
            String stopCategory = execution != null
                    ? execution.optString("stopCategory", "UNKNOWN")
                    : "UNKNOWN";
            String stopReason = execution != null
                    ? execution.optString("stopReason", "")
                    : "";
            long instructions = execution != null
                    ? execution.optLong("executedInstructions", 0L)
                    : 0L;
            long lastPc = execution != null
                    ? execution.optLong("lastProgramCounter", 0L)
                    : 0L;
            String trappedImport = firstTrappedImport(report.optJSONArray("trappedImports"));
            boolean failedClosed = !"runnable".equals(runStatus);

            String statusLine;
            if (!trappedImport.isEmpty()) {
                statusLine = "Stopped at first unimplemented import (" + stopCategory + "): "
                        + trappedImport;
            } else if (!stopReason.isEmpty()) {
                statusLine = "Boot stopped (" + stopCategory + "): " + stopReason;
            } else {
                statusLine = "Boot finished with status: " + runStatus + " (" + stopCategory + ")";
            }

            StringBuilder details = new StringBuilder();
            details.append("Contract: game-runtime-v1 (guest boot attempt)\n");
            details.append("Splash viewport: active (bundle splash screen rendered above)\n");
            details.append("Runtime status: ").append(runStatus).append('\n');
            details.append("Stop category: ").append(stopCategory).append('\n');
            if (!stopReason.isEmpty()) {
                details.append("Stop reason: ").append(stopReason).append('\n');
            }
            if (!trappedImport.isEmpty()) {
                details.append("Trapped import: ").append(trappedImport).append('\n');
            }
            details.append("Instructions executed: ").append(instructions).append('\n');
            details.append(String.format("Last guest PC: 0x%08x\n", lastPc));
            JSONObject loader = report.optJSONObject("loader");
            if (loader != null) {
                details.append("Loader status: ").append(loader.optString("status", "")).append('\n');
                details.append("Mapped segments: ")
                        .append(loader.optInt("mappedSegments", 0))
                        .append(", bound imports: ")
                        .append(loader.optInt("boundImports", 0))
                        .append(", trapped stubs: ")
                        .append(loader.optInt("trappedImports", 0))
                        .append('\n');
            }
            JSONObject lifecycle = report.optJSONObject("appLifecycle");
            if (lifecycle != null) {
                details.append("UIApplicationMain callReached: ")
                        .append(lifecycle.optBoolean("callReached", false))
                        .append(", delegateReturned: ")
                        .append(lifecycle.optBoolean("delegateReturned", false))
                        .append('\n');
                String delegateClass = lifecycle.optString("delegateClass", "");
                String delegateSelector = lifecycle.optString("delegateSelector", "");
                if (!delegateClass.isEmpty()) {
                    details.append("Delegate call: -[")
                            .append(delegateClass)
                            .append(' ')
                            .append(delegateSelector)
                            .append("]\n");
                }
            }
            details.append('\n').append(formatManifestHeader(manifest));
            return new BootSummary(headline, statusLine, details.toString(), failedClosed);
        } catch (Exception malformed) {
            return new BootSummary(
                    headline,
                    "BOOT FAILED CLOSED: malformed native report ("
                            + summarizeNativeError(malformed) + ")",
                    rawReportJson + "\n\n" + formatManifestHeader(manifest),
                    true);
        }
    }

    private static String firstTrappedImport(JSONArray trappedImports) {
        if (trappedImports == null || trappedImports.length() == 0) return "";
        JSONObject first = trappedImports.optJSONObject(0);
        if (first == null) return "";
        String symbol = first.optString("symbol", "");
        String library = first.optString("library", "");
        long caller = first.optLong("lastCaller", 0L);
        if (symbol.isEmpty()) return "";
        StringBuilder summary = new StringBuilder(symbol);
        if (!library.isEmpty()) {
            summary.append(" (").append(library).append(')');
        }
        if (caller != 0L) {
            summary.append(String.format(" from lr=0x%08x", caller));
        }
        return summary.toString();
    }

    private static String formatManifestHeader(JSONObject manifest) {
        if (manifest == null) return "";
        StringBuilder out = new StringBuilder();
        JSONObject executable = manifest.optJSONObject("executable");
        if (executable != null) {
            out.append("Executable: ")
                    .append(executable.optString("name", ""))
                    .append(" (sha256=")
                    .append(executable.optString("sha256", ""))
                    .append(")\n");
        }
        JSONObject resources = manifest.optJSONObject("resources");
        if (resources != null) {
            out.append("Packaged bundle resources: ")
                    .append(resources.optInt("files", 0))
                    .append(" files (")
                    .append(resources.optLong("bytes", 0L))
                    .append(" bytes)\n");
        }
        out.append("No gameplay or conversion is claimed by this artifact.");
        return out.toString();
    }

    static String sanitizeRelativePath(String rawPath) throws IOException {
        if (rawPath == null) throw new IOException("Asset path is null");
        String normalized = rawPath.replace('\\', '/').trim();
        if (normalized.isEmpty() || normalized.startsWith("/") || normalized.contains("\0")) {
            throw new IOException("Rejected unsafe asset path: " + rawPath);
        }
        String[] parts = normalized.split("/");
        StringBuilder clean = new StringBuilder();
        for (String part : parts) {
            if (part.isEmpty() || ".".equals(part) || "..".equals(part)) {
                throw new IOException("Rejected relative traversal segment in: " + rawPath);
            }
            if (clean.length() > 0) clean.append('/');
            clean.append(part);
        }
        return clean.toString();
    }

    private JSONObject readManifest() throws Exception {
        byte[] bytes = readAssetBytes(METADATA_ASSET, MAX_METADATA_BYTES);
        return new JSONObject(new String(bytes, StandardCharsets.UTF_8));
    }

    private void stageBundleAssets(JSONObject manifest, File bundleDir) throws IOException {
        JSONArray inventory = manifest.optJSONArray("resourceInventory");
        if (inventory != null && inventory.length() > 0) {
            for (int index = 0; index < inventory.length(); index++) {
                JSONObject item = inventory.optJSONObject(index);
                if (item == null) continue;
                String rel = sanitizeRelativePath(item.optString("path", ""));
                File target = new File(bundleDir, rel);
                ensureInsideRoot(bundleDir, target);
                extractAssetToFile(BUNDLE_ASSET_PREFIX + "/" + rel, target, MAX_SINGLE_ASSET_BYTES);
            }
            return;
        }
        stageAssetTreeRecursive(getAssets(), BUNDLE_ASSET_PREFIX, bundleDir, bundleDir);
    }

    private void stageAssetTreeRecursive(
            AssetManager assets,
            String assetPath,
            File rootDir,
            File currentDir) throws IOException {
        String[] children = assets.list(assetPath);
        if (children == null || children.length == 0) {
            return;
        }
        for (String child : children) {
            String safeName = sanitizeRelativePath(child);
            String childAssetPath = assetPath + "/" + safeName;
            File target = new File(currentDir, safeName);
            ensureInsideRoot(rootDir, target);
            String[] grandChildren = assets.list(childAssetPath);
            if (grandChildren != null && grandChildren.length > 0) {
                if (!target.mkdirs() && !target.isDirectory()) {
                    throw new IOException("Could not create bundle subdirectory: " + target);
                }
                stageAssetTreeRecursive(assets, childAssetPath, rootDir, target);
            } else {
                extractAssetToFile(childAssetPath, target, MAX_SINGLE_ASSET_BYTES);
            }
        }
    }

    private static void ensureInsideRoot(File rootDir, File target) throws IOException {
        String rootCanonical = rootDir.getCanonicalPath();
        String targetCanonical = target.getCanonicalPath();
        if (!targetCanonical.equals(rootCanonical)
                && !targetCanonical.startsWith(rootCanonical + File.separator)) {
            throw new IOException("Refusing to write outside bundle root: " + target);
        }
    }

    private byte[] readAssetBytes(String assetPath, long maxBytes) throws IOException {
        try (InputStream input = getAssets().open(assetPath)) {
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            byte[] buffer = new byte[16384];
            long total = 0L;
            while (true) {
                int count = input.read(buffer);
                if (count < 0) break;
                total += count;
                if (total > maxBytes) {
                    throw new IOException("Asset exceeds size cap: " + assetPath);
                }
                output.write(buffer, 0, count);
            }
            return output.toByteArray();
        }
    }

    private void extractAssetToFile(String assetPath, File target, long maxBytes) throws IOException {
        File parent = target.getParentFile();
        if (parent != null && !parent.mkdirs() && !parent.isDirectory()) {
            throw new IOException("Could not create parent directory for " + target);
        }
        try (InputStream input = getAssets().open(assetPath);
             FileOutputStream output = new FileOutputStream(target)) {
            byte[] buffer = new byte[32768];
            long total = 0L;
            while (true) {
                int count = input.read(buffer);
                if (count < 0) break;
                total += count;
                if (total > maxBytes) {
                    throw new IOException("Asset exceeds size cap while extracting: " + assetPath);
                }
                output.write(buffer, 0, count);
            }
        }
    }

    private static void deleteRecursively(File file) {
        if (file == null || !file.exists()) return;
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) {
                for (File child : children) {
                    deleteRecursively(child);
                }
            }
        }
        // Best-effort cleanup of the previous run's staged cache directory.
        //noinspection ResultOfMethodCallIgnored
        file.delete();
    }
}
