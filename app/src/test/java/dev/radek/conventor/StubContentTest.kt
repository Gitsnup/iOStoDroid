package dev.radek.conventor

import java.io.File
import java.nio.file.Files
import org.json.JSONArray
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class StubContentTest {
    @Test fun cachesOnlyAllowlistedNonExecutableBundleContent() {
        val root = Files.createTempDirectory("radek-stub-content").toFile()
        try {
            val app = File(root, "Fixture.app").apply { mkdirs() }
            val libraryEntry = File(root, "library-entry").apply { mkdirs() }
            File(app, "Fixture").writeBytes(byteArrayOf(1, 2, 3, 4))
            File(app, "config.json").writeText("{\"levels\":2}")
            File(app, "en.lproj").mkdirs()
            File(app, "en.lproj/Localizable.strings").writeText("\"start\" = \"Start\";")
            File(app, "AppIcon.png").writeBytes(byteArrayOf(0x89.toByte(), 0x50, 0x4e, 0x47, 1, 2, 3, 4))
            File(app, "Disguised.png").writeBytes(byteArrayOf(0xcf.toByte(), 0xfa.toByte(), 0xed.toByte(), 0xfe.toByte()))
            File(app, "Payload.dylib").writeBytes(byteArrayOf(1, 2, 3, 4))

            val summary = StubContent.collect(app, libraryEntry, "Fixture")

            assertEquals(3, summary.getInt("fileCount"))
            assertTrue(File(libraryEntry, "stub-content/config.json").isFile)
            assertTrue(File(libraryEntry, "stub-content/AppIcon.png").isFile)
            assertTrue(File(libraryEntry, "stub-content/en.lproj/Localizable.strings").isFile)
            assertFalse(File(libraryEntry, "stub-content/Fixture").exists())
            assertFalse(File(libraryEntry, "stub-content/Disguised.png").exists())
            assertFalse(File(libraryEntry, "stub-content/Payload.dylib").exists())
            val index = JSONArray(File(libraryEntry, "stub-content-index.json").readText())
            assertEquals(3, index.length())
            assertTrue(summary.getString("notice").contains("not translated"))
        } finally { root.deleteRecursively() }
    }
}
