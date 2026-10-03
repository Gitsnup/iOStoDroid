package dev.radek.generated;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.util.Log;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;

/**
 * Entry activity of every APK this converter generates.
 *
 * It loads the converted ARM64 image, calls the proved native entry point and
 * shows what was actually converted: the source identity, the native backend,
 * the Android provider coverage and the iOS dependencies that were mapped.
 *
 * The class is compiled both into the converter application and into the
 * standalone {@code runtime.dex} asset that generated APKs carry.
 */
public final class MainActivity extends Activity {
    private static final String TAG = "RadekConverted";
    private static boolean loaded;
    private static String loadError;

    static {
        try {
            System.loadLibrary("converted");
            loaded = true;
        } catch (Throwable error) {
            loadError = error.toString();
            Log.e(TAG, "could not load libconverted.so", error);
        }
    }

    private static native int runNative();

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        int density = (int) getResources().getDisplayMetrics().density;
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(12, 19, 31));
        root.setPadding(22 * density, 30 * density, 22 * density, 36 * density);

        JSONObject conversion = readConversion();
        JSONObject source = conversion == null ? new JSONObject() : conversion.optJSONObject("source");
        String label = source.optString("name", "Converted application");

        Bitmap icon = readIcon();
        if (icon != null) {
            ImageView view = new ImageView(this);
            view.setImageBitmap(icon);
            view.setContentDescription(label);
            root.addView(view, new LinearLayout.LayoutParams(96 * density, 96 * density));
        }

        addText(root, label, 26, Color.WHITE, true);
        addText(root, "Converted from iOS by RadekConventor, on device", 13, MUTED, false);

        if (conversion != null) {
            JSONObject nativeReport = conversion.optJSONObject("conversion");
            int support = conversion.optInt("supportPercent");
            addText(root, "Android support " + support + "%", 18,
                    support == 100 ? ACCENT : WARNING, true);
            addText(root, "package " + conversion.optString("package"), 12, MUTED, false);
            String hash = source.optString("sha256");
            addText(root, "source " + source.optString("bundleId") + " · sha256 "
                    + hash.substring(0, Math.min(16, hash.length())), 12, MUTED, false);
            addText(root, "native backend " + nativeReport.optString("backend") + " · "
                            + nativeReport.optInt("outputBytes") + " bytes of Android ARM64 · "
                            + nativeReport.optInt("instructions") + " proved instructions",
                    12, MUTED, false);
            if (conversion.optBoolean("forced")) {
                addText(root, "Forced conversion: the entry leaf was outside the proved "
                        + "closed-integer subset.", 12, DANGER, false);
            }
            JSONArray providers = conversion.optJSONArray("providers");
            if (providers != null && providers.length() > 0) {
                addText(root, "iOS dependencies mapped to Android", 16, Color.WHITE, true);
                for (int index = 0; index < providers.length(); index++) {
                    JSONObject provider = providers.optJSONObject(index);
                    String status = provider.optString("status");
                    int color = "provided".equals(status) ? ACCENT
                            : "compatibility".equals(status) ? WARNING : DANGER;
                    addText(root, provider.optString("framework") + " → "
                            + provider.optString("provider"), 12, color, false);
                }
            }
        }

        String result;
        if (!loaded) {
            result = "libconverted.so could not be loaded: " + loadError;
        } else {
            try {
                int value = runNative();
                Log.i(TAG, "native entry returned " + value);
                result = "Converted native entry returned " + value + "\n\nThis is real Android "
                        + "ARM64 machine code re-emitted from the iOS entry leaf.";
            } catch (Throwable error) {
                result = "Native entry failed: " + error;
            }
        }
        addText(root, result, 15, Color.WHITE, false);

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.rgb(12, 19, 31));
        scroll.addView(root);
        setContentView(scroll);
    }

    private static final int MUTED = Color.rgb(160, 178, 199);
    private static final int ACCENT = Color.rgb(92, 227, 181);
    private static final int WARNING = Color.rgb(245, 203, 116);
    private static final int DANGER = Color.rgb(255, 157, 139);

    private void addText(LinearLayout parent, String value, int size, int color, boolean bold) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        if (bold) {
            view.setTypeface(view.getTypeface(), Typeface.BOLD);
        }
        view.setPadding(0, 6, 0, 7);
        parent.addView(view, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
    }

    private JSONObject readConversion() {
        try {
            InputStream input = getAssets().open("conversion.json");
            try {
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] buffer = new byte[65536];
                while (true) {
                    int count = input.read(buffer);
                    if (count < 0) {
                        break;
                    }
                    out.write(buffer, 0, count);
                }
                return new JSONObject(new String(out.toByteArray(), "UTF-8"));
            } finally {
                input.close();
            }
        } catch (Exception error) {
            Log.w(TAG, "no conversion.json", error);
            return null;
        }
    }

    private Bitmap readIcon() {
        try {
            InputStream input = getAssets().open("icon.png");
            try {
                return BitmapFactory.decodeStream(input);
            } finally {
                input.close();
            }
        } catch (Exception error) {
            return null;
        }
    }
}
