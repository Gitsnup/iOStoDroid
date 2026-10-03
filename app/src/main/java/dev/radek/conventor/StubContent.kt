package dev.radek.conventor

import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest
import java.util.Locale

/** Copies a bounded allowlist of non-executable bundle resources for the display-only stub. */
internal object StubContent {
    private const val MAX_FILES = 1200
    private const val MAX_TOTAL_BYTES = 64L * 1024 * 1024
    private const val MAX_FILE_BYTES = 16L * 1024 * 1024
    private const val MAX_CATALOG_BYTES = 48L * 1024 * 1024
    private val allowedExtensions = setOf(
        "png", "jpg", "jpeg", "webp", "gif", "bmp", "tif", "tiff", "heic", "heif",
        "mp3", "wav", "caf", "m4a", "aac", "ogg", "opus", "mp4", "mov",
        "json", "xml", "strings", "plist", "txt", "csv", "xcprivacy", "car",
        "ttf", "otf", "woff", "woff2", "bank", "wem", "fsb", "bytes",
        "metal", "glsl", "shader",
    )
    private val executableMagics = setOf(
        "cffaedfe", "cefaedfe", "feedface", "feedfacf", "cafebabe", "cafebabf",
        "bebafeca", "bfbafeca", "7f454c46",
    )

    fun collect(app: File, libraryEntry: File, mainExecutable: String, onProgress: (Int, Int) -> Unit = { _, _ -> }): JSONObject {
        val output = File(libraryEntry, "stub-content")
        val indexFile = File(libraryEntry, "stub-content-index.json")
        if (output.exists()) require(output.deleteRecursively()) { "cannot clear previous stub resource cache" }
        indexFile.delete()
        require(app.isDirectory && output.mkdirs()) { "cannot prepare bounded stub resource cache" }
        val index = JSONArray()
        var count = 0
        var total = 0L
        var skipped = 0
        try {
            val candidates = app.walkTopDown().filter { it.isFile }
                .sortedBy { it.relativeTo(app).invariantSeparatorsPath }.toList()
            for ((candidateIndex, source) in candidates.withIndex()) {
                if (candidateIndex % 32 == 0 || candidateIndex == candidates.lastIndex) onProgress(candidateIndex + 1, candidates.size)
                if (count >= MAX_FILES) { skipped += 1; continue }
                val relative = source.relativeTo(app).invariantSeparatorsPath
                if (relative == mainExecutable || relative.startsWith("_CodeSignature/") || relative.contains("/_CodeSignature/")) {
                    skipped += 1
                    continue
                }
                val extension = source.extension.lowercase(Locale.ROOT)
                if (extension !in allowedExtensions) continue
                val maxFile = if (extension == "car") MAX_CATALOG_BYTES else MAX_FILE_BYTES
                val size = source.length()
                if (size <= 0 || size > maxFile || size > MAX_TOTAL_BYTES - total) {
                    skipped += 1
                    continue
                }
                if (hasExecutableMagic(source)) {
                    skipped += 1
                    continue
                }
                val safePath = SafeZip.validateName(relative)
                val target = File(output, safePath)
                require(target.canonicalPath.startsWith(output.canonicalPath + File.separator)) { "unsafe stub resource path" }
                target.parentFile?.let { require(it.isDirectory || it.mkdirs()) }
                val digest = MessageDigest.getInstance("SHA-256")
                var copied = 0L
                source.inputStream().buffered().use { input ->
                    target.outputStream().buffered().use { destination ->
                        val buffer = ByteArray(64 * 1024)
                        while (true) {
                            val amount = input.read(buffer)
                            if (amount < 0) break
                            copied += amount
                            require(copied <= size && copied <= maxFile && total + copied <= MAX_TOTAL_BYTES) {
                                "bundle resource changed or exceeded the APK resource limit"
                            }
                            destination.write(buffer, 0, amount)
                            digest.update(buffer, 0, amount)
                        }
                    }
                }
                require(copied == size) { "bundle resource changed while caching" }
                total += copied
                count += 1
                val sha = digest.digest().joinToString("") { "%02x".format(it.toInt() and 0xff) }
                index.put(JSONObject()
                    .put("path", safePath)
                    .put("extension", extension)
                    .put("bytes", copied)
                    .put("sha256", sha))
            }
            if (count == 0) {
                output.deleteRecursively()
                indexFile.delete()
            } else indexFile.writeText(index.toString())
            return JSONObject()
                .put("status", if (skipped > 0) "PARTIAL" else if (count > 0) "COPIED" else "EMPTY")
                .put("fileCount", count)
                .put("bytes", total)
                .put("skippedCount", skipped)
                .put("indexPath", "stub-content-index.json")
                .put("notice", "Only bounded allowlisted non-executable bundle resources are copied. This data is not translated or played.")
        } catch (error: Throwable) {
            output.deleteRecursively()
            indexFile.delete()
            throw error
        }
    }

    fun isSafeCachedResource(file: File): Boolean {
        val extension = file.extension.lowercase(Locale.ROOT)
        val maximum = if (extension == "car") MAX_CATALOG_BYTES else MAX_FILE_BYTES
        return file.isFile && extension in allowedExtensions && file.length() in 1..maximum && !hasExecutableMagic(file)
    }

    private fun hasExecutableMagic(file: File): Boolean {
        val head = ByteArray(4)
        val read = file.inputStream().use { it.read(head) }
        return read == 4 && head.joinToString("") { "%02x".format(it.toInt() and 0xff) } in executableMagics
    }
}
