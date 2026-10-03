package dev.radek.conventor

import android.content.Context
import android.net.Uri
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

    /** Run the on-device conversion for one library entry. */
    fun convert(dir: File, force: Boolean, progress: (Int, String) -> Unit): JSONObject {
        val report = JSONObject(File(dir, "report.json").readText())
        report.put("state", ConversionState.CONVERTING.name)
        save(dir, report)
        return try {
            val result = Converter.convert(context, dir, report, force) { percent, message ->
                report.put("percent", percent)
                progress(percent, message)
            }
            report.put("apk", result)
            report.put("percent", 100)
            report.put("state", if (result.optBoolean("complete")) ConversionState.READY.name else ConversionState.PARTIAL.name)
            report.put("blockers", JSONArray().apply {
                if (!result.optBoolean("complete")) {
                    put("Android provider coverage is ${result.optInt("supportPercent")}%; " +
                        (result.optJSONArray("missing")?.let { m -> (0 until m.length()).joinToString(", ") { m.getString(it) } } ?: ""))
                }
                if (result.optBoolean("forced")) put("Forced conversion: the entry leaf is not inside the proved closed-integer subset")
            })
            save(dir, report)
            report
        } catch (error: Exception) {
            report.put("state", ConversionState.FAILED.name)
            report.put("error", "${error.javaClass.simpleName}: ${error.message}")
            save(dir, report)
            throw error
        }
    }

    fun recoverInterrupted() {
        root.listFiles().orEmpty().filter { it.isDirectory && !File(it, "report.json").isFile }.forEach { it.deleteRecursively() }
        entries().forEach { (dir, report) ->
            if (report.optString("state") in listOf("IMPORTED", "ANALYZING", "CONVERTING", "PACKAGING", "VALIDATING")) {
                report.put("state", "FAILED").put("error", "Process ended before work completed; reimport to retry")
                File(dir, "source.ipa").delete(); File(dir, "extracted").deleteRecursively()
                save(dir, report)
            }
        }
    }

    fun import(uri: Uri, progress: (String) -> Unit): Pair<File, JSONObject> {
        val dir = File(root, "${System.currentTimeMillis()}-${UUID.randomUUID()}").apply { check(mkdir()) }
        val events = JSONArray()
        val report = JSONObject().put("schemaVersion", 1).put("authorizationConfirmed", true).put("events", events)
        fun log(state: ConversionState, message: String) {
            report.put("state", state.name)
            val event = JSONObject().put("time", java.time.Instant.now().toString()).put("stage", state.name).put("message", message)
            events.put(event); File(dir, "conversion.jsonl").appendText(event.toString() + "\n")
            save(dir, report); progress(message)
        }
        try {
            log(ConversionState.IMPORTED, "Copying selected IPA into isolated private storage")
            val source = File(dir, "source.ipa")
            val digest = MessageDigest.getInstance("SHA-256")
            context.contentResolver.openInputStream(uri).use { input ->
                requireNotNull(input) { "cannot open selected document" }
                source.outputStream().use { output ->
                    val buffer = ByteArray(65536); var total = 0L
                    while (true) { val n = input.read(buffer); if (n < 0) break; total += n; require(total <= SafeZip.MAX_ARCHIVE) { "IPA exceeds 512 MiB" }; digest.update(buffer, 0, n); output.write(buffer, 0, n) }
                }
            }
            val hash = digest.digest().joinToString("") { "%02x".format(it.toInt() and 255) }
            SafeZip.extract(source, File(dir, "extracted")) { done, total -> if (done == total || done % 100 == 0) progress("Extracted $done / $total archive entries") }
            log(ConversionState.ANALYZING, "Reading Info.plist and application icon")
            val apps = File(dir, "extracted/Payload").listFiles().orEmpty().filter { it.isDirectory && it.name.endsWith(".app") }
            require(apps.size == 1) { "expected exactly one Payload/*.app" }
            val app = apps.single()
            val plistFile = File(app, "Info.plist"); require(plistFile.length() <= 8 * 1024 * 1024)
            val plist = Plist.read(plistFile.readBytes())
            val executable = plist["CFBundleExecutable"] as? String ?: error("CFBundleExecutable missing")
            require(SafeZip.memberName(executable) == executable && '/' !in executable) { "unsafe executable name" }
            val bundle = plist["CFBundleIdentifier"] as? String ?: error("CFBundleIdentifier missing")
            require(bundle.isNotBlank())
            report.put("application", JSONObject().put("name", plist["CFBundleDisplayName"] ?: plist["CFBundleName"] ?: executable)
                .put("bundleId", bundle).put("version", plist["CFBundleShortVersionString"] ?: "")
                .put("build", plist["CFBundleVersion"] ?: "").put("executable", executable).put("fileSize", source.length()).put("sha256", hash))
            val names = mutableListOf<String>()
            for (key in listOf("CFBundleIcons", "CFBundleIcons~ipad")) {
                val icons = plist[key] as? Map<*, *>
                val primary = icons?.get("CFBundlePrimaryIcon") as? Map<*, *>
                (primary?.get("CFBundleIconName") as? String)?.let { names.add(it) }
                names.addAll((primary?.get("CFBundleIconFiles") as? List<*>)?.filterIsInstance<String>().orEmpty())
            }
            names.addAll((plist["CFBundleIconFiles"] as? List<*>)?.filterIsInstance<String>().orEmpty())
            (plist["CFBundleIconFile"] as? String)?.let { names.add(it) }
            val iconReport = Icons.recover(app, names, dir,
                (report.optJSONObject("application")?.optString("name") ?: executable), JSONArray())
            report.put("icon", iconReport)
            val binary = File(app, executable)
            require(binary.isFile && binary.length() <= 64 * 1024 * 1024) { "Missing executable or exceeds the on-device 64 MiB analysis limit; use the host analyzer for larger files" }
            // The IPA and the extracted tree are discarded after import, but the
            // Mach-O image itself is retained: on-device conversion needs the real
            // entry-point bytes and nothing else from the archive.
            binary.copyTo(File(dir, "binary.macho"), overwrite = true)
            log(ConversionState.ANALYZING, "Parsing Mach-O load commands, symbols, fixups and dependencies")
            val macho = JSONObject(NativeBridge.analyze(binary.readBytes()))
            report.put("machO", macho)
            val graph = JSONArray(); val nodes = JSONArray()
            var encrypted = false; var incompatible = false
            var incompleteBindings = false
            var hasCandidate = false
            fun inspect(file: File, analysis: JSONObject) {
                nodes.put(JSONObject().put("path", file.relativeTo(app).path).put("analysis", analysis))
                val slices = analysis.getJSONArray("slices")
                for (index in 0 until slices.length()) {
                    val slice = slices.getJSONObject(index)
                    encrypted = encrypted || slice.getBoolean("encrypted")
                    val arch = slice.getString("architecture")
                    if (file == binary && (arch == "arm64" || arch in Ir.ARM32)) hasCandidate = true
                    if (!slice.optBoolean("bindDecodingComplete", true)) {
                        incompleteBindings = true
                    }
                    val deps = slice.getJSONArray("dependencies")
                    for (d in 0 until deps.length()) {
                        val edge = Providers.classify(deps.getJSONObject(d).getString("path"))
                        edge.put("from", file.relativeTo(app).path)
                        graph.put(edge)
                    }
                    incompatible = incompatible || deps.length() > 0 || slice.getJSONArray("imports").length() > 0 || slice.getJSONArray("metadata").length() > 0 || slice.has("chainedFixups")
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
            val reason = when {
                encrypted -> "Protected/encrypted Mach-O. Conversion prohibited; no DRM or FairPlay bypass."
                !hasCandidate -> "No supported ARM64/ARMv6/ARMv7 slice. ARM64e PAC reconstruction is blocked."
                incompleteBindings -> "Incomplete dyld binding table: the binding stream is malformed, so dependencies cannot be resolved safely on this device."
                incompatible -> "Dependencies, imports, metadata or embedded code need the compatibility runtime; conversion continues on device and reports real provider coverage."
                else -> "Analysis completed. The entry leaf can be proved and converted to Android ARM64 on this device."
            }
            report.put("blockers", JSONArray().put(reason)).put("hostCommand", "python3 -m radek convert input.ipa --authorized --output workspace/result")
            log(if (encrypted || incompatible || !hasCandidate) ConversionState.BLOCKED else ConversionState.PARTIAL, reason)
        } catch (e: Exception) {
            report.put("error", "${e.javaClass.simpleName}: ${e.message}")
            log(ConversionState.FAILED, e.message ?: "Import failed")
        } finally {
            File(dir, "source.ipa").delete(); File(dir, "extracted").deleteRecursively()
        }
        return dir to report
    }
}
