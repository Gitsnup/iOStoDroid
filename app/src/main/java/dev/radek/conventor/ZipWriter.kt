package dev.radek.conventor

import java.io.ByteArrayOutputStream
import java.security.MessageDigest
import java.util.zip.CRC32
import java.util.zip.Deflater

/**
 * Store/deflate ZIP writer with the exact byte layout the APK v1 and v2 signing
 * schemes are computed over.
 *
 * `java.util.zip.ZipOutputStream` is deliberately not used: APK signing needs the
 * SHA-256 of every *uncompressed* entry and the central directory offset while
 * the archive is still being assembled.
 */
class ZipWriter {
    data class Entry(val name: String, val digest: ByteArray, val offset: Long)

    private val parts = ByteArrayOutputStream()
    private val central = ByteArrayOutputStream()
    val entries = mutableListOf<Entry>()

    private fun put16(out: ByteArrayOutputStream, value: Int) {
        out.write(value and 0xFF); out.write((value ushr 8) and 0xFF)
    }

    private fun put32(out: ByteArrayOutputStream, value: Long) {
        val v = value.toInt()
        out.write(v and 0xFF); out.write((v ushr 8) and 0xFF)
        out.write((v ushr 16) and 0xFF); out.write((v ushr 24) and 0xFF)
    }

    /** @return the local header offset of the entry inside the archive */
    fun add(name: String, data: ByteArray, compress: Boolean = true): Long {
        var payload = data
        var method = 0
        if (compress && data.isNotEmpty()) {
            val deflater = Deflater(9, true)               // nowrap: raw deflate stream
            deflater.setInput(data)
            deflater.finish()
            val buffer = ByteArrayOutputStream()
            val chunk = ByteArray(16384)
            while (!deflater.finished()) buffer.write(chunk, 0, deflater.deflate(chunk))
            deflater.end()
            val raw = buffer.toByteArray()
            if (raw.isNotEmpty() && raw.size < data.size) {
                payload = raw
                method = 8
            }
        }
        val crc = CRC32()
        crc.update(data)
        val offset = parts.size().toLong()
        val encoded = name.toByteArray(Charsets.UTF_8)

        parts.write(byteArrayOf(0x50, 0x4B, 0x03, 0x04))
        put16(parts, 20)                                    // version needed to extract
        put16(parts, 0)                                     // general purpose flags
        put16(parts, method)
        put16(parts, 0); put16(parts, 0)                    // modification time / date
        put32(parts, crc.value)
        put32(parts, payload.size.toLong())
        put32(parts, data.size.toLong())
        put16(parts, encoded.size)
        put16(parts, 0)                                     // extra field length
        parts.write(encoded)
        parts.write(payload)

        central.write(byteArrayOf(0x50, 0x4B, 0x01, 0x02))
        put16(central, 20)                                  // version made by
        put16(central, 20)                                  // version needed
        put16(central, 0); put16(central, method)
        put16(central, 0); put16(central, 0)
        put32(central, crc.value)
        put32(central, payload.size.toLong())
        put32(central, data.size.toLong())
        put16(central, encoded.size)
        put16(central, 0); put16(central, 0)                // extra / comment length
        put16(central, 0); put16(central, 0)                // disk start / internal attrs
        put32(central, 0)                                   // external attrs
        put32(central, offset)
        central.write(encoded)

        entries.add(Entry(name, MessageDigest.getInstance("SHA-256").digest(data), offset))
        return offset
    }

    /** Assemble local headers, central directory and EOCD (no ZIP comment). */
    fun finish(): ByteArray {
        val count = entries.size
        val body = ByteArrayOutputStream()
        body.write(parts.toByteArray())
        val cdOffset = body.size().toLong()
        body.write(central.toByteArray())
        body.write(byteArrayOf(0x50, 0x4B, 0x05, 0x06))
        put16(body, 0); put16(body, 0)                      // disk numbers
        put16(body, count); put16(body, count)
        put32(body, central.size().toLong())
        put32(body, cdOffset)
        put16(body, 0)                                      // comment length
        return body.toByteArray()
    }
}
