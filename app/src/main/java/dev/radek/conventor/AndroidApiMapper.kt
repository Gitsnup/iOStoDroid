package dev.radek.conventor

import org.json.JSONArray
import org.json.JSONObject

/**
 * Conservative symbol-name inventory for APIs whose public C symbol has a
 * Bionic counterpart. A match is only a relinking candidate; this report does
 * not claim that a Mach-O instruction, Darwin ABI, or API semantics were
 * converted. Objective-C, Swift and Apple-framework symbols remain unsupported.
 */
internal object AndroidApiMapper {
    private const val MAX_SYMBOLS = 10_000
    private val bionicLibraries = mapOf(
        "libc.so" to setOf(
            "abort", "abs", "atoi", "atof", "calloc", "clock_gettime", "close", "exit", "fclose", "feof",
            "ferror", "fflush", "fgetc", "fgets", "fopen", "fprintf", "fputc", "fputs", "fread", "free",
            "fseek", "ftell", "fwrite", "getenv", "gettimeofday", "malloc", "memcmp", "memcpy", "memmove",
            "memset", "mkdir", "open", "perror", "printf", "puts", "read", "realloc", "remove", "rename",
            "rmdir", "scanf", "snprintf", "sprintf", "strcmp", "strcpy", "strdup", "strerror", "strlen",
            "strncat", "strncmp", "strncpy", "strnlen", "strrchr", "strchr", "strstr", "strtol", "strtoll",
            "strtoul", "strtoull", "tolower", "toupper", "unlink", "vsnprintf", "write", "__stack_chk_fail",
        ),
        "libm.so" to setOf("acos", "asin", "atan", "atan2", "ceil", "cos", "exp", "fabs", "floor", "log", "pow", "sin", "sqrt", "tan"),
    )

    fun analyze(nodes: JSONArray): JSONObject {
        val symbols = linkedSetOf<String>()
        var truncated = false
        outer@ for (nodeIndex in 0 until nodes.length()) {
            val analysis = nodes.optJSONObject(nodeIndex)?.optJSONObject("analysis") ?: continue
            val slices = analysis.optJSONArray("slices") ?: continue
            for (sliceIndex in 0 until slices.length()) {
                val imports = slices.optJSONObject(sliceIndex)?.optJSONArray("imports") ?: continue
                for (importIndex in 0 until imports.length()) {
                    val item = imports.opt(importIndex)
                    val name = when (item) {
                        is JSONObject -> item.optString("name", "")
                        is String -> item
                        else -> ""
                    }.takeIf { it.isNotBlank() } ?: continue
                    if (name !in symbols && symbols.size >= MAX_SYMBOLS) {
                        truncated = true
                        break@outer
                    }
                    symbols += name
                }
            }
        }

        val result = JSONArray()
        var candidates = 0
        symbols.sorted().forEach { source ->
            // Mach-O C symbols conventionally carry one leading underscore. Remove
            // only that decoration; Objective-C selector/mangled prefixes are not
            // normalized into guessed Android functions.
            val candidate = source.removePrefix("_")
            val library = bionicLibraries.entries.firstOrNull { candidate in it.value }?.key
            val isCandidate = library != null
            if (isCandidate) candidates++
            result.put(JSONObject()
                .put("sourceSymbol", source)
                .put("classification", if (isCandidate) "BIONIC_SYMBOL_CANDIDATE" else "UNMAPPED")
                .put("targetLibrary", library ?: JSONObject.NULL)
                .put("targetSymbol", if (isCandidate) candidate else JSONObject.NULL)
                .put("linkedOrRewritten", false)
                .put("reason", if (isCandidate) {
                    "A same-named Bionic symbol exists in $library; Darwin ABI/data-layout/semantics were not verified and no binary relinking was performed."
                } else {
                    classifyUnsupported(source)
                }))
        }
        val percent = if (symbols.isEmpty()) 0 else candidates * 100 / symbols.size
        return JSONObject()
            .put("schemaVersion", 1)
            .put("measure", "Percentage of distinct imported symbol names with a conservative Bionic libc/libm name candidate; not a percentage of a working Android port.")
            .put("distinctImportSymbols", symbols.size)
            .put("mappedNameCandidates", candidates)
            .put("candidateCoveragePercent", percent)
            .put("truncated", truncated)
            .put("symbols", result)
    }

    private fun classifyUnsupported(symbol: String): String = when {
        symbol.contains("objc", ignoreCase = true) -> "Objective-C runtime ABI and message dispatch are not implemented."
        symbol.startsWith("_swift", ignoreCase = true) || symbol.contains("Swift", ignoreCase = true) -> "Swift runtime/ABI translation is not implemented."
        symbol.startsWith("_UI") || symbol.startsWith("_CG") || symbol.startsWith("_CA") || symbol.startsWith("_MTL") ->
            "Apple UI/graphics/Metal framework API has no verified Android implementation in this converter."
        symbol.startsWith("_AV") || symbol.startsWith("_Audio") || symbol.startsWith("_AL") ->
            "Apple audio/video API has no verified Android implementation in this converter."
        else -> "No reviewed Android API mapping exists for this imported symbol."
    }
}
