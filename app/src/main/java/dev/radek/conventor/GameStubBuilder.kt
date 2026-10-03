package dev.radek.conventor

import android.content.Context
import com.android.apksig.ApkSigner
import com.android.apksig.ApkVerifier
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedOutputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.security.KeyStore
import java.security.MessageDigest
import java.security.PrivateKey
import java.security.cert.X509Certificate
import java.util.zip.Deflater
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream

/** Builds a signed installable Android app stub branded with the imported game's icon. */
internal object GameStubBuilder {
    private const val BUFFER_SIZE = 64 * 1024
    private const val MAX_ICON_BYTES = 16L * 1024 * 1024
    private const val SIGNING_ALIAS = "androiddebugkey"
    private const val SIGNING_PASSWORD = "android"

    /** Output basename derived from the selected IPA filename, with path/control characters removed. */
    fun fileName(report: JSONObject): String {
        val original = report.optJSONObject("source")?.optString("originalName")?.takeIf { it.isNotBlank() }
            ?: report.optJSONObject("application")?.optString("name")?.takeIf { it.isNotBlank() }
            ?: "ConvertedIPA"
        val basename = original.replace('\\', '/').substringAfterLast('/')
        val stem = if (basename.endsWith(".ipa", ignoreCase = true)) basename.dropLast(4)
            else basename.substringBeforeLast('.', basename)
        val safe = stem.map { ch -> if (ch.isLetterOrDigit() || ch in " ._-") ch else '_' }
            .joinToString("").trim(' ', '.', '_', '-')
            .take(80).trim(' ', '.', '_', '-')
            .ifBlank { "ConvertedIPA" }
        return "$safe.apk"
    }

    fun create(
        context: Context,
        directory: File,
        report: JSONObject,
        progress: (Int, String) -> Unit,
    ): File {
        val source = File(directory, "source.ipa")
        require(source.isFile && source.length() in 1..SafeZip.MAX_ARCHIVE) { "the retained authorized IPA is unavailable" }
        val sourceInfo = report.optJSONObject("source")
        val expectedHash = sourceInfo?.optString("sha256")?.takeIf { it.isNotBlank() }
            ?: report.optJSONObject("application")?.optString("sha256")?.takeIf { it.isNotBlank() }
        require(expectedHash != null) { "the source IPA SHA-256 is missing from the report" }
        progress(0, "Starting signed APK build")
        var lastHashPercent = 0
        val actualHash = sha256(source) { copied, total ->
            val percent = (copied * 15 / total.coerceAtLeast(1L)).toInt().coerceIn(1, 15)
            if (percent > lastHashPercent) {
                lastHashPercent = percent
                progress(percent, "Verifying source IPA SHA-256 · ${copied / (1024 * 1024)} / ${total / (1024 * 1024)} MiB")
            }
        }
        require(actualHash == expectedHash) { "retained IPA does not match the recorded source hash" }
        progress(16, "Source hash verified")
        require(directory.isDirectory && directory.canonicalFile == directory.absoluteFile.canonicalFile) { "invalid APK output directory" }

        val artifactName = fileName(report)
        val work = File(directory, "game-stub-work")
        require(!work.exists() || work.deleteRecursively()) { "cannot clear previous game-stub work files" }
        require(work.mkdirs()) { "cannot create game-stub work directory" }
        val template = File(work, "game-stub.apk")
        val unsigned = File(work, "unsigned.apk")
        val signed = File(work, "signed.apk")
        val pending = File(directory, "$artifactName.pending")
        val output = File(directory, artifactName)
        pending.delete()

        try {
            context.assets.open("game-stub/game-stub.apk").use { input ->
                template.outputStream().buffered().use { output -> input.copyTo(output, BUFFER_SIZE) }
            }
            progress(20, "Loaded the compiled Android stub template")
            val application = report.optJSONObject("application") ?: JSONObject()
            val iconBytes = recoveredIconBytes(directory)
                ?: error("a valid game icon could not be decoded from the IPA; no generic-icon APK was created")
            progress(26, "Decoded and normalized the recovered IPA icon")
            val contentDirectory = File(directory, "stub-content")
            val contentIndexFile = File(directory, "stub-content-index.json")
            val contentIndex = if (contentIndexFile.isFile && contentIndexFile.length() <= 2L * 1024 * 1024) {
                JSONArray(contentIndexFile.readText())
            } else JSONArray()
            val contentFiles = if (contentDirectory.isDirectory) contentDirectory.walkTopDown().filter { it.isFile }
                .sortedBy { it.relativeTo(contentDirectory).invariantSeparatorsPath }.toList() else emptyList()
            require(contentFiles.size <= 1200 && contentFiles.size == contentIndex.length()) { "stub resource index does not match cached files" }
            val contentRoot = contentDirectory.canonicalPath + File.separator
            require(contentFiles.all { StubContent.isSafeCachedResource(it) && it.canonicalPath.startsWith(contentRoot) }) {
                "stub resource cache contains an unsafe, disallowed, or executable file"
            }
            val contentBytes = contentFiles.sumOf { it.length() }
            require(contentBytes <= 64L * 1024 * 1024) { "stub resource data exceeds the APK inclusion limit" }
            val sourceContentSummary = report.optJSONObject("stubContent") ?: JSONObject()
            val bundleContent = JSONObject()
                .put("status", sourceContentSummary.optString("status", if (contentFiles.isEmpty()) "EMPTY" else "COPIED"))
                .put("fileCount", contentFiles.size)
                .put("bytes", contentBytes)
                .put("skippedCount", sourceContentSummary.optInt("skippedCount", 0))
                .put("indexAsset", "assets/radek/content-index.json")
                .put("notice", "Allowlisted non-executable bundle resources only; they are not translated or played.")
            val metadata = JSONObject()
                .put("schemaVersion", 1)
                .put("artifactKind", "INSTALLABLE_GAME_ICON_STUB")
                .put("outputFileName", artifactName)
                .put("installableAndroidPackage", true)
                .put("gameCodeConverted", false)
                .put("originalName", sourceInfo?.optString("originalName") ?: "")
                .put("sourceSha256", expectedHash)
                .put("application", application)
                .put("bundleContent", bundleContent)
                .put("notice", "Installable Android placeholder only. It uses the recovered IPA icon and metadata, includes only allowlisted non-executable assets, and contains no original IPA or playable game code.")
            var launcherIconReplaced = false

            ZipFile(template).use { templateZip ->
                ZipOutputStream(BufferedOutputStream(unsigned.outputStream())).use { outputZip ->
                    outputZip.setLevel(Deflater.BEST_SPEED)
                    val names = mutableSetOf<String>()
                    val entries = templateZip.entries().toList().filterNot { isOldSignatureEntry(it.name) }
                    val rewriteBytes = entries.sumOf { entry ->
                        if (isLauncherIconEntry(entry.name)) iconBytes.size.toLong() else entry.size.coerceAtLeast(0L)
                    }.coerceAtLeast(1L)
                    var copiedBytes = 0L
                    var lastRewritePercent = 26
                    for (entry in entries) {
                        require(names.add(entry.name)) { "game-stub template has a duplicate ZIP entry" }
                        val replaceIcon = isLauncherIconEntry(entry.name)
                        val copy = ZipEntry(entry.name)
                        if (entry.time >= 0) copy.time = entry.time
                        when {
                            replaceIcon -> copy.method = ZipEntry.DEFLATED
                            entry.method == ZipEntry.STORED -> {
                                copy.method = ZipEntry.STORED
                                copy.size = entry.size
                                copy.compressedSize = entry.size
                                copy.crc = entry.crc
                            }
                            entry.method == ZipEntry.DEFLATED -> copy.method = ZipEntry.DEFLATED
                            else -> error("unsupported compression in game-stub template")
                        }
                        outputZip.putNextEntry(copy)
                        if (!entry.isDirectory) {
                            if (replaceIcon) {
                                outputZip.write(iconBytes)
                                launcherIconReplaced = true
                            } else templateZip.getInputStream(entry).use { it.copyTo(outputZip, BUFFER_SIZE) }
                        }
                        outputZip.closeEntry()
                        copiedBytes += if (replaceIcon) iconBytes.size.toLong() else entry.size.coerceAtLeast(0L)
                        val percent = (28 + copiedBytes * 38 / rewriteBytes).toInt().coerceIn(28, 66)
                        if (percent > lastRewritePercent) {
                            lastRewritePercent = percent
                            progress(percent, "Rewriting Android APK entries · ${copiedBytes / 1024} / ${rewriteBytes / 1024} KiB")
                        }
                    }
                    require(names.contains("AndroidManifest.xml") && names.contains("classes.dex")) {
                        "game-stub template is missing Android manifest or DEX code"
                    }
                    require(launcherIconReplaced) { "game-stub template has no replaceable launcher icon resource" }
                    writeText(outputZip, names, "assets/radek/game-stub.json", metadata.toString(2))
                    writeText(outputZip, names, "assets/radek/content-index.json", contentIndex.toString(2))
                    var copiedContent = 0L
                    var lastContentPercent = 70
                    for (index in contentFiles.indices) {
                        val file = contentFiles[index]
                        val relative = SafeZip.validateName(file.relativeTo(contentDirectory).invariantSeparatorsPath)
                        val item = contentIndex.getJSONObject(index)
                        require(item.optString("path") == relative && item.optLong("bytes") == file.length()) {
                            "stub resource index/file mismatch: $relative"
                        }
                        val entryPath = "assets/ipa-content/$relative"
                        require(names.add(entryPath)) { "duplicate IPA resource in stub: $relative" }
                        outputZip.putNextEntry(ZipEntry(entryPath))
                        val digest = MessageDigest.getInstance("SHA-256")
                        var bytes = 0L
                        file.inputStream().buffered().use { input ->
                            val buffer = ByteArray(BUFFER_SIZE)
                            while (true) {
                                val amount = input.read(buffer)
                                if (amount < 0) break
                                bytes += amount
                                require(bytes <= file.length() && copiedContent + bytes <= 64L * 1024 * 1024) {
                                    "stub resource changed or exceeded its inclusion limit"
                                }
                                digest.update(buffer, 0, amount)
                                outputZip.write(buffer, 0, amount)
                                val percent = if (contentBytes > 0) (70 + (copiedContent + bytes) * 8 / contentBytes).toInt().coerceIn(70, 78) else 78
                                if (percent > lastContentPercent) {
                                    lastContentPercent = percent
                                    progress(percent, "Including safe bundle data · ${(copiedContent + bytes) / (1024 * 1024)} MiB")
                                }
                            }
                        }
                        outputZip.closeEntry()
                        require(bytes == file.length()) { "stub resource changed while packaging: $relative" }
                        val hash = digest.digest().joinToString("") { "%02x".format(it.toInt() and 0xff) }
                        require(hash == item.optString("sha256")) { "stub resource hash mismatch: $relative" }
                        copiedContent += bytes
                    }
                    progress(78, "Included ${contentFiles.size} safe bundle resource file(s); no code was copied")
                }
            }

            progress(80, "Preparing APK signing certificate")
            val keyStore = KeyStore.getInstance("JKS")
            context.assets.open("game-stub/debug.keystore").use { keyStore.load(it, SIGNING_PASSWORD.toCharArray()) }
            val privateKey = keyStore.getKey(SIGNING_ALIAS, SIGNING_PASSWORD.toCharArray()) as? PrivateKey
                ?: error("development APK signing key is missing")
            val chain = keyStore.getCertificateChain(SIGNING_ALIAS)?.map { it as X509Certificate }
                ?: error("development APK signing certificate is missing")
            val signer = ApkSigner.SignerConfig.Builder("Radek development signer", privateKey, chain).build()
            ApkSigner.Builder(listOf(signer))
                .setInputApk(unsigned)
                .setOutputApk(signed)
                .setMinSdkVersion(26)
                .setV1SigningEnabled(true)
                .setV2SigningEnabled(true)
                .setV3SigningEnabled(false)
                .build()
                .let { signer ->
                    progress(83, "Signing the IPA-icon-branded Android APK")
                    signer.sign()
                }

            progress(89, "Verifying APK signature and installable entries")
            val verification = ApkVerifier.Builder(signed).build().verify()
            require(verification.isVerified) { "generated game-stub APK signature did not verify" }
            ZipFile(signed).use { apk ->
                require(apk.getEntry("AndroidManifest.xml") != null && apk.getEntry("classes.dex") != null) {
                    "generated file is missing Android package entries"
                }
                require(apk.getEntry("assets/radek/game-stub.json") != null && apk.getEntry("assets/radek/content-index.json") != null) {
                    "generated stub metadata or resource index is missing"
                }
                val packagedEntries = apk.entries()
                while (packagedEntries.hasMoreElements()) {
                    require(!packagedEntries.nextElement().name.endsWith(".ipa", ignoreCase = true)) {
                        "game-stub APK must not contain the original IPA"
                    }
                }
            }
            progress(95, "Staging the signature-verified APK for publication")
            require(signed.copyTo(pending, overwrite = true).length() > 0) { "signed APK could not be staged" }
            if (output.exists()) require(output.delete()) { "cannot replace the previous output APK" }
            require(pending.renameTo(output)) { "cannot publish generated APK" }
            progress(100, "Installable icon-branded APK stub created; it contains no game code")
            return output
        } catch (error: Throwable) {
            pending.delete()
            throw error
        } finally {
            work.deleteRecursively()
        }
    }

    private fun recoveredIconBytes(directory: File): ByteArray? {
        val iconFile = File(directory, "icon.png")
        if (!iconFile.isFile || iconFile.length() !in 1..MAX_ICON_BYTES) return null
        val bitmap = IconDecoder.decode(iconFile, 512) ?: return null
        return try {
            ByteArrayOutputStream().use { output ->
                require(bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, output)) { "cannot encode recovered launcher icon" }
                output.toByteArray()
            }
        } finally { bitmap.recycle() }
    }

    private fun isLauncherIconEntry(name: String): Boolean =
        name.startsWith("res/drawable") && name.endsWith("/game_icon.png")

    private fun isOldSignatureEntry(name: String): Boolean =
        name.equals("META-INF", ignoreCase = true) || name.startsWith("META-INF/", ignoreCase = true)

    private fun writeText(zip: ZipOutputStream, names: MutableSet<String>, path: String, text: String) {
        require(names.add(path)) { "duplicate game-stub APK entry: $path" }
        zip.putNextEntry(ZipEntry(path))
        zip.write(text.toByteArray(Charsets.UTF_8))
        zip.closeEntry()
    }

    private fun sha256(file: File, onProgress: (Long, Long) -> Unit = { _, _ -> }): String {
        val digest = MessageDigest.getInstance("SHA-256")
        val total = file.length()
        var copied = 0L
        file.inputStream().buffered().use { input ->
            val buffer = ByteArray(BUFFER_SIZE)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                digest.update(buffer, 0, count)
                copied += count
                onProgress(copied, total)
            }
        }
        require(copied == total) { "source file changed while verifying its hash" }
        return digest.digest().joinToString("") { "%02x".format(it.toInt() and 0xff) }
    }
}
