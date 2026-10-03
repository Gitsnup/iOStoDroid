package dev.radek.conventor

import android.util.Xml
import org.xmlpull.v1.XmlPullParser
import java.io.ByteArrayInputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Bounded XML and binary property list reader, independent of application UI. */
object Plist {
    fun read(bytes: ByteArray): Map<String, Any?> {
        require(bytes.size <= 8 * 1024 * 1024) { "Info.plist exceeds 8 MiB" }
        val value = if (bytes.take(8).toByteArray().contentEquals("bplist00".toByteArray())) Binary(bytes).read() else xml(bytes)
        require(value is Map<*, *>) { "plist root must be a dictionary" }
        return value.entries.associate { (k, v) -> require(k is String); k to v }
    }

    private fun xml(bytes: ByteArray): Any? {
        // XmlPullParser does not retrieve external DTDs. Custom entity declarations
        // are disallowed, while the ordinary Apple plist DOCTYPE remains valid.
        require(!bytes.toString(Charsets.UTF_8).contains("<!ENTITY", ignoreCase = true)) { "Custom XML entities prohibited" }
        val parser = Xml.newPullParser()
        parser.setInput(ByteArrayInputStream(bytes), null)
        var count = 0
        fun element(depth: Int): Any? {
            require(depth <= 128 && ++count <= 100000) { "plist complexity limit" }
            return when (parser.name) {
                "plist" -> { parser.nextTag(); val child = element(depth + 1); require(parser.nextTag() == XmlPullParser.END_TAG && parser.name == "plist"); child }
                "dict" -> {
                    val map = linkedMapOf<String, Any?>()
                    while (parser.nextTag() == XmlPullParser.START_TAG) {
                        require(parser.name == "key") { "dictionary key expected" }
                        val key = parser.nextText()
                        require(!map.containsKey(key)) { "duplicate plist key" }
                        require(parser.nextTag() == XmlPullParser.START_TAG)
                        map[key] = element(depth + 1)
                    }
                    map
                }
                "array" -> { val list = mutableListOf<Any?>(); while (parser.nextTag() == XmlPullParser.START_TAG) list.add(element(depth + 1)); list }
                "string", "key", "date" -> parser.nextText()
                "integer" -> parser.nextText().trim().toLong()
                "real" -> parser.nextText().trim().toDouble()
                "data" -> android.util.Base64.decode(parser.nextText(), android.util.Base64.DEFAULT)
                "true" -> { require(parser.nextText().isBlank()); true }
                "false" -> { require(parser.nextText().isBlank()); false }
                else -> error("unsupported plist XML element: ${parser.name}")
            }
        }
        require(parser.nextTag() == XmlPullParser.START_TAG && parser.name == "plist")
        val result = element(0)
        var event = parser.next()
        while (event == XmlPullParser.TEXT && parser.isWhitespace) event = parser.next()
        require(event == XmlPullParser.END_DOCUMENT) { "trailing XML data" }
        return result
    }

    private class Binary(private val bytes: ByteArray) {
        private var offsetSize = 0
        private var refSize = 0
        private var objectCount = 0
        private var tableOffset = 0
        private val offsets = mutableListOf<Int>()
        private val memo = mutableMapOf<Int, Any?>()
        private val visiting = mutableSetOf<Int>()
        private fun uint(pos: Int, size: Int, bound: Int = bytes.size): Long {
            require(size in 1..8 && pos >= 0 && pos <= bound - size) { "binary plist range" }
            var value = 0L
            repeat(size) { value = (value shl 8) or (bytes[pos + it].toLong() and 255) }
            return value
        }
        private fun bounded(value: Long, max: Int): Int { require(value in 0..max.toLong()) { "binary plist count/range" }; return value.toInt() }
        fun read(): Any? {
            require(bytes.size >= 40)
            val trailer = bytes.size - 32
            offsetSize = uint(trailer + 6, 1).toInt(); refSize = uint(trailer + 7, 1).toInt()
            require(offsetSize in 1..8 && refSize in 1..8)
            objectCount = bounded(uint(trailer + 8, 8), 100000); require(objectCount > 0)
            val top = bounded(uint(trailer + 16, 8), objectCount - 1)
            tableOffset = bounded(uint(trailer + 24, 8), trailer); require(tableOffset >= 8)
            require(objectCount.toLong() * offsetSize <= trailer - tableOffset)
            repeat(objectCount) { offsets.add(bounded(uint(tableOffset + it * offsetSize, offsetSize, trailer), tableOffset - 1).also { p -> require(p >= 8) }) }
            return obj(top, 0)
        }
        private fun obj(index: Int, depth: Int): Any? {
            require(index in 0 until objectCount && depth <= 128)
            if (memo.containsKey(index)) return memo[index]
            require(visiting.add(index)) { "cyclic binary plist" }
            var p = offsets[index]
            val marker = uint(p++, 1, tableOffset).toInt()
            val kind = marker shr 4
            val low = marker and 15
            fun length(): Int {
                if (low < 15) return low
                val integer = uint(p++, 1, tableOffset).toInt()
                require(integer shr 4 == 1 && (integer and 15) <= 3)
                val size = 1 shl (integer and 15)
                return bounded(uint(p, size, tableOffset), 1000000).also { p += size }
            }
            fun ref(at: Int) = bounded(uint(at, refSize, tableOffset), objectCount - 1)
            fun range(n: Int) { require(n >= 0 && p <= tableOffset - n) { "binary plist payload outside object area" } }
            val value: Any? = when (kind) {
                0 -> when (low) { 0 -> null; 8 -> false; 9 -> true; else -> error("unsupported plist simple value") }
                1 -> { require(low <= 3); uint(p, 1 shl low, tableOffset) }
                2 -> { require(low == 2 || low == 3); range(1 shl low); val b = ByteBuffer.wrap(bytes, p, 1 shl low).order(ByteOrder.BIG_ENDIAN); if (low == 2) b.float.toDouble() else b.double }
                3 -> { require(low == 3); range(8); ByteBuffer.wrap(bytes, p, 8).order(ByteOrder.BIG_ENDIAN).double + 978307200.0 }
                4 -> { val n = length(); range(n); bytes.copyOfRange(p, p + n) }
                5, 6 -> { val n = length(); val size = n * if (kind == 6) 2 else 1; range(size); String(bytes, p, size, if (kind == 6) Charsets.UTF_16BE else Charsets.US_ASCII) }
                8 -> { require(low < 8); uint(p, low + 1, tableOffset) }
                10 -> { val n = length(); range(n * refSize); List(n) { obj(ref(p + it * refSize), depth + 1) } }
                13 -> {
                    val n = length(); range(n * refSize * 2)
                    val map = linkedMapOf<String, Any?>()
                    repeat(n) {
                        val key = obj(ref(p + it * refSize), depth + 1)
                        require(key is String && !map.containsKey(key)) { "invalid/duplicate binary plist key" }
                        map[key] = obj(ref(p + (n + it) * refSize), depth + 1)
                    }
                    map
                }
                else -> error("unsupported binary plist object type $kind")
            }
            visiting.remove(index); memo[index] = value
            return value
        }
    }
}
