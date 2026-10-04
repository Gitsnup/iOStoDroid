package dev.radek.conventor

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Test

class ArtifactNamesTest {
    @Test fun outputNameUsesSanitizedPickedIpaBasename() {
        val report = JSONObject().put("source", JSONObject().put("originalName", "../My Game.ipa"))
        assertEquals("My Game.apk", ArtifactNames.apkFileName(report))
    }

    @Test fun fallsBackSafelyWhenOriginalNameIsMissing() {
        val report = JSONObject().put("application", JSONObject().put("name", "bad:game"))
        assertEquals("bad_game.apk", ArtifactNames.apkFileName(report))
    }
}
