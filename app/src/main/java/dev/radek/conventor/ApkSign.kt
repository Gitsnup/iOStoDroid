package dev.radek.conventor

import java.io.ByteArrayOutputStream
import java.io.File
import java.math.BigInteger
import java.security.KeyFactory
import java.security.KeyPairGenerator
import java.security.MessageDigest
import java.security.PrivateKey
import java.security.PublicKey
import java.security.Signature
import java.security.spec.PKCS8EncodedKeySpec
import java.security.spec.X509EncodedKeySpec
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone

/**
 * Self-contained APK signer: DER encoder, self-signed X.509 certificate,
 * PKCS#7 `CERT.RSA`, JAR (v1) manifests and the APK Signature Scheme v2 block.
 *
 * The device has no `apksigner`, and a converted APK must install and update, so
 * one RSA identity is generated on first use and persisted in the app's private
 * storage. Every converted APK on a device is therefore signed by a stable key.
 */
object ApkSign {
    private const val V2_BLOCK_ID = 0x7109871A
    private const val SIGNATURE_RSA_PKCS1_SHA256 = 0x0103
    private const val CHUNK = 1 shl 20
    private val MAGIC = "APK Sig Block 42".toByteArray(Charsets.US_ASCII)

    private val SHA256_RSA = hex("2a864886f70d01010b")      // 1.2.840.113549.1.1.11
    private val RSA_ENC = hex("2a864886f70d010101")          // 1.2.840.113549.1.1.1
    private val OID_SHA256 = hex("608648016503040201")       // 2.16.840.1.101.3.4.2.1
    private val OID_DATA = hex("2a864886f70d010701")         // 1.2.840.113549.1.7.1
    private val OID_SIGNED = hex("2a864886f70d010702")       // 1.2.840.113549.1.7.2
    private val OID_CN = hex("550403")
    private val OID_O = hex("55040a")
    private val OID_C = hex("550406")

    const val SUBJECT_CN = "Radek Development"
    const val SUBJECT_ORG = "Radek"
    const val SUBJECT_COUNTRY = "US"
    private val SERIAL = BigInteger.ONE

    private fun hex(value: String): ByteArray {
        val out = ByteArray(value.length / 2)
        for (i in out.indices) out[i] = value.substring(i * 2, i * 2 + 2).toInt(16).toByte()
        return out
    }

    // ------------------------------------------------------------------- DER
    private fun length(size: Int): ByteArray {
        if (size < 0x80) return byteArrayOf(size.toByte())
        val body = ByteArrayOutputStream()
        var remaining = size
        while (remaining > 0) {
            body.write(remaining and 0xFF)
            remaining = remaining ushr 8
        }
        val bytes = body.toByteArray().reversedArray()
        return byteArrayOf((0x80 or bytes.size).toByte()) + bytes
    }

    private fun tlv(tag: Int, payload: ByteArray): ByteArray =
        byteArrayOf(tag.toByte()) + length(payload.size) + payload

    private fun seq(vararg parts: ByteArray) = tlv(0x30, concat(*parts))
    private fun setOf(vararg parts: ByteArray) = tlv(0x31, concat(*parts))
    private fun oid(raw: ByteArray) = tlv(0x06, raw)
    private fun integer(value: BigInteger) = tlv(0x02, value.toByteArray())
    private fun octets(payload: ByteArray) = tlv(0x04, payload)
    private fun bitString(payload: ByteArray) = tlv(0x03, byteArrayOf(0) + payload)
    private fun printable(text: String) = tlv(0x13, text.toByteArray(Charsets.US_ASCII))
    private fun utcTime(date: Date) = tlv(0x17, utcFormat().format(date).toByteArray(Charsets.US_ASCII))
    private fun explicit(index: Int, payload: ByteArray) = tlv(0xA0 or index, payload)
    private fun nullTag() = tlv(0x05, ByteArray(0))

    private fun utcFormat(): SimpleDateFormat {
        val format = SimpleDateFormat("yyMMddHHmmss'Z'", Locale.US)
        format.timeZone = TimeZone.getTimeZone("UTC")
        return format
    }

    private fun concat(vararg parts: ByteArray): ByteArray {
        val out = ByteArrayOutputStream()
        parts.forEach { out.write(it) }
        return out.toByteArray()
    }

    private fun lengthPrefixed(payload: ByteArray): ByteArray {
        val out = ByteArrayOutputStream()
        writeU32(out, payload.size)
        out.write(payload)
        return out.toByteArray()
    }

    private fun writeU32(out: ByteArrayOutputStream, value: Int) {
        out.write(value and 0xFF); out.write((value ushr 8) and 0xFF)
        out.write((value ushr 16) and 0xFF); out.write((value ushr 24) and 0xFF)
    }

    private fun writeU64(out: ByteArrayOutputStream, value: Long) {
        for (shift in 0..56 step 8) out.write(((value ushr shift) and 0xFFL).toInt())
    }

    private fun distinguishedName(): ByteArray {
        fun rdn(raw: ByteArray, value: String) = setOf(seq(oid(raw), printable(value)))
        return seq(rdn(OID_C, SUBJECT_COUNTRY), rdn(OID_O, SUBJECT_ORG), rdn(OID_CN, SUBJECT_CN))
    }

    /**
     * A persisted signing identity. [certificate] is the DER X.509 that goes into
     * the v1 `CERT.RSA` and the v2 signer; [spki] is the DER SubjectPublicKeyInfo
     * that must equal the certificate's public key.
     */
    class Identity(
        val privateKey: PrivateKey,
        val publicKey: PublicKey,
        val certificate: ByteArray,
        val spki: ByteArray
    ) {
        fun sign(data: ByteArray): ByteArray {
            val signature = Signature.getInstance("SHA256withRSA")
            signature.initSign(privateKey)
            signature.update(data)
            return signature.sign()
        }
    }

    /**
     * Load the device identity, generating and persisting it on first use.
     *
     * The private key (PKCS#8), its public key (X.509 SubjectPublicKeyInfo) and the
     * certificate are all stored, so loading never has to cast a provider-specific
     * key class - `KeyFactory` implementations differ between Android releases.
     */
    fun identity(directory: File): Identity {
        directory.mkdirs()
        val keyFile = File(directory, "signing.pk8")
        val spkiFile = File(directory, "signing.spki")
        val certFile = File(directory, "signing.x509")
        val factory = KeyFactory.getInstance("RSA")
        if (keyFile.isFile && spkiFile.isFile && certFile.isFile) {
            val privateKey = factory.generatePrivate(PKCS8EncodedKeySpec(keyFile.readBytes()))
            val public = factory.generatePublic(X509EncodedKeySpec(spkiFile.readBytes()))
            return Identity(privateKey, public, certFile.readBytes(), spkiFile.readBytes())
        }
        val generator = KeyPairGenerator.getInstance("RSA")
        generator.initialize(2048)
        val pair = generator.generateKeyPair()
        val spki = pair.public.encoded                        // X.509 SubjectPublicKeyInfo DER
        val pending = Identity(pair.private, pair.public, ByteArray(0), spki)
        val certificate = certificate(spki, pending)
        keyFile.writeBytes(pair.private.encoded)              // PKCS#8 DER
        spkiFile.writeBytes(spki)
        certFile.writeBytes(certificate)
        return Identity(pair.private, pair.public, certificate, spki)
    }

    /** Self-signed X.509 v3 certificate, ten year validity, SHA-256 with RSA. */
    fun certificate(spki: ByteArray, signer: Identity): ByteArray {
        val name = distinguishedName()
        val now = Date()
        val expiry = Date(now.time + 3650L * 24 * 60 * 60 * 1000)
        val tbs = seq(
            explicit(0, integer(BigInteger.valueOf(2))),      // version v3
            integer(SERIAL),
            seq(oid(SHA256_RSA), nullTag()),
            name,
            seq(utcTime(now), utcTime(expiry)),
            name,                                             // subject == issuer: self-signed
            spki
        )
        return seq(tbs, seq(oid(SHA256_RSA), nullTag()), bitString(signer.sign(tbs)))
    }

    /** PKCS#7 SignedData over the JAR signature file (the `META-INF` .RSA member). */
    fun pkcs7(identity: Identity, data: ByteArray): ByteArray {
        val algorithm = seq(oid(OID_SHA256), nullTag())
        val signerInfo = seq(
            integer(BigInteger.ONE),
            seq(distinguishedName(), integer(SERIAL)),
            algorithm,
            seq(oid(SHA256_RSA), nullTag()),
            octets(identity.sign(data))
        )
        val signedData = seq(
            integer(BigInteger.ONE),
            setOf(algorithm),
            seq(oid(OID_DATA)),
            explicit(0, identity.certificate),
            setOf(signerInfo)
        )
        return seq(oid(OID_SIGNED), explicit(0, signedData))
    }

    // ---------------------------------------------------------------- v1 (JAR)
    /** Manifest lines are wrapped at 70 bytes with a single leading space. */
    private fun wrap(line: String): ByteArray {
        val bytes = line.toByteArray(Charsets.UTF_8)
        val chunks = mutableListOf<ByteArray>()
        var current = ByteArrayOutputStream()
        for (byte in bytes) {
            current.write(byte.toInt())
            if (current.size() == 70) {
                chunks.add(current.toByteArray())
                current = ByteArrayOutputStream()
            }
        }
        chunks.add(current.toByteArray())
        val out = ByteArrayOutputStream()
        chunks.forEachIndexed { index, chunk ->
            if (index > 0) { out.write(0x0D); out.write(0x0A); out.write(0x20) }
            out.write(chunk)
        }
        out.write(0x0D); out.write(0x0A)
        return out.toByteArray()
    }

    private fun base64(value: ByteArray) = java.util.Base64.getEncoder().encodeToString(value)

    private fun entryBlock(name: String, digest: String): ByteArray =
        concat(wrap("Name: $name"), wrap("SHA-256-Digest: $digest"), byteArrayOf(0x0D, 0x0A))

    fun manifestMf(entries: List<Pair<String, String>>): ByteArray {
        val out = ByteArrayOutputStream()
        out.write("Manifest-Version: 1.0\r\nBuilt-By: Generated-by-Radek\r\nCreated-By: 1.0 (Radek)\r\n\r\n"
            .toByteArray(Charsets.UTF_8))
        for ((name, digest) in entries) out.write(entryBlock(name, digest))
        return out.toByteArray()
    }

    /** The per-entry blocks exactly as they appear inside `MANIFEST.MF`. */
    fun manifestBlocks(entries: List<Pair<String, String>>): List<Pair<String, ByteArray>> =
        entries.map { (name, digest) -> name to entryBlock(name, digest) }

    fun signatureSf(manifest: ByteArray, blocks: List<Pair<String, ByteArray>>): ByteArray {
        val out = ByteArrayOutputStream()
        out.write(("Signature-Version: 1.0\r\nCreated-By: 1.0 (Radek)\r\n" +
            "SHA-256-Digest-Manifest: " + base64(sha256(manifest)) + "\r\n" +
            // Rollback protection: an APK carrying both schemes must declare v2.
            "X-Android-APK-Signed: 2\r\n\r\n").toByteArray(Charsets.UTF_8))
        for ((name, block) in blocks) {
            out.write(concat(wrap("Name: $name"), wrap("SHA-256-Digest: " + base64(sha256(block))),
                byteArrayOf(0x0D, 0x0A)))
        }
        return out.toByteArray()
    }

    fun sha256(value: ByteArray) = MessageDigest.getInstance("SHA-256").digest(value)

    // --------------------------------------------------------------------- v2
    /**
     * APK Signature Scheme v2 content digest over the three integrity-protected
     * sections: ZIP entries, central directory and EOCD (with its central
     * directory offset treated as the signing block offset).
     */
    fun contentDigest(sections: List<ByteArray>): ByteArray {
        val chunkDigests = ByteArrayOutputStream()
        var count = 0
        for (section in sections) {
            var offset = 0
            while (offset < section.size) {
                val end = minOf(offset + CHUNK, section.size)
                val piece = section.copyOfRange(offset, end)
                val chunk = ByteArrayOutputStream()
                chunk.write(0xA5)
                writeU32(chunk, piece.size)
                chunk.write(piece)
                chunkDigests.write(sha256(chunk.toByteArray()))
                count++
                offset = end
            }
        }
        val top = ByteArrayOutputStream()
        top.write(0x5A)
        writeU32(top, count)
        top.write(chunkDigests.toByteArray())
        return sha256(top.toByteArray())
    }

    /** `size of block` (u64) + u64-length-prefixed ID-value pairs + size + magic. */
    fun signingBlock(v2Value: ByteArray): ByteArray {
        val pair = ByteArrayOutputStream()
        writeU64(pair, (4 + v2Value.size).toLong())
        writeU32(pair, V2_BLOCK_ID)
        pair.write(v2Value)
        val pairBytes = pair.toByteArray()
        val size = pairBytes.size + 8 + MAGIC.size
        val out = ByteArrayOutputStream()
        writeU64(out, size.toLong())
        out.write(pairBytes)
        writeU64(out, size.toLong())
        out.write(MAGIC)
        return out.toByteArray()
    }

    /**
     * Build the v2 value: a length-prefixed sequence holding one signer.
     * @param sections the three integrity-protected ZIP sections, EOCD patched
     */
    fun v2Value(identity: Identity, sections: List<ByteArray>): ByteArray {
        val digest = contentDigest(sections)
        val digestEntry = ByteArrayOutputStream()
        writeU32(digestEntry, SIGNATURE_RSA_PKCS1_SHA256)
        digestEntry.write(lengthPrefixed(digest))
        val signedData = concat(
            lengthPrefixed(lengthPrefixed(digestEntry.toByteArray())),   // digests
            lengthPrefixed(lengthPrefixed(identity.certificate)),        // certificates
            lengthPrefixed(ByteArray(0))                                 // additional attributes
        )
        val signatureEntry = ByteArrayOutputStream()
        writeU32(signatureEntry, SIGNATURE_RSA_PKCS1_SHA256)
        signatureEntry.write(lengthPrefixed(identity.sign(signedData)))
        val signer = concat(
            lengthPrefixed(signedData),
            lengthPrefixed(lengthPrefixed(signatureEntry.toByteArray())),
            lengthPrefixed(identity.spki)
        )
        return lengthPrefixed(lengthPrefixed(signer))
    }

    /** Offset of the central directory in an EOCD-terminated ZIP. */
    fun centralDirectory(data: ByteArray): IntArray {
        var end = -1
        for (i in data.size - 22 downTo 0) {
            if (data[i] == 0x50.toByte() && data[i + 1] == 0x4B.toByte() &&
                data[i + 2] == 0x05.toByte() && data[i + 3] == 0x06.toByte()
            ) {
                end = i
                break
            }
        }
        if (end < 0 || end + 22 != data.size) throw IllegalStateException("EOCD not found")
        fun u16(at: Int) = (data[at].toInt() and 255) or ((data[at + 1].toInt() and 255) shl 8)
        fun u32(at: Int) = u16(at) or (u16(at + 2) shl 16)
        return intArrayOf(u32(end + 16), u32(end + 12), end, u16(end + 10))
    }
}
