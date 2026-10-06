package dev.radek.conventor

import android.graphics.Bitmap
import android.graphics.Color
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.zip.GZIPInputStream
import java.nio.charset.Charset
import java.util.ArrayDeque
import java.util.zip.DataFormatException
import java.util.zip.Inflater

/**
 * Read-only recovery of raster renditions from Apple's compiled Assets.car files.
 * The CAR/BOM format is undocumented, so every offset and count is bounded and
 * unsupported encodings are reported rather than guessed. This is deliberately
 * limited to icon recovery; it is not a resource or UIKit compatibility layer.
 */
internal object AssetCatalogIcon {
    private const val MAX_FILE_BYTES = 64 * 1024 * 1024
    private const val MAX_BLOCKS = 1 shl 20
    private const val MAX_RENDITIONS = 4096
    private const val MAX_TREE_DEPTH = 1 shl 16
    private const val MAX_RENDITION_BLOCK_BYTES = 8 * 1024 * 1024
    private const val MAX_INFLATED_BYTES = 32 * 1024 * 1024
    private const val MAX_DIMENSION = 8192
    private const val MAX_PIXELS = 16 * 1024 * 1024
    private val utf8: Charset = Charsets.UTF_8

    data class Attempt(val asset: String, val detail: String, val ok: Boolean)
    data class Extraction(
        val bitmap: Bitmap? = null,
        val asset: String? = null,
        val format: String? = null,
        val width: Int = 0,
        val height: Int = 0,
        val scale: Double = 1.0,
        val error: String? = null,
        val attempts: List<Attempt> = emptyList(),
    )

    private data class BomEntry(val key: ByteArray, val value: ByteArray)
    private data class Facet(val name: String, val attributes: List<Pair<Int, Int>>)
    private data class Rendition(
        val name: String,
        val filename: String,
        val width: Int,
        val height: Int,
        val scale: Double,
        val pixelFormat: Int,
        val payload: ByteArray,
        val encoding: String,
    )

    private fun matchesPreferred(value: String, preferred: String): Boolean {
        val stem = value.substringAfterLast('/').substringBeforeLast('.', value).lowercase()
        val wanted = preferred.substringAfterLast('/').substringBeforeLast('.', preferred).lowercase()
        if (stem == wanted) return true
        if (!stem.startsWith(wanted)) return false
        val suffix = stem.drop(wanted.length)
        return suffix.isNotEmpty() && (suffix.first() in "@~_-" || suffix.first().isDigit())
    }

    fun extract(file: File, preferredName: String?, targetSize: Int): Extraction {
        if (!file.isFile || file.length() !in 1..MAX_FILE_BYTES.toLong()) {
            return Extraction(error = "Assets.car is missing or exceeds the 64 MiB icon-recovery limit")
        }
        if (targetSize < 1) return Extraction(error = "invalid target icon size")
        val attempts = mutableListOf<Attempt>()
        try {
            val catalog = parse(file.readBytes())
            val preferred = preferredName?.trim()?.takeIf { it.isNotEmpty() }
            val candidates = catalog.sortedWith(
                compareBy<Rendition>(
                    { if (preferred != null && (matchesPreferred(it.name, preferred) || matchesPreferred(it.filename, preferred))) 0 else 1 },
                    { if (it.name.contains("icon", true) || it.filename.contains("icon", true)) 0 else 1 },
                    { -(it.width.toLong() * it.height) },
                    { -it.scale },
                    { if (it.width == it.height) 0 else 1 },
                    { it.name },
                ),
            )
            for (rendition in candidates) {
                val label = rendition.name.ifBlank { rendition.filename.ifBlank { "unnamed" } }
                val displayName = "${file.name}:$label"
                val bitmap = try {
                    decodeRendition(rendition, targetSize)
                } catch (e: Exception) {
                    attempts += Attempt(displayName, e.message ?: "rendition decode failed", false)
                    null
                }
                if (bitmap == null) {
                    if (attempts.none { it.asset == displayName }) {
                        attempts += Attempt(displayName, "unsupported or corrupt ${rendition.encoding} rendition", false)
                    }
                    continue
                }
                if (!Icons.isOpaque(bitmap)) {
                    bitmap.recycle()
                    attempts += Attempt(displayName, "decoded but fully transparent", false)
                    continue
                }
                attempts += Attempt(displayName, "decoded ${rendition.encoding} asset-catalog rendition", true)
                return Extraction(
                    bitmap = bitmap,
                    asset = displayName,
                    format = rendition.encoding,
                    width = rendition.width,
                    height = rendition.height,
                    scale = rendition.scale,
                    attempts = attempts,
                )
            }
            return Extraction(error = if (candidates.isEmpty()) "Assets.car contains no recognized image renditions" else "no Assets.car image rendition could be decoded", attempts = attempts)
        } catch (e: Exception) {
            return Extraction(error = e.message ?: "Assets.car could not be parsed", attempts = attempts)
        }
    }

    private fun parse(data: ByteArray): List<Rendition> {
        require(data.size in 32..MAX_FILE_BYTES && data.copyOfRange(0, 8).contentEquals("BOMStore".toByteArray(utf8))) {
            "not a bounded BOM/Assets.car file"
        }
        val count = u32be(data, 12).bounded(MAX_BLOCKS, "BOM block count")
        val indexOffset = u32be(data, 16).bounded(data.size, "BOM index offset")
        val indexLength = u32be(data, 20).bounded(data.size, "BOM index length")
        val varsOffset = u32be(data, 24).bounded(data.size, "BOM variables offset")
        val varsLength = u32be(data, 28).bounded(data.size, "BOM variables length")
        require(indexLength >= 4 && indexOffset <= data.size - indexLength) { "invalid BOM block index" }
        require(varsLength >= 4 && varsOffset <= data.size - varsLength) { "invalid BOM variables" }
        val declared = u32be(data, indexOffset).bounded(MAX_BLOCKS, "BOM declared block count")
        val blockCount = minOf(count, declared, (indexLength - 4) / 8)
        val blocks = ArrayList<Pair<Int, Int>>(blockCount)
        repeat(blockCount) { i ->
            val at = indexOffset + 4 + i * 8
            val offset = u32be(data, at).bounded(data.size, "BOM block offset")
            val length = u32be(data, at + 4).bounded(data.size, "BOM block length")
            require(offset <= data.size - length) { "BOM block outside file" }
            blocks += offset to length
        }

        fun block(index: Int): ByteArray {
            require(index in blocks.indices) { "BOM block index outside table" }
            val (offset, length) = blocks[index]
            return data.copyOfRange(offset, offset + length)
        }

        val variables = linkedMapOf<String, Int>()
        var variableAt = varsOffset + 4
        val varsEnd = varsOffset + varsLength
        var variableCount = 0
        while (variableAt + 5 <= varsEnd && variableCount < 4096) {
            val index = u32be(data, variableAt).bounded(blocks.size, "BOM variable index")
            val length = data[variableAt + 4].toInt() and 0xff
            variableAt += 5
            if (variableAt + length > varsEnd) break
            val name = String(data, variableAt, length, utf8)
            variableAt += length
            if (name.isNotEmpty() && index in blocks.indices) variables.putIfAbsent(name, index)
            variableCount++
        }

        fun walkTree(root: Int): List<BomEntry> {
            val result = ArrayList<BomEntry>()
            val queue = ArrayDeque<Int>()
            val visited = HashSet<Int>()
            var copiedRenditionBytes = 0L
            queue.add(root)
            while (queue.isNotEmpty() && result.size < MAX_RENDITIONS && visited.size < MAX_TREE_DEPTH) {
                val nodeIndex = queue.removeFirst()
                if (nodeIndex !in blocks.indices || !visited.add(nodeIndex)) continue
                val node = block(nodeIndex)
                require(node.size >= 12 && String(node, 0, 4, Charsets.US_ASCII) == "tree") { "invalid BOM tree node" }
                val entries = block(u32be(node, 8).bounded(blocks.size, "BOM tree entries index"))
                require(entries.size >= 12) { "truncated BOM tree entries" }
                val n = u16be(entries, 2)
                val safeCount = minOf(n, (entries.size - 12) / 8)
                repeat(safeCount) { i ->
                    val at = 12 + i * 8
                    val keyIndex = u32be(entries, at).bounded(blocks.size, "BOM key index")
                    val valueIndex = u32be(entries, at + 4).bounded(blocks.size, "BOM value index")
                    val (valueOffset, valueLength) = blocks[valueIndex]
                    val isTree = valueLength >= 4 && String(data, valueOffset, 4, Charsets.US_ASCII) == "tree"
                    if (isTree) {
                        queue.add(valueIndex)
                    } else if (valueLength <= MAX_RENDITION_BLOCK_BYTES && copiedRenditionBytes + valueLength <= 32L * 1024 * 1024) {
                        result += BomEntry(block(keyIndex), block(valueIndex))
                        copiedRenditionBytes += valueLength
                    }
                }
            }
            return result
        }

        val keyTokens = variables["KEYFORMAT"]?.let { parseKeyFormat(block(it)) }.orEmpty()
        val facets = variables["FACETKEYS"]?.let { root ->
            walkTree(root).mapNotNull { entry ->
                val name = String(entry.key, utf8)
                if (name.isEmpty()) null else Facet(name, parseFacet(entry.value))
            }
        }.orEmpty()
        val renditionRoot = variables["RENDITIONS"] ?: error("Assets.car has no RENDITIONS tree")
        val entries = walkTree(renditionRoot).take(MAX_RENDITIONS)
        val renditions = ArrayList<Rendition>(entries.size)
        for (entry in entries) {
            val values = keyValues(entry.key)
            val name = resolveName(values, keyTokens, facets)
            parseRendition(entry.value, name)?.let(renditions::add)
        }
        return renditions
    }

    private fun parseKeyFormat(data: ByteArray): List<Int> {
        require(data.size >= 12 && String(data, 0, 4, Charsets.US_ASCII) == "kfmt") { "invalid Assets.car KEYFORMAT" }
        val count = u32le(data, 8).bounded(64, "KEYFORMAT token count")
        require(12L + count * 4L <= data.size) { "truncated Assets.car KEYFORMAT" }
        return List(count) { i -> u32le(data, 12 + i * 4).bounded(64, "KEYFORMAT token") }
    }

    private fun parseFacet(data: ByteArray): List<Pair<Int, Int>> {
        if (data.size < 6) return emptyList()
        val count = u16le(data, 4)
        val safeCount = minOf(count, (data.size - 6) / 4)
        return List(safeCount) { i ->
            val at = 6 + i * 4
            u16le(data, at) to u16le(data, at + 2)
        }
    }

    private fun keyValues(data: ByteArray): List<Int> {
        val count = data.size / 2
        return List(count) { i -> u16le(data, i * 2) }
    }

    private fun resolveName(key: List<Int>, tokens: List<Int>, facets: List<Facet>): String {
        var selected = ""
        var bestScore = 0
        for (facet in facets) {
            var score = 0
            var matches = true
            for ((attribute, value) in facet.attributes) {
                val position = tokens.indexOf(attribute)
                if (position >= 0) {
                    if (position < key.size && key[position] == value) score++ else {
                        matches = false
                        break
                    }
                }
            }
            if (matches && score > bestScore) {
                selected = facet.name
                bestScore = score
            }
        }
        if (selected.isNotEmpty()) return selected
        val identifierPosition = tokens.indexOf(16)
        if (identifierPosition in key.indices) {
            val identifier = key[identifierPosition]
            return facets.firstOrNull { facet -> facet.attributes.any { it.first == 16 && it.second == identifier } }?.name.orEmpty()
        }
        return ""
    }

    private fun parseRendition(data: ByteArray, assetName: String): Rendition? {
        if (data.size < 184) return null
        val magic = String(data, 0, 4, Charsets.US_ASCII)
        if (magic != "CTSI" && magic != "ISTC") return null
        val width = u32le(data, 12)
        val height = u32le(data, 16)
        val scale = u32le(data, 20) / 100.0
        val pixelFormat = u32le(data, 24)
        val layout = u16le(data, 36)
        val name = cString(data, 40, 128)
        val tvlLength = u32le(data, 168).bounded(data.size, "rendition TLV length")
        val bitmapCount = u32le(data, 172).bounded(4096, "rendition bitmap count")
        val length = u32le(data, 180).bounded(data.size, "rendition payload length")
        if (width !in 1..MAX_DIMENSION || height !in 1..MAX_DIMENSION || width.toLong() * height > MAX_PIXELS || scale <= 0) return null
        // `layout` is recorded by Apple; do not reject a valid raster payload solely
        // because a newer catalog uses a previously unknown layout number.
        @Suppress("UNUSED_VARIABLE") val ignoredLayout = layout
        val starts = listOf(184L + bitmapCount * 4L + tvlLength, 184L + tvlLength, 184L)
        var payload: ByteArray? = null
        var encoding = "unknown"
        for (startLong in starts) {
            if (startLong >= data.size) continue
            val start = startLong.toInt()
            val available = data.size - start
            val candidateLength = if (length > 0) length else available
            if (candidateLength <= 0 || candidateLength > available) continue
            val candidate = data.copyOfRange(start, start + candidateLength)
            val sniffed = sniff(candidate)
            if (sniffed != "unknown") {
                payload = candidate
                encoding = sniffed
                break
            }
        }
        if (payload == null) {
            val start = starts.min().coerceAtMost(data.size.toLong()).toInt()
            payload = data.copyOfRange(start, data.size)
            encoding = sniff(payload)
        }
        return Rendition(name = assetName, filename = name, width = width, height = height, scale = scale,
            pixelFormat = pixelFormat, payload = payload, encoding = encoding)
    }

    private fun decodeRendition(rendition: Rendition, targetSize: Int): Bitmap? {
        val payload = rendition.payload
        when (rendition.encoding) {
            "png", "jpeg" -> return IconDecoder.decode(payload, targetSize)
            "gzip", "zlib" -> {
                val inflated = inflate(payload, rendition.encoding == "gzip") ?: return null
                when (sniff(inflated)) {
                    "png", "jpeg" -> return IconDecoder.decode(inflated, targetSize)
                    else -> return decodeRaw(rendition, inflated, targetSize)
                }
            }
            "lzfse" -> {
                val inflated = decodeUncompressedLzfse(payload) ?: return null
                return decodeRaw(rendition, inflated, targetSize)
            }
            "unknown" -> return decodeRaw(rendition, payload, targetSize)
            else -> return null
        }
    }

    private fun decodeRaw(rendition: Rendition, data: ByteArray, targetSize: Int): Bitmap? {
        val fourcc = fourCc(rendition.pixelFormat)
        val bytesPerPixel = when (fourcc) { "ARGB" -> 4; "GA16", "RGB5" -> 2; "RGBW" -> 8; else -> return null }
        val count = rendition.width.toLong() * rendition.height
        val expected = count * bytesPerPixel
        if (expected > MAX_INFLATED_BYTES || expected > data.size) return null
        val sample = maxOf(1, maxOf(rendition.width, rendition.height) / targetSize)
        val outWidth = (rendition.width + sample - 1) / sample
        val outHeight = (rendition.height + sample - 1) / sample
        val colors = IntArray(outWidth * outHeight)
        var outputY = 0
        for (y in 0 until rendition.height step sample) {
            var outputX = 0
            for (x in 0 until rendition.width step sample) {
                val at = (y.toLong() * rendition.width + x) * bytesPerPixel
                val color = when (fourcc) {
                    // Apple catalogs store ARGB-format pixels in premultiplied BGRA byte order.
                    "ARGB" -> {
                        val b = data[at.toInt()].toInt() and 0xff
                        val g = data[at.toInt() + 1].toInt() and 0xff
                        val r = data[at.toInt() + 2].toInt() and 0xff
                        val a = data[at.toInt() + 3].toInt() and 0xff
                        Color.argb(a, unpremultiply(r, a), unpremultiply(g, a), unpremultiply(b, a))
                    }
                    "GA16" -> {
                        val gray = data[at.toInt()].toInt() and 0xff
                        val alpha = data[at.toInt() + 1].toInt() and 0xff
                        val value = unpremultiply(gray, alpha)
                        Color.argb(alpha, value, value, value)
                    }
                    "RGB5" -> {
                        val word = (data[at.toInt()].toInt() and 0xff) or ((data[at.toInt() + 1].toInt() and 0xff) shl 8)
                        val red = ((word shr 10) and 31) * 8
                        val green = ((word shr 5) and 31) * 8
                        val blue = (word and 31) * 8
                        Color.argb(if ((word and 1) != 0) 255 else 0, red, green, blue)
                    }
                    else -> {
                        val red = u16le(data, at.toInt()) ushr 8
                        val green = u16le(data, at.toInt() + 2) ushr 8
                        val blue = u16le(data, at.toInt() + 4) ushr 8
                        Color.rgb(red, green, blue)
                    }
                }
                colors[outputY * outWidth + outputX] = color
                outputX++
            }
            outputY++
        }
        return Bitmap.createBitmap(colors, outWidth, outHeight, Bitmap.Config.ARGB_8888)
    }

    private fun decodeUncompressedLzfse(input: ByteArray): ByteArray? {
        val out = ByteArrayOutputStream()
        var at = 0
        while (at + 8 <= input.size) {
            val magic = String(input, at, 4, Charsets.US_ASCII)
            if (magic == "bvxe") return out.toByteArray().takeIf { it.isNotEmpty() }
            if (magic != "bvxn") return null // LZFSE V1/V2 are intentionally not approximated.
            val length = u32le(input, at + 4)
            if (length > MAX_INFLATED_BYTES || at + 8L + length > input.size || out.size().toLong() + length > MAX_INFLATED_BYTES) return null
            out.write(input, at + 8, length)
            at += 8 + length
        }
        return null
    }

    private fun inflate(input: ByteArray, gzip: Boolean): ByteArray? {
        return try {
            if (gzip) {
                GZIPInputStream(ByteArrayInputStream(input)).use { readBounded(it) }
            } else {
                val inflater = Inflater(false)
                try {
                    inflater.setInput(input)
                    val out = ByteArrayOutputStream()
                    val buffer = ByteArray(16 * 1024)
                    while (!inflater.finished()) {
                        val count = inflater.inflate(buffer)
                        if (count == 0) {
                            if (inflater.needsInput() || inflater.needsDictionary()) break
                            throw DataFormatException("zlib decoder made no progress")
                        }
                        if (out.size().toLong() + count > MAX_INFLATED_BYTES) return null
                        out.write(buffer, 0, count)
                    }
                    out.toByteArray().takeIf { inflater.finished() && inflater.remaining == 0 }
                } finally { inflater.end() }
            }
        } catch (_: Exception) { null }
    }

    private fun readBounded(input: GZIPInputStream): ByteArray? {
        input.use {
            val out = ByteArrayOutputStream()
            val buffer = ByteArray(16 * 1024)
            while (true) {
                val count = it.read(buffer)
                if (count < 0) break
                if (out.size().toLong() + count > MAX_INFLATED_BYTES) return null
                out.write(buffer, 0, count)
            }
            return out.toByteArray()
        }
    }

    private fun sniff(data: ByteArray): String = when {
        data.size >= 8 && data[0] == 0x89.toByte() && data[1] == 0x50.toByte() && data[2] == 0x4e.toByte() && data[3] == 0x47.toByte() -> "png"
        data.size >= 3 && data[0] == 0xff.toByte() && data[1] == 0xd8.toByte() && data[2] == 0xff.toByte() -> "jpeg"
        data.size >= 8 && String(data, 4, 4, Charsets.US_ASCII) == "ftyp" -> "heif"
        data.size >= 4 && String(data, 0, 4, Charsets.US_ASCII) in setOf("bvx1", "bvx2", "bvxn", "bvxe") -> "lzfse"
        data.size >= 2 && data[0] == 0x1f.toByte() && data[1] == 0x8b.toByte() -> "gzip"
        data.size >= 2 && data[0] == 0x78.toByte() && (data[1] == 0x01.toByte() || data[1] == 0x9c.toByte() || data[1] == 0xda.toByte() || data[1] == 0x5e.toByte()) -> "zlib"
        else -> "unknown"
    }

    private fun fourCc(value: Int): String = String(byteArrayOf(value.toByte(), (value ushr 8).toByte(), (value ushr 16).toByte(), (value ushr 24).toByte()), Charsets.US_ASCII)
    private fun cString(data: ByteArray, offset: Int, size: Int): String {
        val end = (offset until minOf(data.size, offset + size)).firstOrNull { data[it].toInt() == 0 } ?: minOf(data.size, offset + size)
        return String(data, offset, end - offset, utf8)
    }
    private fun unpremultiply(channel: Int, alpha: Int): Int = when { alpha == 0 -> 0; alpha == 255 -> channel; else -> minOf(255, (channel * 255 + alpha / 2) / alpha) }
    private fun u16be(data: ByteArray, at: Int): Int { require(at >= 0 && at + 2 <= data.size); return ((data[at].toInt() and 0xff) shl 8) or (data[at + 1].toInt() and 0xff) }
    private fun u16le(data: ByteArray, at: Int): Int { require(at >= 0 && at + 2 <= data.size); return (data[at].toInt() and 0xff) or ((data[at + 1].toInt() and 0xff) shl 8) }
    private fun u32be(data: ByteArray, at: Int): Long {
        require(at >= 0 && at + 4 <= data.size)
        return ((data[at].toLong() and 0xff) shl 24) or ((data[at + 1].toLong() and 0xff) shl 16) or ((data[at + 2].toLong() and 0xff) shl 8) or (data[at + 3].toLong() and 0xff)
    }
    private fun u32le(data: ByteArray, at: Int): Int {
        require(at >= 0 && at + 4 <= data.size)
        return (data[at].toInt() and 0xff) or ((data[at + 1].toInt() and 0xff) shl 8) or ((data[at + 2].toInt() and 0xff) shl 16) or ((data[at + 3].toInt() and 0xff) shl 24)
    }
    private fun Long.bounded(max: Int, label: String): Int {
        require(this >= 0 && this <= max.toLong()) { "$label exceeds limit" }
        return toInt()
    }
    private fun Int.bounded(max: Int, label: String): Int {
        require(this >= 0 && this <= max) { "$label exceeds limit" }
        return this
    }
}
