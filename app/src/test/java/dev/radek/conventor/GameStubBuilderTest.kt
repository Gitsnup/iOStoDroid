package dev.radek.conventor

import android.app.Application
import android.graphics.Bitmap
import android.graphics.Color
import com.android.apksig.ApkVerifier
import java.io.ByteArrayOutputStream
import java.io.File
import java.security.MessageDigest
import java.util.zip.ZipFile
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class GameStubBuilderTest {
    private fun sha256(bytes: ByteArray): String = MessageDigest.getInstance("SHA-256").digest(bytes)
        .joinToString("") { "%02x".format(it.toInt() and 0xff) }

    private fun png(color: Int): ByteArray {
        val bitmap = Bitmap.createBitmap(16, 16, Bitmap.Config.ARGB_8888)
        bitmap.eraseColor(color)
        return try {
            ByteArrayOutputStream().use { output ->
                assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, output))
                output.toByteArray()
            }
        } finally { bitmap.recycle() }
    }

    @Test fun outputNameUsesPickedIpaBasenameAndSanitizesPathCharacters() {
        val report = JSONObject().put("source", JSONObject().put("originalName", "../My Game.ipa"))
        assertEquals("My Game.apk", GameStubBuilder.fileName(report))
    }

    @Test fun generatedArtifactIsSignedAndUsesRecoveredIconWithoutEmbeddingIpa() {
        val root = createTempDir(prefix = "radek-game-stub-test")
        try {
            val input = "authorized test IPA bytes that must not be copied".toByteArray()
            File(root, "source.ipa").writeBytes(input)
            File(root, "icon.png").writeBytes(png(Color.MAGENTA))
            val appBundle = File(root, "Fixture.app").apply { mkdirs() }
            File(appBundle, "config.json").writeText("{\"levelCount\":3}")
            val stubContent = StubContent.collect(appBundle, root, "Fixture")
            val report = JSONObject()
                .put("stubContent", stubContent)
                .put("source", JSONObject().put("sha256", sha256(input)).put("bytes", input.size).put("originalName", "My Actual Game.ipa"))
                .put("application", JSONObject().put("name", "Fixture Game").put("bundleId", "org.example.fixture")
                    .put("minimumIOSVersion", "12.0"))
            val application = RuntimeEnvironment.getApplication<Application>()
            var lastProgress = 0

            val output = GameStubBuilder.create(application, root, report) { percent, _ -> lastProgress = percent }

            assertEquals("My Actual Game.apk", output.name)
            assertEquals(100, lastProgress)
            ZipFile(output).use { apk ->
                assertNotNull(apk.getEntry("AndroidManifest.xml"))
                assertNotNull(apk.getEntry("classes.dex"))
                assertFalse(apk.getEntry("assets/radek/source.ipa") != null)
                assertNotNull(apk.getEntry("assets/radek/game-stub.json"))
                assertNotNull(apk.getEntry("assets/radek/content-index.json"))
                assertEquals("{\"levelCount\":3}", apk.getInputStream(apk.getEntry("assets/ipa-content/config.json")).bufferedReader().use { it.readText() })
                val metadata = JSONObject(apk.getInputStream(apk.getEntry("assets/radek/game-stub.json")).bufferedReader().use { it.readText() })
                assertTrue(metadata.getBoolean("installableAndroidPackage"))
                assertFalse(metadata.getBoolean("gameCodeConverted"))
                assertEquals("INSTALLABLE_GAME_ICON_STUB", metadata.getString("artifactKind"))
                assertEquals("12.0", metadata.getJSONObject("application").getString("minimumIOSVersion"))
                assertEquals(1, metadata.getJSONObject("bundleContent").getInt("fileCount"))
                val index = JSONArray(apk.getInputStream(apk.getEntry("assets/radek/content-index.json")).bufferedReader().use { it.readText() })
                assertEquals("config.json", index.getJSONObject(0).getString("path"))

                val entries = apk.entries()
                var iconBytes: ByteArray? = null
                while (entries.hasMoreElements()) {
                    val entry = entries.nextElement()
                    if (entry.name.startsWith("res/drawable") && entry.name.endsWith("/game_icon.png")) {
                        iconBytes = apk.getInputStream(entry).use { it.readBytes() }
                        break
                    }
                }
                assertNotNull(iconBytes)
                val icon = IconDecoder.decode(iconBytes!!, 512)
                assertNotNull(icon)
                try { assertEquals(Color.MAGENTA, icon!!.getPixel(0, 0)) }
                finally { icon?.recycle() }
            }
            assertTrue(ApkVerifier.Builder(output).build().verify().isVerified)
        } finally {
            root.deleteRecursively()
        }
    }

    @Test fun missingRecoveredIconCannotFallBackToGenericTemplateIcon() {
        val root = createTempDir(prefix = "radek-game-stub-no-icon-test")
        try {
            val input = byteArrayOf(4, 5, 6)
            File(root, "source.ipa").writeBytes(input)
            val report = JSONObject().put("source", JSONObject()
                .put("sha256", sha256(input)).put("originalName", "No Icon.ipa"))
            val error = org.junit.Assert.assertThrows(IllegalStateException::class.java) {
                GameStubBuilder.create(RuntimeEnvironment.getApplication<Application>(), root, report) { _, _ -> }
            }
            assertTrue(error.message.orEmpty().contains("no generic-icon APK was created"))
            assertFalse(File(root, GameStubBuilder.fileName(report)).exists())
        } finally { root.deleteRecursively() }
    }

    @Test fun sourceHashMismatchCannotPublishApk() {
        val root = createTempDir(prefix = "radek-game-stub-hash-test")
        try {
            File(root, "source.ipa").writeBytes(byteArrayOf(1, 2, 3))
            val report = JSONObject().put("source", JSONObject()
                .put("sha256", "0".repeat(64)).put("originalName", "Wrong Game.ipa"))
            val error = org.junit.Assert.assertThrows(IllegalArgumentException::class.java) {
                GameStubBuilder.create(RuntimeEnvironment.getApplication<Application>(), root, report) { _, _ -> }
            }
            assertTrue(error.message.orEmpty().contains("does not match"))
            assertFalse(File(root, GameStubBuilder.fileName(report)).exists())
        } finally {
            root.deleteRecursively()
        }
    }
}
