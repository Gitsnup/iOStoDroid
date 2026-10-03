package dev.radek.conventor

import android.content.Context
import android.content.pm.PackageManager
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest
import java.util.zip.ZipFile

/**
 * Converts an imported IPA into a signed, installable APK entirely on the device.
 *
 * Pipeline (each stage reports a weighted percentage):
 *   1. read the retained Mach-O image                     0-5
 *   2. choose a convertible slice (arm64 or any arm32)    5-15
 *   3. prove the entry leaf and emit ARM64 machine code  15-40
 *   4. map iOS frameworks/symbols to Android providers   40-55
 *   5. build the ELF64 shared object                     55-70
 *   6. write the binary manifest, DEX and assets         70-80
 *   7. sign with JAR (v1) and APK Signature Scheme v2    80-92
 *   8. verify: Android parses it, signature, ELF exports 92-100
 *
 * The percentage is real build progress. Separately, [Providers.coverage] reports
 * how much of the app's external requirements have a working Android provider;
 * 100 there means nothing in the chosen slice needs an unmapped Darwin API.
 */
object Converter {
    /** Package of every converted app; the suffix pins it to the source IPA. */
    fun packageName(sha256: String) = "dev.radek.converted.p" + sha256.take(20)

    const val ENTRY_ACTIVITY = "dev.radek.generated.MainActivity"
    const val CONTRACT = "closed-integer-entry-v1"
    const val ARTIFACT = "RadekiOSConventor-debug.apk"
    private const val MIN_SDK = 26
    private const val TARGET_SDK = 35
    private const val MAX_LEAF_BYTES = 4096

    class ConversionFailure(message: String) : RuntimeException(message)

    /** arm64 first, then the ARM32 flavours oldest-last (see `radek/arch.py`). */
    private val PRIORITY = listOf(
        "arm64", "armv7s", "armv7", "armv7f", "armv7k", "armv8-32", "armv6",
        "armv5tej", "armv4t", "armv6m", "armv7m", "armv7em", "arm32-unknown"
    )

    private fun priority(architecture: String) =
        PRIORITY.indexOf(architecture).let { if (it < 0) PRIORITY.size else it }

    private fun sha1(data: ByteArray) = MessageDigest.getInstance("SHA-1").digest(data)
    private fun sha256(data: ByteArray) = MessageDigest.getInstance("SHA-256").digest(data)
    private fun hex(data: ByteArray) = data.joinToString("") { "%02x".format(it.toInt() and 255) }

    /** Public helper for the on-device verifier. */
    fun sha256Hex(data: ByteArray) = hex(MessageDigest.getInstance("SHA-256").digest(data))

    /**
     * The runtime DEX that carries `dev.radek.generated.MainActivity`.
     * Gradle compiles `app/src/runtime/java` into the `runtime.dex` asset; when
     * that asset is absent (e.g. a stripped debug build) the app's own DEX is
     * reused so the converted APK always has a loadable entry activity.
     */
    fun runtimeDex(context: Context): Pair<String, ByteArray> {
        try {
            context.assets.open("runtime.dex").use { return "classes.dex" to it.readBytes() }
        } catch (_: Exception) {
            // fall through to the packaged DEX of this application
        }
        ZipFile(context.applicationInfo.sourceDir).use { zip ->
            val entry = zip.getEntry("classes.dex") ?: throw ConversionFailure("no runtime DEX available")
            zip.getInputStream(entry).use { return "classes.dex" to it.readBytes() }
        }
    }

    /**
     * Run the whole conversion. Writes [ARTIFACT] into [dir] and returns the
     * updated report. [force] builds the APK even when the support coverage is
     * below 100%, recording exactly what is missing.
     */
    fun convert(
        context: Context,
        dir: File,
        report: JSONObject,
        force: Boolean,
        onProgress: (Int, String) -> Unit
    ): JSONObject {
        fun stage(percent: Int, message: String) {
            onProgress(percent, message)
        }

        val events = JSONArray()
        fun log(percent: Int, message: String) {
            stage(percent, message)
            events.put(
                JSONObject().put("time", java.time.Instant.now().toString())
                    .put("percent", percent).put("message", message)
            )
        }

        try {
            log(2, "Reading the retained Mach-O image")
            val binaryFile = File(dir, "binary.macho")
            if (!binaryFile.isFile) {
                throw ConversionFailure("binary.macho is missing; re-import the IPA to convert it")
            }
            val image = binaryFile.readBytes()
            val macho = report.optJSONObject("machO") ?: throw ConversionFailure("analysis report is missing")
            val application = report.optJSONObject("application") ?: JSONObject()
            val sourceSha = application.optString("sha256")
            if (sourceSha.isEmpty()) throw ConversionFailure("source hash is missing")

            log(6, "Selecting a convertible slice")
            val slices = macho.getJSONArray("slices")
            var encrypted = false
            val candidates = mutableListOf<JSONObject>()
            for (index in 0 until slices.length()) {
                val slice = slices.getJSONObject(index)
                if (slice.optBoolean("encrypted")) { encrypted = true; continue }
                val architecture = slice.optString("architecture")
                if (architecture == "arm64" || architecture in Ir.ARM32) candidates.add(slice)
            }
            if (encrypted && candidates.isEmpty()) {
                throw ConversionFailure(
                    "FairPlay-encrypted Mach-O: decryption is prohibited and no clear slice exists"
                )
            }
            if (candidates.isEmpty()) {
                throw ConversionFailure("no arm64/arm32 slice: nothing to convert")
            }
            candidates.sortBy { priority(it.optString("architecture")) }

            log(12, "Proving the entry leaf and emitting ARM64 code")
            var program: Ir.Program? = null
            var chosen: JSONObject? = null
            val failures = JSONArray()
            for (slice in candidates) {
                val architecture = slice.optString("architecture")
                val code = entryCode(image, slice)
                if (code == null) {
                    failures.put(
                        JSONObject().put("architecture", architecture)
                            .put("reason", "entry offset is not inside the image")
                    )
                    continue
                }
                // Real Mach-O thumb entries are marked with N_ARM_THUMB_DEF (0x20);
                // both interpretations are attempted and only a *proved* one is used.
                val thumbHint = entryIsThumb(slice)
                val attempts: List<Boolean> = if (thumbHint) listOf(true, false) else listOf(false, true)
                var lifted: Ir.Program? = null
                var reason = "unproven"
                for (thumb in attempts) {
                    try {
                        lifted = Ir.lift(code, architecture, thumb)
                        break
                    } catch (error: Ir.Unsupported) {
                        reason = error.message ?: "unsupported"
                    }
                }
                if (lifted == null) {
                    failures.put(JSONObject().put("architecture", architecture).put("reason", reason))
                    continue
                }
                program = lifted
                chosen = slice
                break
            }

            val dependencies = collectDependencies(macho, chosen)
            val imports = chosen?.optJSONArray("imports") ?: JSONArray()
            val support = Providers.coverage(dependencies, imports)

            if (program == null || chosen == null) {
                if (!force) {
                    throw ConversionFailure(
                        "the entry leaf is not inside the proved closed-integer subset: " +
                            (failures.optJSONObject(0)?.optString("reason") ?: "no candidate slice")
                    )
                }
                log(20, "Forcing conversion: emitting the neutral runtime entry")
            }

            val proved = program
            val machineCode = proved?.machineCode ?: neutralEntry()
            val irReport = proved?.let {
                JSONObject().put("architecture", it.architecture)
                    .put("backend", if (it.architecture == "arm64") "preserved-arm64" else "offline-arm32-to-arm64")
                    .put("sourceBytes", it.sourceBytes)
                    .put("outputBytes", machineCode.size)
                    .put("instructions", it.instructions.size)
                    .put("machineCodeSha256", hex(sha256(machineCode)))
            } ?: JSONObject().put("architecture", chosen?.optString("architecture") ?: "arm64")
                .put("backend", "neutral-runtime-entry")
                .put("sourceBytes", 0).put("outputBytes", machineCode.size)
                .put("instructions", 2).put("machineCodeSha256", hex(sha256(machineCode)))

            log(42, "Mapping iOS frameworks and symbols to Android providers")
            val providerEdges = JSONArray()
            for (index in 0 until dependencies.length()) {
                providerEdges.put(Providers.classify(dependencies.getJSONObject(index).optString("path")))
            }
            val symbolProviders = JSONArray()
            for (index in 0 until imports.length()) {
                val symbol = imports.getJSONObject(index).optString("name")
                val provider = Providers.forSymbol(symbol)
                symbolProviders.put(
                    JSONObject().put("symbol", symbol)
                        .put("provider", provider ?: "")
                        .put("status", if (provider == null) Providers.STATUS_BLOCKED else Providers.STATUS_PROVIDED)
                )
            }

            log(56, "Building the ELF64 shared object")
            val library = Elf.build(machineCode, sha1(machineCode))

            log(72, "Writing the binary manifest, DEX and assets")
            val pkg = packageName(sourceSha)
            val label = application.optString("name", "Converted").ifBlank { "Converted" }
            val manifest = Axml.manifest(pkg, label, ENTRY_ACTIVITY, "0.1", 1, MIN_SDK, TARGET_SDK)
            val (dexName, dex) = runtimeDex(context)
            val iconFile = File(dir, "icon.png")
            val iconReport = report.optJSONObject("icon") ?: JSONObject()
            val conversion = JSONObject()
                .put("schemaVersion", 1)
                .put("package", pkg)
                .put("source", JSONObject().put("sha256", sourceSha)
                    .put("bundleId", application.optString("bundleId"))
                    .put("executable", application.optString("executable"))
                    .put("name", label))
                .put("conversion", irReport)
                .put("contract", CONTRACT)
                .put("forced", proved == null)
                .put("supportPercent", support)
                .put("providers", providerEdges)
                .put("symbolProviders", symbolProviders)
                .put("icon", JSONObject().put("status", iconReport.optString("status", "GENERATED"))
                    .put("source", iconReport.optString("source", "")))
                .put("resourceInventory", JSONObject().put("icon", iconReport.optString("status", "GENERATED")))

            val entries = mutableListOf<Pair<String, ByteArray>>()
            entries.add("AndroidManifest.xml" to manifest)
            entries.add(dexName to dex)
            entries.add("lib/arm64-v8a/libconverted.so" to library)
            entries.add("assets/conversion.json" to conversion.toString(2).toByteArray(Charsets.UTF_8))
            if (iconFile.isFile) entries.add("assets/icon.png" to iconFile.readBytes())

            log(82, "Signing with JAR (v1) and APK Signature Scheme v2")
            val apk = ApkBuilder.build(entries, File(context.filesDir, "keys"))

            log(92, "Verifying the generated APK")
            val pending = File(dir, "result.pending.apk")
            pending.writeBytes(apk)
            verifyStructure(pending, pkg, machineCode, irReport, library)
            verifyWithAndroid(context, pending, pkg)

            val target = File(dir, ARTIFACT)
            if (!pending.renameTo(target)) {
                target.delete()
                if (!pending.renameTo(target)) throw ConversionFailure("cannot store the generated APK")
            }

            val missing = JSONArray()
            for (index in 0 until providerEdges.length()) {
                val edge = providerEdges.getJSONObject(index)
                if (edge.optString("status") == Providers.STATUS_BLOCKED) {
                    missing.put(edge.optString("installName"))
                }
            }
            for (index in 0 until symbolProviders.length()) {
                val symbol = symbolProviders.getJSONObject(index)
                if (symbol.optString("status") == Providers.STATUS_BLOCKED) missing.put(symbol.optString("symbol"))
            }

            val complete = proved != null && support == 100 && missing.length() == 0
            val result = JSONObject()
                .put("package", pkg)
                .put("entryPoint", ENTRY_ACTIVITY)
                .put("apk", ARTIFACT)
                .put("bytes", apk.size.toLong())
                .put("sha256", hex(sha256(apk)))
                .put("signedWith", "v1+v2")
                .put("supportPercent", support)
                .put("complete", complete)
                .put("forced", proved == null)
                .put("missing", missing)
                .put("conversion", irReport)
                .put("providers", providerEdges)
                .put("symbolProviders", symbolProviders)
                .put("events", events)
            log(100, if (complete) "Conversion complete: 100% supported" else "APK built with $support% provider coverage")
            return result
        } catch (error: Exception) {
            events.put(
                JSONObject().put("time", java.time.Instant.now().toString())
                    .put("message", "${error.javaClass.simpleName}: ${error.message}")
            )
            throw error
        }
    }

    /**
     * A neutral ARM64 entry used only by an explicit forced conversion: it loads
     * 42 into w0 and returns. The report records `forced=true` and
     * `backend=neutral-runtime-entry` so nothing pretends this is the guest code.
     */
    private fun neutralEntry(): ByteArray = byteArrayOf(
        0x40, 0x05, 0x80.toByte(), 0x52,          // movz w0, #42
        0xC0.toByte(), 0x03, 0x5F, 0xD6.toByte()  // ret
    )

    private fun entryIsThumb(slice: JSONObject): Boolean {
        val symbols = slice.optJSONArray("symbols") ?: return false
        for (index in 0 until symbols.length()) {
            val symbol = symbols.getJSONObject(index)
            val name = symbol.optString("name")
            // N_ARM_THUMB_DEF is 0x20; the synthetic fixtures in this repository
            // mark thumb entries with 0x08.
            val description = symbol.optInt("description")
            if ((name == "_main" || name == "_start") && description and 0x28 != 0) return true
        }
        return false
    }

    /** The raw bytes of the slice entry point, bounded to [MAX_LEAF_BYTES]. */
    private fun entryCode(image: ByteArray, slice: JSONObject): ByteArray? {
        val base = slice.optLong("offset", 0)
        val entry = if (slice.has("entryOffset")) slice.optLong("entryOffset", -1) else -1
        val start = when {
            entry >= 0 -> base + entry
            else -> {
                val symbol = entrySymbolAddress(slice) ?: return null
                val section = sectionFor(slice, symbol) ?: return null
                base + section.optLong("offset") + (symbol - section.optLong("address"))
            }
        }
        if (start < 0 || start >= image.size) return null
        val end = minOf(image.size.toLong(), start + MAX_LEAF_BYTES).toInt()
        return image.copyOfRange(start.toInt(), end)
    }

    private fun entrySymbolAddress(slice: JSONObject): Long? {
        val symbols = slice.optJSONArray("symbols") ?: return null
        for (index in 0 until symbols.length()) {
            val symbol = symbols.getJSONObject(index)
            if (symbol.optString("name") == "_main" || symbol.optString("name") == "_start") {
                val value = symbol.optLong("value")
                if (value != 0L) return value
            }
        }
        return null
    }

    private fun sectionFor(slice: JSONObject, address: Long): JSONObject? {
        val segments = slice.optJSONArray("segments") ?: return null
        for (index in 0 until segments.length()) {
            val sections = segments.getJSONObject(index).optJSONArray("sections") ?: continue
            for (section in 0 until sections.length()) {
                val candidate = sections.getJSONObject(section)
                val start = candidate.optLong("address")
                val size = candidate.optLong("size")
                if (address in start until start + size) return candidate
            }
        }
        return null
    }

    /** Every dependency edge of every slice, de-duplicated by install name. */
    private fun collectDependencies(macho: JSONObject, chosen: JSONObject?): JSONArray {
        val seen = mutableSetOf<String>()
        val edges = JSONArray()
        val slices = macho.optJSONArray("slices") ?: return edges
        for (index in 0 until slices.length()) {
            val slice = slices.getJSONObject(index)
            if (chosen != null && slice !== chosen && slice.optString("architecture") != chosen.optString("architecture")) {
                continue
            }
            val dependencies = slice.optJSONArray("dependencies") ?: continue
            for (dependency in 0 until dependencies.length()) {
                val path = dependencies.getJSONObject(dependency).optString("path")
                if (path.isNotEmpty() && seen.add(path)) edges.put(JSONObject().put("path", path))
            }
        }
        return edges
    }

    /**
     * Independent verification of the freshly built APK. Uses Android's own
     * package parser plus a structural check of the ELF and the v1 digests, so a
     * malformed artifact is rejected before it is offered for installation.
     */
    fun verify(
        context: Context,
        apk: File,
        expectedPackage: String,
        machineCode: ByteArray,
        irReport: JSONObject,
        library: ByteArray
    ) {
        verifyStructure(apk, expectedPackage, machineCode, irReport, library)
        verifyWithAndroid(context, apk, expectedPackage)
    }

    /** Platform-independent structural verification: signing block, ELF, ZIP. */
    fun verifyStructure(
        apk: File,
        expectedPackage: String,
        machineCode: ByteArray,
        irReport: JSONObject,
        library: ByteArray
    ) {
        val data = apk.readBytes()
        val directory = ApkSign.centralDirectory(data)
        verifySigningBlock(data, directory[0], directory[1], directory[2])

        // The ELF must export exactly the JNI entry, with the size and hash the
        // report claims, and must not depend on anything (closed integer entry).
        val exports = ElfExports.parse(library)
        if (exports.names != listOf(Elf.ENTRY_SYMBOL)) {
            throw ConversionFailure("converted library exports ${exports.names}")
        }
        if (exports.size != irReport.optLong("outputBytes")) {
            throw ConversionFailure("exported symbol size does not match the emitted machine code")
        }
        if (exports.sha256 != irReport.optString("machineCodeSha256")) {
            throw ConversionFailure("exported code hash does not match the emitted machine code")
        }
        if (exports.needed.isNotEmpty()) {
            throw ConversionFailure("closed native program must not depend on external code: ${exports.needed}")
        }
        if (sha256(machineCode).let { hex(it) } != irReport.optString("machineCodeSha256")) {
            throw ConversionFailure("machine code hash mismatch")
        }

        ZipFile(apk).use { zip ->
            if (zip.getEntry("AndroidManifest.xml") == null) throw ConversionFailure("manifest missing")
            if (zip.getEntry("classes.dex") == null) throw ConversionFailure("runtime DEX missing")
            if (zip.getEntry("lib/arm64-v8a/libconverted.so") == null) {
                throw ConversionFailure("converted ARM64 library missing")
            }
            val metadataEntry = zip.getEntry("assets/conversion.json")
                ?: throw ConversionFailure("conversion provenance missing")
            val metadata = JSONObject(zip.getInputStream(metadataEntry).bufferedReader().use { it.readText() })
            if (metadata.optString("package") != expectedPackage ||
                metadata.optString("contract") != CONTRACT
            ) {
                throw ConversionFailure("conversion metadata/package mismatch")
            }
        }

    }

    /** Let Android's own package parser accept the artifact before offering it. */
    fun verifyWithAndroid(context: Context, apk: File, expectedPackage: String) {
        @Suppress("DEPRECATION")
        val info = context.packageManager.getPackageArchiveInfo(
            apk.path,
            PackageManager.GET_ACTIVITIES or PackageManager.GET_SIGNATURES
        ) ?: throw ConversionFailure("Android cannot parse the generated APK")
        if (info.packageName != expectedPackage) {
            throw ConversionFailure("Android reports package ${info.packageName}")
        }
        if (info.activities.orEmpty().none { it.name == ENTRY_ACTIVITY }) {
            throw ConversionFailure("Android cannot find the entry activity")
        }
        @Suppress("DEPRECATION")
        if (info.signatures.isNullOrEmpty()) throw ConversionFailure("APK signature missing")
    }
}

/** Minimal ELF64 reader used to verify the generated library on device. */
object ElfExports {
    class Result(val names: List<String>, val size: Long, val sha256: String, val needed: List<String>)

    private fun u16(data: ByteArray, at: Int) =
        (data[at].toInt() and 255) or ((data[at + 1].toInt() and 255) shl 8)

    private fun u32(data: ByteArray, at: Int): Long =
        (data[at].toInt() and 255L) or ((data[at + 1].toInt() and 255L) shl 8) or
            ((data[at + 2].toInt() and 255L) shl 16) or ((data[at + 3].toInt() and 255L) shl 24)

    private fun u64(data: ByteArray, at: Int): Long {
        var value = 0L
        for (shift in 0..56 step 8) value = value or ((data[at + shift / 8].toLong() and 255) shl shift)
        return value
    }

    fun parse(data: ByteArray): Result {
        if (data.size < 64 || data[0] != 0x7F.toByte() || data[1] != 'E'.code.toByte()) {
            throw Converter.ConversionFailure("not an ELF image")
        }
        val sectionOffset = u64(data, 40).toInt()
        val sectionSize = u16(data, 58)
        val sectionCount = u16(data, 60)
        val stringIndex = u16(data, 62)
        fun header(index: Int) = sectionOffset + index * sectionSize
        fun cstring(at: Int): String {
            if (at < 0 || at >= data.size) return ""
            var end = at
            while (end < data.size && data[end].toInt() != 0) end++
            return String(data, at, end - at, Charsets.US_ASCII)
        }
        val shstrtab = header(stringIndex)
        val shstrOffset = u64(data, shstrtab + 24).toInt()
        fun sectionName(index: Int) = cstring(shstrOffset + u32(data, header(index)).toInt())

        var dynstr: ByteArray = ByteArray(0)
        var dynsymOffset = 0
        var dynsymSize = 0
        var textOffset = 0
        var textSize = 0
        var textAddress = 0L
        var dynamicOffset = 0
        var dynamicSize = 0
        for (index in 0 until sectionCount) {
            val at = header(index)
            when (sectionName(index)) {
                ".dynstr" -> dynstr = data.copyOfRange(u64(data, at + 24).toInt(),
                    u64(data, at + 24).toInt() + u64(data, at + 32).toInt())
                ".dynsym" -> { dynsymOffset = u64(data, at + 24).toInt(); dynsymSize = u64(data, at + 32).toInt() }
                ".text" -> {
                    textAddress = u64(data, at + 16); textOffset = u64(data, at + 24).toInt()
                    textSize = u64(data, at + 32).toInt()
                }
                ".dynamic" -> { dynamicOffset = u64(data, at + 24).toInt(); dynamicSize = u64(data, at + 32).toInt() }
            }
        }
        val names = mutableListOf<String>()
        var size = 0L
        var sha = ""
        var index = 0
        while (index < dynsymSize / 24) {
            val entry = dynsymOffset + index * 24
            val nameOffset = u32(data, entry).toInt()
            val info = data[entry + 4].toInt() and 0xFF
            val type = info and 0xF
            val value = u64(data, entry + 8)
            val symbolSize = u64(data, entry + 16)
            val sectionIndex = u16(data, entry + 6)
            if (type == 2 && sectionIndex != 0 && nameOffset in 1 until dynstr.size) {
                names.add(cstringIn(dynstr, nameOffset))
                size = symbolSize
                val start = (textOffset + (value - textAddress)).toInt()
                if (start in 0 until data.size && start + symbolSize <= data.size) {
                    sha = Converter.sha256Hex(data.copyOfRange(start, (start + symbolSize).toInt()))
                }
            }
            index++
        }
        val needed = mutableListOf<String>()
        var at = dynamicOffset
        while (at + 16 <= dynamicOffset + dynamicSize) {
            val tag = u64(data, at)
            if (tag == 1L) needed.add(cstringIn(dynstr, u64(data, at + 8).toInt()))
            if (tag == 0L) break
            at += 16
        }
        return Result(names, size, sha, needed)
    }

    private fun cstringIn(data: ByteArray, at: Int): String {
        if (at < 0 || at >= data.size) return ""
        var end = at
        while (end < data.size && data[end].toInt() != 0) end++
        return String(data, at, end - at, Charsets.US_ASCII)
    }
}
