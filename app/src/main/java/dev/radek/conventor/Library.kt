package dev.radek.conventor

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest
import java.util.UUID

object NativeBridge {
    init { System.loadLibrary("radek") }
    external fun analyze(bytes: ByteArray): String
}

enum class ConversionState { IMPORTED, ANALYZING, CONVERTING, PACKAGING, VALIDATING, READY, PARTIAL, BLOCKED, FAILED }

data class WorkflowProgress(val percent: Int, val stage: String, val message: String, val status: String = "RUNNING") {
    fun toJson(): JSONObject = JSONObject()
        .put("percent", percent.coerceIn(0, 100))
        .put("stage", stage)
        .put("message", message)
        .put("status", status)
        .put("basis", "completed workflow stages; not a playable-code percentage")
        .put("updatedAt", java.time.Instant.now().toString())
}

/** Persistent private library. Android imports/analyzes; compilation uses the host CLI. */
class Library(private val context: Context) {
    val root = File(context.filesDir, "library").apply { mkdirs() }
    fun entries(): List<Pair<File, JSONObject>> = root.listFiles().orEmpty().filter { it.isDirectory }.mapNotNull { dir ->
        try { dir to JSONObject(File(dir, "report.json").readText()) } catch (_: Exception) { null }
    }.sortedByDescending { it.first.name }

    fun save(dir: File, report: JSONObject) {
        val temporary = File(dir, "report.json.tmp")
        temporary.writeText(report.toString(2))
        require(temporary.renameTo(File(dir, "report.json"))) { "cannot persist report" }
    }

    fun recoverInterrupted() {
        root.listFiles().orEmpty().filter { it.isDirectory && !File(it, "report.json").isFile }.forEach { it.deleteRecursively() }
        entries().forEach { (dir, report) ->
            var changed = false
            if (report.optString("state") in listOf("IMPORTED", "ANALYZING", "CONVERTING", "PACKAGING", "VALIDATING")) {
                val failureMessage = "Process ended before work completed; inspect the last saved stage or retry"
                report.put("state", "FAILED").put("error", failureMessage)
                val savedProgress = report.optJSONObject("workflowProgress")
                val interruptedStage = savedProgress?.optString("stage")?.takeIf { it.isNotBlank() } ?: "workflow"
                report.put("workflowProgress", WorkflowProgress(
                    savedProgress?.optInt("percent", 0) ?: 0,
                    "Interrupted during $interruptedStage",
                    failureMessage,
                    "FAILED",
                ).toJson())
                File(dir, "extracted").deleteRecursively()
                val savedHash = report.optJSONObject("source")?.optString("sha256").orEmpty()
                if (!savedHash.matches(Regex("[0-9a-f]{64}"))) File(dir, "source.ipa").delete()
                changed = true
            }
            val forced = report.optJSONObject("forceConversion")
            if (forced?.optString("status") == "PACKAGING") {
                forced.put("status", "FAILED").put("error", "Process stopped while building the installable game-stub APK")
                File(dir, "${GameStubBuilder.fileName(report)}.pending").delete()
                changed = true
            }
            val automatic = report.optJSONObject("automaticPackage")
            if (automatic?.optString("status") == "PACKAGING") {
                automatic.put("status", "FAILED").put("error", "Process stopped while building the game-stub APK")
                File(dir, "${GameStubBuilder.fileName(report)}.pending").delete()
                changed = true
            }
            if (forced?.optString("status") == "FAILED" || automatic?.optString("status") == "FAILED") {
                val savedProgress = report.optJSONObject("workflowProgress")
                if (savedProgress?.optString("status") == "RUNNING") {
                    val failure = WorkflowProgress(
                        savedProgress.optInt("percent", 0),
                        "APK build interrupted",
                        forced?.optString("error")?.takeIf { it.isNotBlank() }
                            ?: automatic?.optString("error")?.takeIf { it.isNotBlank() }
                            ?: "The APK build stopped before completion",
                        "FAILED",
                    )
                    report.put("workflowProgress", failure.toJson())
                    changed = true
                }
            }
            if (forced?.optString("status") == "FAILED" || automatic?.optString("status") == "FAILED") {
                File(dir, "game-stub-work").deleteRecursively()
            }
            if (changed) save(dir, report)
        }
    }

    private fun sourceDetails(uri: Uri): Pair<String, Long?> {
        val details = try {
            context.contentResolver.query(
                uri,
                arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE),
                null,
                null,
                null,
            )?.use { cursor ->
                if (!cursor.moveToFirst()) null else {
                    val nameColumn = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    val sizeColumn = cursor.getColumnIndex(OpenableColumns.SIZE)
                    val name = if (nameColumn >= 0) cursor.getString(nameColumn) else null
                    val size = if (sizeColumn >= 0 && !cursor.isNull(sizeColumn)) cursor.getLong(sizeColumn) else null
                    name to size
                }
            }
        } catch (_: Exception) { null }
        val name = details?.first?.takeIf { it.isNotBlank() }
            ?: uri.lastPathSegment?.substringAfterLast('/')?.takeIf { it.isNotBlank() }
            ?: "converted.ipa"
        return name to details?.second?.takeIf { it > 0 }
    }

    fun import(uri: Uri, progress: (WorkflowProgress) -> Unit): Pair<File, JSONObject> {
        val (originalName, declaredSize) = sourceDetails(uri)
        val dir = File(root, "${System.currentTimeMillis()}-${UUID.randomUUID()}").apply { check(mkdir()) }
        val events = JSONArray()
        val report = JSONObject().put("schemaVersion", 1).put("authorizationConfirmed", true).put("events", events)
        val source = File(dir, "source.ipa")
        var sourceReady = false
        var currentPercent = 0
        var lastStage = "Import"
        var lastMessage = "Preparing IPA import"
        fun updateProgress(percent: Int, stage: String, message: String, status: String = "RUNNING") {
            val next = maxOf(currentPercent, percent.coerceIn(0, 100))
            if (next == currentPercent && stage == lastStage && message == lastMessage && status == report.optJSONObject("workflowProgress")?.optString("status")) return
            currentPercent = next
            lastStage = stage
            lastMessage = message
            val snapshot = WorkflowProgress(currentPercent, stage, message, status)
            report.put("workflowProgress", snapshot.toJson())
            save(dir, report)
            progress(snapshot)
        }
        fun log(state: ConversionState, message: String) {
            report.put("state", state.name)
            val event = JSONObject().put("time", java.time.Instant.now().toString()).put("stage", state.name).put("message", message)
            events.put(event); File(dir, "conversion.jsonl").appendText(event.toString() + "\n")
            save(dir, report)
            val stagePercent = when (state) {
                ConversionState.IMPORTED -> 1
                ConversionState.ANALYZING -> 32
                ConversionState.CONVERTING -> 65
                ConversionState.PACKAGING -> 75
                ConversionState.VALIDATING -> 92
                ConversionState.READY -> 100
                ConversionState.PARTIAL, ConversionState.BLOCKED -> 65
                ConversionState.FAILED -> currentPercent
            }
            val status = when (state) {
                ConversionState.FAILED -> "FAILED"
                ConversionState.BLOCKED -> "BLOCKED"
                ConversionState.PARTIAL -> "ANALYSIS_COMPLETE"
                else -> "RUNNING"
            }
            updateProgress(stagePercent, state.name.lowercase().replaceFirstChar { it.uppercase() }, message, status)
        }
        try {
            log(ConversionState.IMPORTED, "Copying selected IPA into isolated private storage")
            val digest = MessageDigest.getInstance("SHA-256")
            context.contentResolver.openInputStream(uri).use { input ->
                requireNotNull(input) { "cannot open selected document" }
                source.outputStream().use { output ->
                    val buffer = ByteArray(65536)
                    var total = 0L
                    var lastReport = 0L
                    while (true) {
                        val count = input.read(buffer)
                        if (count < 0) break
                        total += count
                        require(total <= SafeZip.MAX_ARCHIVE) { "IPA exceeds 512 MiB" }
                        digest.update(buffer, 0, count)
                        output.write(buffer, 0, count)
                        if (total - lastReport >= 1024 * 1024 || (declaredSize != null && total >= declaredSize)) {
                            val percent = declaredSize?.let { (1 + total * 10 / it).toInt().coerceIn(1, 11) } ?: currentPercent
                            updateProgress(percent, "Copying IPA", "Copied ${total / (1024 * 1024)} MiB of the source file")
                            lastReport = total
                        }
                    }
                }
            }
            val hash = digest.digest().joinToString("") { "%02x".format(it.toInt() and 255) }
            report.put("source", JSONObject().put("path", "source.ipa").put("originalName", originalName)
                .put("sha256", hash).put("bytes", source.length()))
            save(dir, report)
            sourceReady = true
            updateProgress(12, "Source verified", "Copied ${source.length() / (1024 * 1024)} MiB and calculated the IPA SHA-256")
            SafeZip.extract(source, File(dir, "extracted")) { done, total ->
                val percent = if (total > 0) 12 + (done * 18 / total) else 30
                updateProgress(percent, "Extracting IPA", "Validated $done of $total archive entries")
            }
            log(ConversionState.ANALYZING, "Reading Info.plist and application icon")
            val apps = File(dir, "extracted/Payload").listFiles().orEmpty().filter { it.isDirectory && it.name.endsWith(".app") }
            require(apps.size == 1) { "expected exactly one Payload/*.app" }
            val app = apps.single()
            val plistFile = File(app, "Info.plist"); require(plistFile.length() <= 8 * 1024 * 1024)
            val plist = Plist.read(plistFile.readBytes())
            val executable = plist["CFBundleExecutable"] as? String ?: error("CFBundleExecutable missing")
            require(SafeZip.validateName(executable) == executable && '/' !in executable)
            val bundle = plist["CFBundleIdentifier"] as? String ?: error("CFBundleIdentifier missing")
            require(bundle.isNotBlank())
            val minimumIOSVersion = (plist["MinimumOSVersion"] as? String)?.takeIf { it.isNotBlank() }.orEmpty()
            report.put("application", JSONObject().put("name", plist["CFBundleDisplayName"] ?: plist["CFBundleName"] ?: executable)
                .put("bundleId", bundle).put("version", plist["CFBundleShortVersionString"] ?: "")
                .put("build", plist["CFBundleVersion"] ?: "").put("executable", executable)
                .put("minimumIOSVersion", minimumIOSVersion).put("fileSize", source.length()).put("sha256", hash))
            val names = mutableListOf<String>()
            for (key in listOf("CFBundleIcons", "CFBundleIcons~ipad")) {
                val icons = plist[key] as? Map<*, *>
                val primary = icons?.get("CFBundlePrimaryIcon") as? Map<*, *>
                (primary?.get("CFBundleIconName") as? String)?.let { names.add(it) }
                names.addAll((primary?.get("CFBundleIconFiles") as? List<*>)?.filterIsInstance<String>().orEmpty())
            }
            names.addAll((plist["CFBundleIconFiles"] as? List<*>)?.filterIsInstance<String>().orEmpty())
            (plist["CFBundleIconFile"] as? String)?.let { names.add(it) }
            val iconReport = extractIcon(app, names, dir)
            report.put("icon", iconReport)
            updateProgress(40, "Bundle assets", if (iconReport.optString("status") == "SUPPORTED") "Recovered the game's launcher icon" else "No compatible icon could be decoded")
            val stubContent = try {
                StubContent.collect(app, dir, executable) { done, total ->
                    val percent = if (total > 0) 41 + (done * 3 / total) else 44
                    updateProgress(percent, "Safe bundle content", "Checking $done of $total resources for non-executable content")
                }
            } catch (resourceError: Exception) {
                JSONObject().put("status", "UNAVAILABLE").put("fileCount", 0).put("bytes", 0)
                    .put("notice", "Safe resource copy failed: ${resourceError.message}")
            }
            report.put("stubContent", stubContent)
            updateProgress(44, "Bundle content", "Cached ${stubContent.optInt("fileCount", 0)} allowed non-executable resource file(s)")
            val binary = File(app, executable)
            require(binary.isFile && binary.length() <= 64 * 1024 * 1024) { "Missing executable or exceeds the on-device 64 MiB analysis limit; use the host analyzer for larger files" }
            updateProgress(44, "Mach-O analysis", "Reading architecture, symbols, imports and loader metadata")
            log(ConversionState.ANALYZING, "Parsing Mach-O load commands, symbols, fixups and dependencies")
            val macho = JSONObject(NativeBridge.analyze(binary.readBytes()))
            report.put("machO", macho)
            updateProgress(53, "Mach-O analysis", "Primary executable analysis completed")
            val graph = JSONArray(); val nodes = JSONArray()
            var encrypted = false; var incompatible = false
            var hasCandidate = false
            fun inspect(file: File, analysis: JSONObject) {
                nodes.put(JSONObject().put("path", file.relativeTo(app).path).put("analysis", analysis))
                val slices = analysis.getJSONArray("slices")
                for (index in 0 until slices.length()) {
                    val slice = slices.getJSONObject(index)
                    encrypted = encrypted || slice.getBoolean("encrypted")
                    val arch = slice.getString("architecture")
                    if (file == binary && arch in listOf("arm64", "armv7", "armv7s", "armv6")) hasCandidate = true
                    val deps = slice.getJSONArray("dependencies")
                    for (d in 0 until deps.length()) graph.put(JSONObject().put("from", file.relativeTo(app).path)
                        .put("installName", deps.getJSONObject(d).getString("path")).put("classification", "unverified")
                        .put("reason", "Linked dependency; this on-device report cannot prove API reachability and ships no Darwin ABI provider"))
                    incompatible = incompatible || deps.length() > 0 || slice.getJSONArray("imports").length() > 0 || slice.getJSONArray("metadata").length() > 0 || slice.has("chainedFixups") || !slice.optBoolean("bindDecodingComplete", true)
                }
            }
            inspect(binary, macho)
            val magics = setOf("cffaedfe", "cefaedfe", "feedface", "feedfacf", "cafebabe", "cafebabf", "bebafeca", "bfbafeca")
            app.walkTopDown().filter { it.isFile && it != binary }.forEach { file ->
                val head = ByteArray(4)
                val size = file.inputStream().use { it.read(head) }
                if (size == 4 && head.joinToString("") { "%02x".format(it.toInt() and 255) } in magics) {
                    require(file.length() <= 64 * 1024 * 1024) { "Embedded executable exceeds 64 MiB on-device limit" }
                    inspect(file, JSONObject(NativeBridge.analyze(file.readBytes())))
                    incompatible = true
                }
            }
            report.put("dependencies", JSONObject().put("nodes", nodes).put("edges", graph))
            updateProgress(57, "Dependencies", "Scanned ${nodes.length()} Mach-O image(s) and ${graph.length()} linked dependency edge(s)")
            val apiMapping = AndroidApiMapper.analyze(nodes)
            report.put("apiMapping", apiMapping)
            updateProgress(62, "Conversion assessment", "API candidates inventoried; no game code is emitted by the on-device backend")
            report.put("portProgress", JSONObject()
                .put("percent", 0)
                .put("status", "NO_RUNNABLE_ANDROID_CODE_BUILT")
                .put("basis", "On-device importer analyzed the IPA but emitted no Android executable code; this is actual output progress, not a stability prediction."))
            log(ConversionState.ANALYZING,
                "Inventoried ${apiMapping.getInt("distinctImportSymbols")} unique imported API symbols; " +
                    "${apiMapping.getInt("mappedNameCandidates")} have a same-named Bionic candidate. This is not binary/API conversion.")
            val reason = when {
                encrypted -> "Protected/encrypted Mach-O. Conversion prohibited; no DRM or FairPlay bypass."
                !hasCandidate -> "No supported ARM64/ARMv7/ARMv6 slice. ARM64e PAC reconstruction is blocked."
                incompatible -> "Frameworks, imports, incomplete dyld bindings, metadata or embedded code require unsupported compatibility/linker implementations."
                else -> "Analysis completed. A host SDK/NDK is required to prove the restricted leaf subset, reconstruct native code and package an APK. On-device compilation is not implemented."
            }
            report.put("blockers", JSONArray().put(reason)).put("hostCommand", "python3 -m radek convert input.ipa --authorized --output workspace/result")
            log(if (encrypted || incompatible || !hasCandidate) ConversionState.BLOCKED else ConversionState.PARTIAL, reason)
        } catch (e: Exception) {
            report.put("error", "${e.javaClass.simpleName}: ${e.message}")
            log(ConversionState.FAILED, e.message ?: "Import failed")
        } finally {
            if (!sourceReady) source.delete()
            File(dir, "extracted").deleteRecursively()
        }
        return dir to report
    }
}

/** Icon suffixes, highest scale first: the best available representation wins. */
private val ICON_SUFFIXES = listOf(
    "@3x.png", "@2x.png", ".png", "@3x~ipad.png", "@2x~ipad.png", "~ipad.png",
    "@3x~iphone.png", "@2x~iphone.png", "~iphone.png",
    "@3x.jpg", "@2x.jpg", ".jpg", "@3x.jpeg", "@2x.jpeg", ".jpeg", ""
)
private const val ICON_MAX_BYTES = 16L * 1024 * 1024
private const val ICON_TARGET = 512

/**
 * Resolve the best icon in a bundle and record every attempt.
 * Order: Info.plist names -> scale/device variants -> icon-like bundle images ->
 * any other image. Nothing is invented: when nothing decodes the status is
 * UNAVAILABLE and the library shows that state.
 */
internal fun extractIcon(app: File, names: List<String>, dir: File): JSONObject {
    val attempts = JSONArray()
    fun attempt(source: String, ok: Boolean, detail: String, width: Int = 0, height: Int = 0) {
        attempts.put(JSONObject().put("source", source).put("ok", ok).put("detail", detail)
            .put("width", width).put("height", height))
    }
    val candidates = mutableListOf<File>()
    val seen = mutableSetOf<String>()
    fun addCandidate(file: File) {
        if (!file.isFile) return
        val key = file.relativeTo(app).path.lowercase(java.util.Locale.ROOT)
        if (seen.add(key)) candidates.add(file)
    }

    // Info.plist names are authoritative. If a name includes an extension, try
    // scale variants of its basename (Icon.png -> Icon@3x.png) before the exact
    // fallback. This also keeps icon choice independent of the Mach-O CPU slices.
    for (rawName in names) {
        val name = rawName.trim()
        if (name.isEmpty()) continue
        SafeZip.validateName(name) // fail closed on traversal or absolute names
        val lower = name.lowercase(java.util.Locale.ROOT)
        val extension = listOf(".png", ".jpg", ".jpeg").firstOrNull { lower.endsWith(it) }
        val base = if (extension == null) name else name.dropLast(extension.length)
        for (suffix in ICON_SUFFIXES) addCandidate(File(app, base + suffix))
        addCandidate(File(app, name))
    }

    fun saveBitmap(
        bitmap: android.graphics.Bitmap,
        source: String,
        format: String,
        decoder: String,
        scale: Double,
        kind: String,
    ): JSONObject {
        val width = bitmap.width
        val height = bitmap.height
        File(dir, "icon.png").outputStream().use {
            require(bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it)) { "cannot save recovered icon" }
        }
        bitmap.recycle()
        return JSONObject().put("status", "SUPPORTED")
            .put("source", source).put("path", "icon.png").put("kind", kind)
            .put("format", format).put("decoder", decoder)
            .put("width", width).put("height", height).put("scale", scale)
            .put("reason", "Decoded bundle icon ($source, ${width}x${height})")
            .put("attempts", attempts)
    }

    fun tryFiles(files: List<File>): JSONObject? {
        for (candidate in files.take(48)) {
            val relative = candidate.relativeTo(app).path
            if (candidate.length() > ICON_MAX_BYTES) {
                attempt(relative, false, "image exceeds size limit")
                continue
            }
            val applePng = IconDecoder.isCgbi(candidate)
            val bitmap = try { IconDecoder.decode(candidate, ICON_TARGET) } catch (_: Exception) { null }
            if (bitmap == null) {
                attempt(relative, false, "unsupported or corrupt image; tried Android PNG/JPEG and Apple CgBI decoders")
                continue
            }
            attempt(relative, true, if (applePng) "decoded and normalized Apple CgBI channel order/alpha" else "decoded image", bitmap.width, bitmap.height)
            val scale = when { "@3x" in candidate.name -> 3.0; "@2x" in candidate.name -> 2.0; else -> 1.0 }
            val format = when {
                applePng -> "cgbi-png"
                candidate.name.endsWith(".jpeg", true) -> "jpeg"
                candidate.name.endsWith(".jpg", true) -> "jpeg"
                else -> "png"
            }
            val decoder = if (applePng) "radek-cgbi+android.graphics.Bitmap" else "android.graphics.BitmapFactory"
            return saveBitmap(bitmap, relative, format, decoder, scale, "file")
        }
        return null
    }

    // Prefer the explicit bundle icon, but tolerate a broken or missing file.
    // This pass occurs before reading the executable, so arm32, arm64 and FAT
    // archives all follow the same icon path.
    tryFiles(candidates)?.let { return it }

    // Modern iOS games commonly keep their only app icon in a compiled asset
    // catalog. Try it before arbitrary bundle textures/screenshots.
    val preferred = names.firstOrNull()?.substringBeforeLast('.', names.firstOrNull().orEmpty())
    val catalogs = app.walkTopDown().filter {
        it.isFile && (it.name.equals("Assets.car", ignoreCase = true) || it.extension.equals("car", ignoreCase = true))
    }.sortedWith(compareBy<File>({ if (it.name.equals("Assets.car", ignoreCase = true)) 0 else 1 }, { it.path }))
    for (catalog in catalogs.take(8)) {
        val extracted = AssetCatalogIcon.extract(catalog, preferred, ICON_TARGET)
        val prefix = catalog.relativeTo(app).path
        for (item in extracted.attempts) {
            attempt("$prefix:${item.asset}", item.ok, item.detail)
        }
        if (extracted.bitmap != null) {
            val source = extracted.asset ?: prefix
            attempt(source, true, "decoded compiled asset-catalog rendition", extracted.bitmap.width, extracted.bitmap.height)
            return saveBitmap(
                extracted.bitmap,
                source,
                extracted.format ?: "asset-catalog-image",
                "assetcatalog+android.graphics.Bitmap",
                extracted.scale,
                "assets.car",
            )
        }
        attempt(prefix, false, extracted.error ?: "no usable icon rendition")
    }

    // Some games ship the launch artwork as an unlisted loose resource. Rank
    // icon-like filenames above backgrounds and generic textures, then try all
    // image files. `iTunesArtwork` is often extensionless, so the decoder sniffs
    // its actual file signature rather than trusting the suffix.
    val fallbackImages = app.walkTopDown()
        .filter {
            it.isFile && (it.extension.lowercase() in setOf("png", "jpg", "jpeg") || it.name.equals("iTunesArtwork", true))
        }
        .sortedWith(compareBy<File>({
            val lower = it.name.lowercase()
            when {
                "appicon" in lower || lower.startsWith("itunesartwork") -> 0
                "icon" in lower -> 1
                "artwork" in lower || "logo" in lower -> 2
                else -> 3
            }
        }, { -it.length() }, { it.path }))
    tryFiles(fallbackImages)?.let { return it }

    return JSONObject().put("status", "UNAVAILABLE")
        .put("reason", "Icon unavailable: no decodable icon image was found in the bundle")
        .put("decoder", "android.graphics.BitmapFactory")
        .put("attempts", attempts)
}
