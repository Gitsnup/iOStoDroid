package dev.iostodroid.conventor

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.Typeface
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/**
 * Icon recovery for imported IPAs.
 *
 * Modern iOS games ship their artwork only inside `Assets.car` (a compiled BOM
 * asset catalog) and as Apple `CgBI` PNGs, which `BitmapFactory` cannot decode.
 * The previous code therefore produced a blank or transparent tile for most
 * library entries. This pipeline adds:
 *
 *  1. bounded magic-sniffing extraction of PNG/JPEG payloads out of `Assets.car`,
 *  2. `CgBI` repair (raw-deflate IDAT re-wrapped as zlib, R/B channels swapped),
 *  3. rejection of fully transparent bitmaps, which are never usable icons,
 *  4. a deterministic generated icon so no library entry is ever blank.
 */
object Icons {
    private const val TARGET = 512
    private const val MAX_IMAGE_BYTES = 16L * 1024 * 1024
    private const val MAX_CATALOG_BYTES = 96L * 1024 * 1024
    private const val MAX_PAYLOADS = 64
    private val PNG_SIGNATURE = byteArrayOf(0x89.toByte(), 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A)

    /** Icon suffixes, highest scale first: the best available representation wins. */
    val SUFFIXES = listOf(
        "@3x.png", "@2x.png", ".png", "@3x~ipad.png", "@2x~ipad.png", "~ipad.png",
        "@3x~iphone.png", "@2x~iphone.png", "~iphone.png", "", "@3x.jpg", "@2x.jpg", ".jpg",
        ".png.png", "@2x.png.png"
    )

    // ------------------------------------------------------------------ decode
    /**
     * Decode image bytes.
     *
     * Ordinary PNG/JPEG goes through `BitmapFactory`. Apple `CgBI` PNGs - which
     * every `pngcrush`-processed iOS bundle ships - cannot be decoded by
     * `BitmapFactory` at all, so they are normalised by [IconDecoder] (raw/zlib
     * DEFLATE, BGRA channel order and premultiplied alpha).
     */
    fun decode(data: ByteArray, sample: Int = 1): Bitmap? {
        if (data.isEmpty() || data.size > MAX_IMAGE_BYTES) return null
        if (isCgbi(data)) {
            return try {
                IconDecoder.decodeCgbi(data, TARGET)
            } catch (_: Exception) {
                null
            }
        }
        val options = BitmapFactory.Options()
        options.inSampleSize = maxOf(1, sample)
        return try {
            BitmapFactory.decodeByteArray(data, 0, data.size, options)
        } catch (_: Exception) {
            // Robolectric's BitmapFactory throws where the platform returns null;
            // either way this candidate simply did not decode.
            null
        }
    }

    /** True when the payload is an Apple CgBI PNG. */
    fun isCgbi(data: ByteArray): Boolean = try {
        IconDecoder.isCgbiBytes(data)
    } catch (_: Exception) {
        false
    }

    /**
     * True when a bitmap is usable as a launcher icon. A fully transparent image
     * (the usual result of decoding an alpha mask or a template asset) is not.
     */
    fun isOpaque(bitmap: Bitmap): Boolean {
        val width = bitmap.width
        val height = bitmap.height
        if (width <= 0 || height <= 0) return false
        val step = maxOf(1, (width * height) / 4096)
        val pixels = IntArray(width * height)
        bitmap.getPixels(pixels, 0, width, 0, 0, width, height)
        var visible = 0
        var sampled = 0
        var index = 0
        while (index < pixels.size) {
            sampled++
            if ((pixels[index] ushr 24) != 0) visible++
            index += step
        }
        return sampled > 0 && visible * 100 / sampled >= 5
    }

    // ---------------------------------------------------------- Assets.car scan
    /**
     * Extract embedded image payloads from a compiled asset catalog. The catalog
     * structure is only partially understood, so payloads are located by sniffing
     * for PNG/JPEG magic and validated by walking their own container structure.
     * Nothing is executed; the result is a list of candidate image files.
     */
    fun catalogPayloads(file: File): List<ByteArray> {
        if (!file.isFile || file.length() > MAX_CATALOG_BYTES) return emptyList()
        val data = file.readBytes()
        if (data.size < 8 || String(data, 0, 8, Charsets.US_ASCII) != "BOMStore") return emptyList()
        val found = mutableListOf<ByteArray>()
        var at = 0
        while (at < data.size - 8 && found.size < MAX_PAYLOADS) {
            if (startsWith(data, PNG_SIGNATURE, at)) {
                val end = pngEnd(data, at)
                if (end > at) {
                    found.add(data.copyOfRange(at, end))
                    at = end
                    continue
                }
            }
            if (data[at] == 0xFF.toByte() && data[at + 1] == 0xD8.toByte() && data[at + 2] == 0xFF.toByte()) {
                val end = jpegEnd(data, at)
                if (end > at) {
                    found.add(data.copyOfRange(at, end))
                    at = end
                    continue
                }
            }
            at++
        }
        return found
    }

    private fun pngEnd(data: ByteArray, start: Int): Int {
        var at = start + 8
        while (at + 12 <= data.size) {
            val length = readU32(data, at)
            if (length < 0 || at + 12 + length > data.size) return -1
            val type = String(data, at + 4, 4, Charsets.US_ASCII)
            at += 12 + length
            if (type == "IEND") return at
        }
        return -1
    }

    private fun jpegEnd(data: ByteArray, start: Int): Int {
        var at = start + 2
        while (at + 1 < data.size) {
            if (data[at] == 0xFF.toByte() && data[at + 1] == 0xD9.toByte()) return at + 2
            at++
        }
        return -1
    }

    // ------------------------------------------------------------------- build
    /**
     * A deterministic, always-visible icon for entries whose artwork could not be
     * recovered: rounded gradient tile derived from the app name plus its initials.
     */
    fun generate(label: String, size: Int = TARGET): Bitmap {
        val bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bitmap)
        val seed = label.hashCode() and 0x7FFFFFFF
        val hue = (seed % 360).toFloat()
        val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        paint.shader = LinearGradient(
            0f, 0f, size.toFloat(), size.toFloat(),
            Color.HSVToColor(floatArrayOf(hue, 0.62f, 0.96f)),
            Color.HSVToColor(floatArrayOf((hue + 42f) % 360f, 0.72f, 0.55f)),
            Shader.TileMode.CLAMP
        )
        val radius = size * 0.22f
        canvas.drawRoundRect(RectF(0f, 0f, size.toFloat(), size.toFloat()), radius, radius, paint)
        val initials = label.split(Regex("[^\\p{L}\\p{N}]+"))
            .filter { it.isNotEmpty() }.take(2).joinToString("") { it.substring(0, 1) }
            .uppercase().ifEmpty { "?" }
        val text = Paint(Paint.ANTI_ALIAS_FLAG)
        text.color = Color.WHITE
        text.textAlign = Paint.Align.CENTER
        text.typeface = Typeface.DEFAULT_BOLD
        text.textSize = size * (if (initials.length > 1) 0.38f else 0.46f)
        val metrics = Paint.FontMetrics()
        text.getFontMetrics(metrics)
        canvas.drawText(initials, size / 2f, size / 2f - (metrics.ascent + metrics.descent) / 2f, text)
        return bitmap
    }

    // ---------------------------------------------------------------- pipeline
    private fun readU32(data: ByteArray, at: Int): Int =
        ((data[at].toInt() and 255) shl 24) or ((data[at + 1].toInt() and 255) shl 16) or
            ((data[at + 2].toInt() and 255) shl 8) or (data[at + 3].toInt() and 255)

    private fun startsWith(data: ByteArray, prefix: ByteArray, at: Int = 0): Boolean {
        if (at + prefix.size > data.size) return false
        for (index in prefix.indices) if (data[at + index] != prefix[index]) return false
        return true
    }

    /** Read original dimensions before BitmapFactory downsamples the candidate. */
    private fun imageDimensions(data: ByteArray): Pair<Int, Int>? {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        try {
            BitmapFactory.decodeByteArray(data, 0, data.size, bounds)
        } catch (_: Exception) {
            // CgBI is not a standard Android PNG; read its IHDR below.
        }
        if (bounds.outWidth in 1..8192 && bounds.outHeight in 1..8192) {
            return bounds.outWidth to bounds.outHeight
        }
        if (!startsWith(data, PNG_SIGNATURE) || data.size < 33) return null
        var offset = PNG_SIGNATURE.size
        val limit = minOf(data.size, 4096)
        while (offset + 12 <= limit) {
            val length = readU32(data, offset)
            if (length < 0 || length.toLong() + 12 > data.size - offset) return null
            val type = String(data, offset + 4, 4, Charsets.US_ASCII)
            if (type == "IHDR" && length == 13 && offset + 20 <= data.size) {
                val width = readU32(data, offset + 8)
                val height = readU32(data, offset + 12)
                return if (width in 1..8192 && height in 1..8192) width to height else null
            }
            if (type == "IEND") return null
            offset += 12 + length
        }
        return null
    }

    private data class Candidate(
        val source: String,
        val bitmap: Bitmap,
        val declared: Boolean,
        val scale: Double,
        val named: Boolean,
        val sourceWidth: Int,
        val sourceHeight: Int,
        val kind: String,
        val format: String,
        val decoder: String,
    )

    private fun iconNameMatches(preferred: String, source: String): Boolean {
        fun stem(value: String): String = value.substringAfterLast(':').substringAfterLast('/')
            .substringBeforeLast('.', value).lowercase()
        val wanted = stem(preferred)
        val actual = stem(source)
        if (actual == wanted) return true
        if (!actual.startsWith(wanted)) return false
        val suffix = actual.drop(wanted.length)
        return suffix.isNotEmpty() && (suffix.first() in "@~_-" || suffix.first().isDigit())
    }

    /**
     * Recover the best icon for a bundle and record every attempt.
     * Order: declared Info.plist names -> scale/idiom variants -> `Assets.car`
     * payloads -> icon-named bundle images -> any opaque image -> generated tile.
     */
    fun recover(app: File, names: List<String>, dir: File, label: String, attempts: JSONArray): JSONObject {
        fun attempt(source: String, ok: Boolean, detail: String, width: Int = 0, height: Int = 0) {
            attempts.put(
                JSONObject().put("source", source).put("ok", ok).put("detail", detail)
                    .put("width", width).put("height", height)
            )
        }

        val declared = mutableListOf<File>()
        for (name in names) {
            val safe = try {
                SafeZip.memberName(name)
            } catch (error: IllegalArgumentException) {
                attempt(name, false, "icon name rejected: ${error.message}")
                continue
            }
            if ('/' in safe) continue
            for (suffix in SUFFIXES) {
                val file = File(app, safe + suffix)
                if (file.isFile && file !in declared) declared.add(file)
            }
        }
        val images = app.walkTopDown().filter { it.isFile && it.length() in 1..MAX_IMAGE_BYTES &&
            it.extension.lowercase() in setOf("png", "jpg", "jpeg") }.toList()
        val named = images.filter {
            val lower = it.name.lowercase()
            "icon" in lower || "artwork" in lower || "logo" in lower || "appicon" in lower
        }.sortedByDescending { it.length() }

        val queue = mutableListOf<File>()
        queue.addAll(declared)
        queue.addAll(named.filter { it !in queue })
        queue.addAll(images.filter { it !in queue })

        var best: Candidate? = null
        fun consider(
            source: String,
            data: ByteArray,
            declaredFile: Boolean,
            namedFile: Boolean,
            kind: String = "file",
            knownWidth: Int? = null,
            knownHeight: Int? = null,
            knownScale: Double? = null,
            knownFormat: String? = null,
            knownDecoder: String? = null,
        ) {
            if (data.isEmpty() || data.size > MAX_IMAGE_BYTES) {
                attempt(source, false, "payload outside the size limit"); return
            }
            val dimensions = if (knownWidth != null && knownHeight != null) {
                knownWidth to knownHeight
            } else {
                imageDimensions(data)
            }
            if (dimensions == null) {
                attempt(source, false, "image dimensions could not be read")
                return
            }
            val (sourceWidth, sourceHeight) = dimensions
            val sample = maxOf(1, maxOf(sourceWidth, sourceHeight) / TARGET)
            val bitmap = decode(data, sample)
            if (bitmap == null) {
                attempt(source, false, "image payload could not be decoded", sourceWidth, sourceHeight)
                return
            }
            if (!isOpaque(bitmap)) {
                bitmap.recycle()
                attempt(source, false, "decoded but fully transparent", sourceWidth, sourceHeight)
                return
            }
            attempt(source, true, "decoded", sourceWidth, sourceHeight)
            val scale = knownScale ?: when {
                "@3x" in source -> 3.0
                "@2x" in source -> 2.0
                else -> 1.0
            }
            val format = knownFormat ?: when {
                startsWith(data, PNG_SIGNATURE) -> "png"
                data.size >= 3 && data[0] == 0xFF.toByte() && data[1] == 0xD8.toByte() -> "jpeg"
                else -> source.substringAfterLast('.', "unknown")
            }
            val decoder = knownDecoder ?: if (isCgbi(data)) "IconDecoder" else "BitmapFactory"
            val candidate = Candidate(
                source = source,
                bitmap = bitmap,
                declared = declaredFile,
                scale = scale,
                named = namedFile,
                sourceWidth = sourceWidth,
                sourceHeight = sourceHeight,
                kind = kind,
                format = format,
                decoder = decoder,
            )
            if (better(candidate, best)) {
                best?.bitmap?.recycle()
                best = candidate
            } else {
                bitmap.recycle()
            }
        }

        var examined = 0
        for (file in queue) {
            if (examined >= 48) break
            examined++
            val relative = file.relativeTo(app).path
            consider(relative, file.readBytes(), declared.contains(file), named.contains(file))
        }
        val preferredName = names.firstOrNull()
        for (catalog in app.walkTopDown().filter { it.isFile && it.name.equals("Assets.car", ignoreCase = true) }) {
            val relative = catalog.relativeTo(app).path
            val extraction = AssetCatalogIcon.extract(catalog, preferredName, TARGET)
            extraction.attempts.forEach { item ->
                val assetLabel = item.asset.removePrefix("${catalog.name}:")
                attempt("$relative:$assetLabel", item.ok, item.detail, extraction.width, extraction.height)
            }
            val catalogBitmap = extraction.bitmap
            if (catalogBitmap != null) {
                val assetLabel = extraction.asset?.removePrefix("${catalog.name}:") ?: "unnamed"
                val assetSource = "$relative:$assetLabel"
                val matchesDeclared = preferredName?.let { iconNameMatches(it, assetSource) } ?: false
                val candidate = Candidate(
                    source = assetSource,
                    bitmap = catalogBitmap,
                    declared = matchesDeclared,
                    scale = extraction.scale,
                    named = "icon" in assetSource.lowercase() || matchesDeclared,
                    sourceWidth = extraction.width,
                    sourceHeight = extraction.height,
                    kind = "assets.car",
                    format = extraction.format ?: "asset-catalog",
                    decoder = "AssetCatalogIcon",
                )
                if (better(candidate, best)) {
                    best?.bitmap?.recycle()
                    best = candidate
                } else {
                    catalogBitmap.recycle()
                }
                continue
            }

            attempt(relative, false, extraction.error ?: "asset catalog has no decodable icon")
            // Last-resort magic scan is less precise because it loses rendition
            // names and idiom metadata; use it only when the bounded CAR parser
            // cannot produce a bitmap.
            val payloads = catalogPayloads(catalog)
            if (payloads.isNotEmpty()) {
                attempt(relative, true, "magic scan found ${payloads.size} image payload(s)")
            }
            payloads.forEachIndexed { index, payload ->
                consider("$relative#$index", payload, false, false, kind = "assets.car")
            }
        }

        val chosen = best
        if (chosen != null) {
            val bitmap = chosen.bitmap
            File(dir, "icon.png").outputStream().use {
                bitmap.compress(Bitmap.CompressFormat.PNG, 100, it)
            }
            val width = bitmap.width
            val height = bitmap.height
            bitmap.recycle()
            return JSONObject().put("status", "SUPPORTED")
                .put("source", chosen.source)
                .put("path", "icon.png")
                .put("kind", chosen.kind)
                .put("format", chosen.format)
                .put("decoder", chosen.decoder)
                .put("width", width).put("height", height)
                .put("sourceWidth", chosen.sourceWidth).put("sourceHeight", chosen.sourceHeight)
                .put("scale", chosen.scale)
                .put(
                    "reason",
                    "Selected the highest-resolution eligible bundle icon (${chosen.source}, " +
                        "${chosen.sourceWidth}x${chosen.sourceHeight}; output ${width}x${height})",
                )
                .put("attempts", attempts)
        }
        // Nothing usable: generate a deterministic tile so the library entry is
        // never transparent, and say exactly why in the report.
        val generated = generate(label)
        File(dir, "icon.png").outputStream().use { generated.compress(Bitmap.CompressFormat.PNG, 100, it) }
        generated.recycle()
        return JSONObject().put("status", "GENERATED")
            .put("path", "icon.png")
            .put("kind", "generated")
            .put("decoder", "android.graphics.Canvas")
            .put("width", TARGET).put("height", TARGET).put("scale", 1.0)
            .put("reason", "No decodable, non-transparent artwork in this bundle; generated a deterministic icon from the app name")
            .put("attempts", attempts)
    }

    /** Prefer the declared icon family, then compare original source resolution. */
    private fun better(candidate: Candidate, current: Candidate?): Boolean {
        if (current == null) return true
        fun rank(value: Candidate) = when {
            value.declared -> 2
            value.named -> 1
            else -> 0
        }
        if (rank(candidate) != rank(current)) return rank(candidate) > rank(current)
        val candidatePixels = candidate.sourceWidth.toLong() * candidate.sourceHeight
        val currentPixels = current.sourceWidth.toLong() * current.sourceHeight
        if (candidatePixels != currentPixels) return candidatePixels > currentPixels
        val candidateLongest = maxOf(candidate.sourceWidth, candidate.sourceHeight)
        val currentLongest = maxOf(current.sourceWidth, current.sourceHeight)
        if (candidateLongest != currentLongest) return candidateLongest > currentLongest
        if (candidate.scale != current.scale) return candidate.scale > current.scale
        return candidate.source < current.source
    }
}
