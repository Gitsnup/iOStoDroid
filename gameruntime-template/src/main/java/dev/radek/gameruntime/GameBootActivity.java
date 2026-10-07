package dev.radek.gameruntime;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

/**
 * Launcher of a game-runtime boot-attempt APK (contract "game-runtime-v1").
 *
 * <p>The APK embeds one authorized IPA main executable. This activity runs the
 * real guest boot through libcompat_runtime_v1.so and shows the boot attempt as
 * a minimal diagnostic log. Guest execution stops at the first unimplemented
 * import, but the Android activity remains open to show the result. This is not
 * a preview, menu, or gameplay UI; it is a boot-attempt diagnostic screen.
 */
public final class GameBootActivity extends Activity {
    private static final String RUNTIME_LIBRARY = "compat_runtime_v1";
    private static final String EXECUTABLE_ASSET = "gameboot/main-executable.bin";
    private static final String METADATA_ASSET = "gameboot.json";
    private static final long MAX_EXECUTABLE_BYTES = 256L * 1024L * 1024L;
    private static final long MAX_METADATA_BYTES = 4L * 1024L * 1024L;

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private TextView titleView;
    private TextView logView;
    private ScrollView scroller;
    private volatile boolean destroyed = false;

    private static native String runGameBootAttempt(byte[] mainBinary, boolean authorizationConfirmed);

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private void appendLine(final String line) {
        mainHandler.post(new Runnable() {
            @Override
            public void run() {
                if (destroyed || logView == null) return;
                logView.append(line);
                logView.append("\n");
                if (scroller != null) {
                    scroller.post(new Runnable() {
                        @Override
                        public void run() {
                            scroller.fullScroll(ScrollView.FOCUS_DOWN);
                        }
                    });
                }
            }
        });
    }

    /**
     * Report a terminal boot result without throwing on Android's main thread.
     * A missing import or runtime dependency should leave useful diagnostics on
     * screen, not turn a handled boot failure into an application crash.
     */
    private void showTerminalState(final String title, final String detail) {
        mainHandler.post(new Runnable() {
            @Override
            public void run() {
                if (destroyed || titleView == null || logView == null) return;
                titleView.setText(title);
                titleView.setTextColor(Color.rgb(255, 190, 92));
                logView.append(detail);
                logView.append("\n");
                logView.append("The diagnostic screen will remain open. This APK is not a playable conversion.\n");
                scrollToBottom();
            }
        });
    }

    private void scrollToBottom() {
        if (scroller == null) return;
        scroller.post(new Runnable() {
            @Override
            public void run() {
                if (!destroyed && scroller != null) scroller.fullScroll(ScrollView.FOCUS_DOWN);
            }
        });
    }

    private byte[] readAssetBounded(String name, long maximum) throws Exception {
        InputStream input = getAssets().open(name);
        try {
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            byte[] buffer = new byte[65536];
            long total = 0;
            while (true) {
                int count = input.read(buffer);
                if (count < 0) break;
                total += count;
                if (total > maximum) throw new IllegalStateException("embedded asset exceeds its limit: " + name);
                output.write(buffer, 0, count);
            }
            if (total == 0) throw new IllegalStateException("embedded asset is empty: " + name);
            return output.toByteArray();
        } finally {
            try {
                input.close();
            } catch (Exception ignored) {
                // Closing a fully-read asset stream cannot fail the boot.
            }
        }
    }

    private String summarizeBoot(String reportText) {
        try {
            JSONObject report = new JSONObject(reportText);
            JSONObject loader = report.optJSONObject("loader");
            JSONObject execution = report.optJSONObject("execution");
            StringBuilder summary = new StringBuilder();
            summary.append("Loader: ").append(loader != null ? loader.optString("status", "?") : "?");
            if (loader != null) {
                summary.append(" (").append(loader.optInt("resolvedSymbolCount", 0)).append(" resolved, ")
                        .append(loader.optInt("trappedSymbolCount", 0)).append(" trapped, ")
                        .append(loader.optInt("unresolvedSymbolCount", 0)).append(" unresolved)");
            }
            summary.append("\n");
            if (execution != null) {
                summary.append("Executed ").append(execution.optLong("instructions", 0)).append(" guest instruction(s); ");
                summary.append("status ").append(execution.optString("status", "?")).append("\n");
            }
            String trapped = report.optString("trappedImport", "");
            if (!trapped.isEmpty()) summary.append("Stopped at unimplemented import: ").append(trapped).append("\n");
            JSONArray trappedSymbols = report.optJSONArray("trappedSymbols");
            if (trappedSymbols != null) summary.append("Trapped imports bound: ").append(trappedSymbols.length()).append("\n");
            String reason = report.optString("reason", "");
            if (!reason.isEmpty()) summary.append(reason);
            return summary.toString();
        } catch (Exception error) {
            return "Unparseable boot report (" + error + "): " + reportText;
        }
    }

    private String bootStatus(String reportText) {
        try {
            JSONObject execution = new JSONObject(reportText).optJSONObject("execution");
            return execution != null ? execution.optString("status", "") : "";
        } catch (Exception ignored) {
            return "";
        }
    }

    // Package-private so the launcher template's Robolectric test can exercise
    // the same blocked-guest path used after runGameBootAttempt returns.
    void displayBootResult(String reportText) {
        String safeReport = reportText != null ? reportText : "{}";
        appendLine(summarizeBoot(safeReport));
        if ("RETURNED".equals(bootStatus(safeReport))) {
            showTerminalState("Guest entry returned", "Guest entry returned without a game lifecycle.");
        } else {
            showTerminalState("Guest boot stopped", "Guest execution stopped at a missing or unimplemented runtime call.");
        }
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(Color.rgb(11, 16, 29));
        getWindow().setNavigationBarColor(Color.rgb(11, 16, 29));

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(11, 16, 29));
        root.setPadding(dp(20), dp(32), dp(20), dp(32));

        titleView = new TextView(this);
        titleView.setText("Game boot attempt");
        titleView.setTextSize(20);
        titleView.setTextColor(Color.WHITE);
        titleView.setTypeface(null, Typeface.BOLD);
        titleView.setGravity(Gravity.CENTER_HORIZONTAL);
        root.addView(titleView, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        logView = new TextView(this);
        logView.setTextSize(12);
        logView.setTextColor(Color.rgb(92, 227, 181));
        logView.setTypeface(Typeface.MONOSPACE);
        scroller = new ScrollView(this);
        scroller.addView(logView, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        LinearLayout.LayoutParams scrollerParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1.0f);
        scrollerParams.topMargin = dp(16);
        root.addView(scroller, scrollerParams);
        setContentView(root);

        String appName = "";
        try {
            byte[] metadataBytes = readAssetBounded(METADATA_ASSET, MAX_METADATA_BYTES);
            JSONObject metadata = new JSONObject(new String(metadataBytes, StandardCharsets.UTF_8));
            appName = metadata.optString("applicationName", "");
        } catch (Exception error) {
            appendLine("Boot metadata is missing or invalid: " + error);
            showTerminalState("Boot attempt could not start", "The diagnostic screen remains open so this error can be reviewed.");
            return;
        }
        final String displayName = appName.isEmpty() ? "embedded game" : appName;
        appendLine("Booting " + displayName + " (game-runtime-v1)...");
        appendLine("Loading the compatibility runtime library...");

        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    System.loadLibrary(RUNTIME_LIBRARY);
                } catch (Throwable error) {
                    appendLine("Runtime library failed to load: " + error);
                    showTerminalState("Runtime library unavailable", "Check that the APK includes every native dependency. The diagnostic screen will remain open.");
                    return;
                }
                appendLine("Runtime library loaded.");
                final byte[] executable;
                try {
                    executable = readAssetBounded(EXECUTABLE_ASSET, MAX_EXECUTABLE_BYTES);
                } catch (Throwable error) {
                    appendLine("Embedded executable could not be read: " + error);
                    showTerminalState("Executable unavailable", "The boot attempt could not continue; the diagnostic screen will remain open.");
                    return;
                }
                appendLine("Executable loaded: " + executable.length + " byte(s). Mapping and binding traps...");
                final String reportText;
                try {
                    reportText = runGameBootAttempt(executable, true);
                } catch (Throwable error) {
                    appendLine("Boot attempt failed inside the runtime: " + error);
                    showTerminalState("Guest boot failed", "The runtime could not complete the boot attempt; diagnostics will remain visible.");
                    return;
                }
                displayBootResult(reportText);
            }
        }, "game-boot").start();
    }

    @Override
    protected void onDestroy() {
        destroyed = true;
        super.onDestroy();
    }
}
