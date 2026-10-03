package dev.radek.conventor

import android.app.Application
import java.io.File
import java.util.UUID
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class WorkflowProgressTest {
    @Test fun interruptedAnalysisKeepsItsLastProgressAndExplainsFailure() {
        val library = Library(RuntimeEnvironment.getApplication<Application>())
        val entry = File(library.root, "interrupted-${UUID.randomUUID()}").apply { mkdirs() }
        try {
            File(entry, "source.ipa").writeText("retained authorized source")
            File(entry, "extracted/Payload/Game.app").apply { mkdirs() }
            val report = JSONObject()
                .put("state", "ANALYZING")
                .put("source", JSONObject().put("sha256", "a".repeat(64)))
                .put("workflowProgress", WorkflowProgress(43, "Safe bundle content", "Checking resources", "RUNNING").toJson())
            library.save(entry, report)

            library.recoverInterrupted()

            val recovered = JSONObject(File(entry, "report.json").readText())
            val progress = recovered.getJSONObject("workflowProgress")
            assertEquals("FAILED", recovered.getString("state"))
            assertEquals("FAILED", progress.getString("status"))
            assertEquals(43, progress.getInt("percent"))
            assertTrue(progress.getString("stage").contains("Safe bundle content"))
            assertFalse(File(entry, "extracted").exists())
            assertTrue(File(entry, "source.ipa").isFile)
            assertTrue(recovered.getString("error").contains("Process ended"))
        } finally { entry.deleteRecursively() }
    }
}
