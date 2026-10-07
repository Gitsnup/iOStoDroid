package dev.radek.gameruntime;

import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;
import org.robolectric.shadows.ShadowLooper;

import java.util.concurrent.TimeUnit;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 28, manifest = Config.NONE)
public final class GameBootActivityTest {
    @Test
    public void missingMetadataLeavesTheDiagnosticScreenOpen() {
        GameBootActivity activity = Robolectric.buildActivity(GameBootActivity.class).create().get();
        ShadowLooper mainLooper = Shadows.shadowOf(Looper.getMainLooper());
        mainLooper.idle();

        assertFalse(activity.isFinishing());
        String screenText = textIn(activity.getWindow().getDecorView());
        assertTrue(screenText.contains("Boot metadata is missing or invalid"));
        assertTrue(screenText.contains("diagnostic screen will remain open"));

        // The old launcher posted a delayed RuntimeException after displaying
        // this same error. Advancing past that delay must not crash the activity.
        mainLooper.idleFor(2, TimeUnit.SECONDS);
        assertFalse(activity.isFinishing());
    }

    @Test
    public void blockedGuestBootShowsTheImportAndDoesNotCrash() {
        GameBootActivity activity = Robolectric.buildActivity(GameBootActivity.class).create().get();
        activity.displayBootResult(
                "{\"loader\":{\"status\":\"LOADED_WITH_TRAPS\",\"resolvedSymbolCount\":39," +
                        "\"trappedSymbolCount\":539,\"unresolvedSymbolCount\":0}," +
                        "\"execution\":{\"status\":\"STOPPED_AT_TRAP\",\"instructions\":34}," +
                        "\"trappedImport\":\"_UIApplicationMain\",\"reason\":\"unimplemented import\"}"
        );
        ShadowLooper mainLooper = Shadows.shadowOf(Looper.getMainLooper());
        mainLooper.idle();

        String screenText = textIn(activity.getWindow().getDecorView());
        assertTrue(screenText.contains("_UIApplicationMain"));
        assertTrue(screenText.contains("Guest boot stopped"));
        assertTrue(screenText.contains("This APK is not a playable conversion"));
        assertFalse(activity.isFinishing());

        mainLooper.idleFor(2, TimeUnit.SECONDS);
        assertFalse(activity.isFinishing());
    }

    private static String textIn(View view) {
        StringBuilder result = new StringBuilder();
        if (view instanceof TextView) result.append(((TextView) view).getText()).append('\n');
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int index = 0; index < group.getChildCount(); index++) {
                result.append(textIn(group.getChildAt(index)));
            }
        }
        return result.toString();
    }
}
