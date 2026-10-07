package dev.radek.conventor

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.nio.file.Files
import java.security.MessageDigest

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class GameRuntimeArtifactContractTest {
    @Test fun validatesOnlyTheExplicitGameRuntimeArtifactAndItsDigest() {
        val directory = Files.createTempDirectory("game-result").toFile()
        try {
            val report = reportFor(directory)
            val name = ArtifactNames.gameApkFileName(report)
            assertEquals("Example-game.apk", name)
            assertEquals(name, GameRuntimeArtifactContract.validate(report, directory, name).name)
            assertThrows(IllegalArgumentException::class.java) {
                GameRuntimeArtifactContract.validate(report, directory, ArtifactNames.apkFileName(report))
            }
            assertThrows(IllegalArgumentException::class.java) {
                GameRuntimeArtifactContract.validate(report, directory, ArtifactNames.placeholderApkFileName(report))
            }
        } finally {
            directory.deleteRecursively()
        }
    }

    @Test fun refusesModifiedOrMisrepresentedGameRuntimeFiles() {
        val directory = Files.createTempDirectory("game-result").toFile()
        try {
            val report = reportFor(directory)
            val name = ArtifactNames.gameApkFileName(report)
            directory.resolve(name).writeText("changed")
            assertThrows(IllegalArgumentException::class.java) {
                GameRuntimeArtifactContract.validate(report, directory, name)
            }

            directory.resolve(name).writeBytes(gameBytes)
            report.getJSONObject("gameRuntimeConversion").put("completeGameConversion", true)
            assertThrows(IllegalArgumentException::class.java) {
                GameRuntimeArtifactContract.validate(report, directory, name)
            }

            report.getJSONObject("gameRuntimeConversion").put("completeGameConversion", false)
            report.getJSONObject("gameRuntimeConversion").put("gamePlayable", true)
            assertThrows(IllegalArgumentException::class.java) {
                GameRuntimeArtifactContract.validate(report, directory, name)
            }
        } finally {
            directory.deleteRecursively()
        }
    }

    private fun reportFor(directory: java.io.File): JSONObject {
        val hash = "a".repeat(64)
        val report = JSONObject()
            .put("source", JSONObject().put("sha256", hash).put("originalName", "Example.ipa"))
            .put("application", JSONObject().put("sha256", hash))
        val name = ArtifactNames.gameApkFileName(report)
        directory.resolve(name).writeBytes(gameBytes)
        val digest = MessageDigest.getInstance("SHA-256").digest(gameBytes)
            .joinToString("") { "%02x".format(it.toInt() and 0xff) }
        val certificateHash = "c".repeat(64)
        report.put("gameRuntimeConversion", JSONObject()
            .put("status", "GENERATED")
            .put("contract", "game-runtime-v1")
            .put("bootAttemptIncluded", true)
            .put("completeGameConversion", false)
            .put("gameCodeRecompiled", false)
            .put("gamePlayable", false)
            .put("installableAndroidPackage", true)
            .put("artifact", name)
            .put("package", "dev.radek.gameruntime.p${hash.take(20)}${certificateHash.take(8)}")
            .put("sourceSha256", hash)
            .put("sha256", digest)
            .put("signing", JSONObject().put("certificateSha256", certificateHash)))
        return report
    }

    private val gameBytes = "signed-APK-game-runtime-test-fixture".toByteArray()
}
