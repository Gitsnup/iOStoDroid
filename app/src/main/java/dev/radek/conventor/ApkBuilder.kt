package dev.radek.conventor

import java.io.ByteArrayOutputStream
import java.io.File

/**
 * Assembles and signs a complete APK on the device: store/deflate ZIP, JAR (v1)
 * signature for Android 6.0 and below, then the APK Signature Scheme v2 block so
 * the same file verifies on Android 7.0+.
 */
object ApkBuilder {
    private fun putU32(data: ByteArray, at: Int, value: Int) {
        data[at] = (value and 0xFF).toByte()
        data[at + 1] = ((value ushr 8) and 0xFF).toByte()
        data[at + 2] = ((value ushr 16) and 0xFF).toByte()
        data[at + 3] = ((value ushr 24) and 0xFF).toByte()
    }

    private fun readU32(data: ByteArray, at: Int): Int =
        (data[at].toInt() and 255) or ((data[at + 1].toInt() and 255) shl 8) or
            ((data[at + 2].toInt() and 255) shl 16) or ((data[at + 3].toInt() and 255) shl 24)

    private fun base64(value: ByteArray) = java.util.Base64.getEncoder().encodeToString(value)

    /**
     * @param entries ordered archive members; the payload entries are signed, the
     *   `META-INF` members are appended afterwards exactly like `apksigner` does
     * @param keyDirectory where the persistent signing identity is stored
     */
    fun build(entries: List<Pair<String, ByteArray>>, keyDirectory: File): ByteArray {
        val identity = ApkSign.identity(keyDirectory)
        val zip = ZipWriter()
        for ((name, data) in entries) zip.add(name, data)

        val digests = zip.entries.map { it.name to base64(it.digest) }
        val manifest = ApkSign.manifestMf(digests)
        val blocks = ApkSign.manifestBlocks(digests)
        val signatureFile = ApkSign.signatureSf(manifest, blocks)
        val rsa = ApkSign.pkcs7(identity, signatureFile)
        zip.add("META-INF/RADEK.SF", signatureFile)
        zip.add("META-INF/RADEK.RSA", rsa)
        zip.add("META-INF/MANIFEST.MF", manifest)
        val body = zip.finish()

        val directory = ApkSign.centralDirectory(body)
        val cdOffset = directory[0]
        val cdSize = directory[1]
        val eocdOffset = directory[2]
        val eocd = body.copyOfRange(eocdOffset, body.size)

        // Section 4 is digested with the EOCD "offset of start of central
        // directory" field treated as the offset of the APK signing block.
        val eocdForDigest = eocd.copyOf()
        putU32(eocdForDigest, 16, cdOffset)
        val v2Value = ApkSign.v2Value(
            identity,
            listOf(body.copyOfRange(0, cdOffset), body.copyOfRange(cdOffset, cdOffset + cdSize), eocdForDigest)
        )
        val block = ApkSign.signingBlock(v2Value)

        val out = ByteArrayOutputStream()
        out.write(body, 0, cdOffset)
        out.write(block)
        out.write(body, cdOffset, eocdOffset - cdOffset)
        out.write(eocd)
        val bytes = out.toByteArray()
        putU32(bytes, bytes.size - 22 + 16, cdOffset + block.size)
        if (readU32(bytes, bytes.size - 22 + 16) != cdOffset + block.size) {
            throw IllegalStateException("EOCD central directory offset was not patched")
        }
        return bytes
    }
}
