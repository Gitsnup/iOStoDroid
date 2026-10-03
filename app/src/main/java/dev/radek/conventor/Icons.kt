package dev.radek.conventor

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
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.zip.CRC32
import java.util.zip.Deflater
import java.util.zip.Inflater

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
    /** Decode bytes to a bitmap, repairing Apple `CgBI` payloads when needed. */
    fun decode(data: ByteArray, sample: Int = 1): Bitmap? {
        val options = BitmapFactory.Options()
        options.inSampleSize = maxOf(1, sample)
        BitmapFactory.decodeByteArray(data, 0, data.size, options)?.let { return it }
        val repaired = repairCgbi(data) ?: return null
        val bitmap = BitmapFactory.decodeByteArray(repaired, 0, repaired.size, options) ?: return null
        return swapRedBlue(bitmap)
    }

    private fun swapRedBlue(bitmap: Bitmap): Bitmap {
        val width = bitmap.width
        val height = bitmap.height
        val pixels = IntArray(width * height)
        bitmap.getPixels(pixels, 0, width, 0, 0, width, height)
        for (index in pixels.indices) {
            val value = pixels[index]
            val alpha = value and 0xFF000000.toInt()
            val red = (value shr 16) and 0xFF
            val green = (value shr 8) and 0xFF
            val blue = value and 0xFF
            pixels[index] = alpha or (blue shl 16) or (green shl 8) or red
        }
        val swapped = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        swapped.setPixels(pixels, 0, width, 0, 0, width, height)
        if (bitmap != swapped) bitmap.recycle()
        return swapped
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

    // ------------------------------------------------------------- CgBI repair
    /**
     * Rewrite an Apple `CgBI` PNG as a standard PNG: the `CgBI` chunk is dropped
     * and the raw-deflate IDAT stream is re-wrapped in zlib, which is what
     * `BitmapFactory` requires. Returns null when the payload is not a CgBI PNG.
     */
    fun repairCgbi(data: ByteArray): ByteArray? {
        if (data.size < 33 || !startsWith(data, PNG_SIGNATURE)) return null
        var at = 8
        var cgbi = false
        val idat = ByteArrayOutputStream()
        val chunks = mutableListOf<Triple<String, ByteArray, Boolean>>()
        while (at + 12 <= data.size) {
            val length = readU32(data, at)
            val type = String(data, at + 4, 4, Charsets.US_ASCII)
            if (length < 0 || at + 12 + length > data.size) return null
            val payload = data.copyOfRange(at + 8, at + 8 + length)
            if (type == "CgBI") cgbi = true
            if (type == "IDAT") idat.write(payload) else chunks.add(Triple(type, payload, false))
            if (type == "IEND") break
            at += 12 + length
        }
        if (!cgbi) return null
        val raw = idat.toByteArray()
        if (raw.isEmpty()) return null
        val inflated = try {
            rawInflate(raw)
        } catch (error: Exception) {
            return null
        } ?: return null
        val out = ByteArrayOutputStream()
        out.write(PNG_SIGNATURE)
        fun writeChunk(type: String, payload: ByteArray) {
            val name = type.toByteArray(Charsets.US_ASCII)
            writeU32(out, payload.size)
            out.write(name)
            out.write(payload)
            val crc = CRC32()
            crc.update(name)
            crc.update(payload)
            writeU32(out, crc.value.toInt())
        }
        for ((type, payload, _) in chunks) {
            if (type == "CgBI") continue
            if (type == "IDAT" || type == "IEND") continue
            writeChunk(type, payload)
        }
        writeChunk("IDAT", zlibDeflate(inflated))
        writeChunk("IEND", ByteArray(0))
        return out.toByteArray()
    }

    private fun rawInflate(data: ByteArray): ByteArray? {
        val inflater = Inflater(true)                       // nowrap: raw deflate stream
        inflater.setInput(data)
        val out = ByteArrayOutputStream()
        val buffer = ByteArray(65536)
        try {
            while (!inflater.finished() && out.size() < 64 * 1024 * 1024) {
                val count = inflater.inflate(buffer)
                if (count == 0) {
                    if (inflater.needsInput() || inflater.needsDictionary()) break
                }
                out.write(buffer, 0, count)
            }
        } finally {
            inflater.end()
        }
        return if (out.size() == 0) null else out.toByteArray()
    }

    private fun zlibDeflate(data: ByteArray): ByteArray {
        val deflater = Deflater(6, false)                   // zlib wrapper
        deflater.setInput(data)
        deflater.finish()
        val out = ByteArrayOutputStream()
        val buffer = ByteArray(65536)
        while (!deflater.finished()) out.write(buffer, 0, deflater.deflate(buffer))
        deflater.end()
        return out.toByteArray()
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

    private fun writeU32(out: ByteArrayOutputStream, value: Int) {
        out.write((value ushr 24) and 0xFF); out.write((value ushr 16) and 0xFF)
        out.write((value ushr 8) and 0xFF); out.write(value and 0xFF)
    }

    private fun startsWith(data: ByteArray, prefix: ByteArray, at: Int = 0): Boolean {
        if (at + prefix.size > data.size) return false
        for (index in prefix.indices) if (data[at + index] != prefix[index]) return false
        return true
    }

    private data class Candidate(
        val source: String, val bitmap: Bitmap, val declared: Boolean, val scale: Int, val named: Boolean
    )

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
        fun consider(source: String, data: ByteArray, declaredFile: Boolean, namedFile: Boolean) {
            if (data.isEmpty() || data.size > MAX_IMAGE_BYTES) {
                attempt(source, false, "payload outside the size limit"); return
            }
            val bounds = BitmapFactory.Options()
            bounds.inJustDecodeBounds = true
            BitmapFactory.decodeByteArray(data, 0, data.size, bounds)
            if (bounds.outWidth in 1..8192 && bounds.outHeight in 1..8192) {
                val sample = maxOf(1, maxOf(bounds.outWidth, bounds.outHeight) / TARGET)
                val bitmap = decode(data, sample)
                if (bitmap != null) {
                    if (isOpaque(bitmap)) {
                        attempt(source, true, "decoded", bitmap.width, bitmap.height)
                        val scale = when {
                            "@3x" in source -> 3
                            "@2x" in source -> 2
                            else -> 1
                        }
                        val candidate = Candidate(source, bitmap, declaredFile, scale, namedFile)
                        if (better(candidate, best)) {
                            best?.bitmap?.recycle()
                            best = candidate
                        } else bitmap.recycle()
                        return
                    }
                    bitmap.recycle()
                    attempt(source, false, "decoded but fully transparent")
                    return
                }
            }
            attempt(source, false, "BitmapFactory could not decode this payload")
        }

        var examined = 0
        for (file in queue) {
            if (examined >= 48) break
            examined++
            val relative = file.relativeTo(app).path
            consider(relative, file.readBytes(), declared.contains(file), named.contains(file))
        }
        for (catalog in app.walkTopDown().filter { it.isFile && it.name == "Assets.car" }) {
            val relative = catalog.relativeTo(app).path
            val payloads = catalogPayloads(catalog)
            attempt(relative, payloads.isNotEmpty(), "${payloads.size} embedded image payloads")
            payloads.forEachIndexed { index, payload ->
                consider("$relative#$index", payload, false, false)
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
                .put("kind", if ("#" in chosen.source) "asset-catalog" else "file")
                .put("format", if ("#" in chosen.source) "car-payload" else chosen.source.substringAfterLast('.', "png"))
                .put("decoder", "android.graphics.BitmapFactory")
                .put("width", width).put("height", height)
                .put("scale", chosen.scale.toDouble())
                .put("reason", "Decoded bundle icon (${chosen.source}, ${width}x${height})")
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

    /** Declared Info.plist icons win, then icon-named files, then pixel count. */
    private fun better(candidate: Candidate, current: Candidate?): Boolean {
        if (current == null) return true
        fun rank(value: Candidate) = when {
            value.declared -> 2
            value.named -> 1
            else -> 0
        }
        if (rank(candidate) != rank(current)) return rank(candidate) > rank(current)
        val candidatePixels = candidate.bitmap.width * candidate.bitmap.height
        val currentPixels = current.bitmap.width * current.bitmap.height
        if (candidatePixels != currentPixels) return candidatePixels > currentPixels
        return candidate.scale > current.scale
    }
}
