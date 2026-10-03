package dev.radek.conventor

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.file.Files
import java.security.MessageDigest
import java.util.zip.CRC32
import java.util.zip.Deflater
import java.util.zip.ZipFile

/**
 * Covers the on-device conversion pipeline: instruction lowering, the generated
 * ELF, the binary manifest, ZIP/v1/v2 signing, provider mapping and icon repair.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class ConverterTest {

    private fun u32(data: ByteArray, at: Int): Long {
        val bytes = (0 until 4).map { (data[at + it].toInt() and 255).toLong() }
        return bytes[0] or (bytes[1] shl 8) or (bytes[2] shl 16) or (bytes[3] shl 24)
    }

    private fun u16(data: ByteArray, at: Int): Int =
        (data[at].toInt() and 255) or ((data[at + 1].toInt() and 255) shl 8)

    private fun words(data: ByteArray): List<Long> =
        (0 until data.size / 4).map { u32(data, it * 4) }

    private fun sha256(data: ByteArray) = MessageDigest.getInstance("SHA-256").digest(data)

    private fun hex(data: ByteArray) = data.joinToString("") { "%02x".format(it.toInt() and 255) }

    // ------------------------------------------------------------------- IR
    @Test fun preservesArm64LeafBytes() {
        val code = byteArrayOf(0x00, 0x05, 0x80.toByte(), 0x52, 0x00, 0x08, 0x00, 0x11, 0xC0.toByte(), 0x03, 0x5F, 0xD6.toByte())
        val program = Ir.lift(code, "arm64", false)
        assertArrayEquals(code, program.machineCode)
        assertEquals(listOf(Ir.Op.CONST, Ir.Op.ADD, Ir.Op.RETURN), program.instructions.map { it.op })
    }

    @Test fun lowersArmv6ArmAndThumbLeaves() {
        val arm = Ir.lift(byteArrayOf(0x2A, 0x00, 0xA0.toByte(), 0xE3.toByte(), 0x1E, 0xFF.toByte(), 0x2F, 0xE1.toByte()), "armv6", false)
        assertEquals(listOf(0x52800540L, 0xD65F03C0L), words(arm.machineCode))

        val thumb = Ir.lift(byteArrayOf(0x2A, 0x20, 0x70, 0x47), "armv6", true)
        assertEquals(listOf(0x52800540L, 0xD65F03C0L), words(thumb.machineCode))
    }

    @Test fun lowersEveryArm32FlavourThisRepositoryNames() {
        val code = byteArrayOf(0x2A, 0x00, 0xA0.toByte(), 0xE3.toByte(), 0x1E, 0xFF.toByte(), 0x2F, 0xE1.toByte())
        for (architecture in listOf("armv4t", "armv5tej", "armv6", "armv7", "armv7s", "armv8-32", "arm32-unknown")) {
            assertEquals(8, Ir.lift(code, architecture, false).machineCode.size)
        }
    }

    @Test fun emitsRealArm64ForArm32Operations() {
        // MOV R0,R1 ; MVN R0,R1 ; ADD R0,R1,R2 ; SUB R0,R1,R2 ; MUL R0,R1,R2 ; LSL R0,R1,#3 ; RET
        val cases = mapOf(
            0xE1A00001L to 0x2A0103E0L,          // MOV  W0, W1
            0xE1E00001L to 0x2A2103E0L,          // MVN  W0, W1
            0xE0810002L to 0x0B020020L,          // ADD  W0, W1, W2
            0xE0410002L to 0x4B020020L,          // SUB  W0, W1, W2
            0xE0000291L to 0x1B017C40L,          // MUL  W0, Rm=W1, Rs=W2
            0xE1A00181L to 0x531D7020L           // LSL  W0, W1, #3
        )
        for ((instruction, expected) in cases) {
            val bytes = ByteArrayOutputStream()
            bytes.write(byteArrayOf(0x07, 0x10, 0xA0.toByte(), 0xE3.toByte()))     // MOV R1, #7
            bytes.write(byteArrayOf(0x06, 0x20, 0xA0.toByte(), 0xE3.toByte()))     // MOV R2, #6
            bytes.write(byteArrayOf(
                (instruction and 0xFF).toInt().toByte(),
                ((instruction shr 8) and 0xFF).toInt().toByte(),
                ((instruction shr 16) and 0xFF).toInt().toByte(),
                ((instruction shr 24) and 0xFF).toInt().toByte()
            ))
            bytes.write(byteArrayOf(0x1E, 0xFF.toByte(), 0x2F, 0xE1.toByte()))     // BX LR
            val program = Ir.lift(bytes.toByteArray(), "armv6", false)
            assertEquals("0x${instruction.toString(16)}", expected, words(program.machineCode)[2])
        }
    }

    @Test fun lowersThumbRegisterGroupBySixOpBits() {
        for ((instruction, op) in mapOf(0x4000 to Ir.Op.AND, 0x4040 to Ir.Op.EOR, 0x4300 to Ir.Op.ORR,
            0x4340 to Ir.Op.MUL, 0x4380 to Ir.Op.BIC, 0x43C0 to Ir.Op.MVN)) {
            val bytes = byteArrayOf(0x07, 0x20, (instruction and 0xFF).toByte(), ((instruction shr 8) and 0xFF).toByte(), 0x70, 0x47)
            val program = Ir.lift(bytes, "armv6", true)
            assertEquals("0x${instruction.toString(16)}", op, program.instructions[1].op)
        }
    }

    @Test fun rejectsEverythingOutsideTheProvedSubset() {
        val arm = listOf(
            byteArrayOf(0x00, 0x00, 0x51, 0xE3.toByte(), 0x1E, 0xFF.toByte(), 0x2F, 0xE1.toByte()),        // CMP
            byteArrayOf(0x04, 0xD0.toByte(), 0x4D, 0xE2.toByte(), 0x1E, 0xFF.toByte(), 0x2F, 0xE1.toByte()),        // SUB SP (stack)
            byteArrayOf(0x00, 0x00, 0xA0.toByte(), 0x11, 0x1E, 0xFF.toByte(), 0x2F, 0xE1.toByte()),        // conditional MOVNE
            byteArrayOf(0x91.toByte(), 0x00, 0x0D, 0xE0.toByte(), 0x1E, 0xFF.toByte(), 0x2F, 0xE1.toByte())         // MUL SP, R1, R0
        )
        for (code in arm) {
            assertThrows(Ir.Unsupported::class.java) { Ir.lift(code, "armv6", false) }
        }
        val thumb = listOf(
            byteArrayOf(0x07, 0x20, 0x40, 0x41, 0x70, 0x47),                    // ADCS
            byteArrayOf(0x07, 0x20, 0xC0.toByte(), 0x40, 0x70, 0x47),                    // LSRS register
            byteArrayOf(0x07, 0x20, 0x00, 0x48, 0x70, 0x47)                     // LDR [PC] (memory)
        )
        for (code in thumb) {
            assertThrows(Ir.Unsupported::class.java) { Ir.lift(code, "armv6", true) }
        }
        // An ARM64 leaf that never returns is not closed.
        assertThrows(Ir.Unsupported::class.java) {
            Ir.lift(byteArrayOf(0x40, 0x05, 0x80.toByte(), 0x52), "arm64", false)
        }
        // ARM64e is analysed and reconstructed but has no conversion backend.
        assertThrows(Ir.Unsupported::class.java) {
            Ir.lift(byteArrayOf(0x40, 0x05, 0x80.toByte(), 0x52, 0xC0.toByte(), 0x03, 0x5F, 0xD6.toByte()), "arm64e", false)
        }
    }

    @Test fun thumbTwoIsRejectedOnThumbOneOnlyArchitectures() {
        // 0xF240 0x002A is MOVW r0, #42, a Thumb-2 encoding that ARMv6 cannot run.
        val movw = byteArrayOf(0x40, 0xF2.toByte(), 0x2A, 0x00, 0x70, 0x47)
        assertThrows(Ir.Unsupported::class.java) { Ir.lift(movw, "armv6", true) }
        assertThrows(Ir.Unsupported::class.java) { Ir.lift(movw, "armv5tej", true) }
        // The same encoding is proven on ARMv7.
        assertEquals(2, Ir.lift(movw, "armv7", true).instructions.size)
    }

    @Test fun refusesArm64eAndUnknownArchitectures() {
        assertThrows(Ir.Unsupported::class.java) { Ir.lift(ByteArray(4), "arm64e", false) }
        assertThrows(Ir.Unsupported::class.java) { Ir.lift(ByteArray(4), "x86_64", false) }
    }

    // ------------------------------------------------------------------ ELF
    @Test fun elfExportsExactlyTheJniEntry() {
        val code = byteArrayOf(0x40, 0x05, 0x80.toByte(), 0x52, 0xC0.toByte(), 0x03, 0x5F, 0xD6.toByte())
        val buildId = MessageDigest.getInstance("SHA-1").digest(code)
        val image = Elf.build(code, buildId)
        val parsed = ElfExports.parse(image)
        assertEquals(listOf(Elf.ENTRY_SYMBOL), parsed.names)
        assertEquals(code.size.toLong(), parsed.size)
        assertEquals(hex(sha256(code)), parsed.sha256)
        assertTrue(parsed.needed.isEmpty())
        assertEquals(0x7F.toByte(), image[0])
        assertEquals(3, u16(image, 16))                       // ET_DYN
        assertEquals(183, u16(image, 18))                      // EM_AARCH64
        assertEquals(6, u16(image, 56))                        // e_phnum
        assertEquals(8, u16(image, 60))                        // e_shnum
    }

    @Test fun elfRejectsMalformedCode() {
        assertThrows(IllegalArgumentException::class.java) { Elf.build(ByteArray(0), ByteArray(20)) }
        assertThrows(IllegalArgumentException::class.java) { Elf.build(ByteArray(6), ByteArray(20)) }
        assertThrows(IllegalArgumentException::class.java) {
            Elf.build(byteArrayOf(0, 0, 0x80.toByte(), 0x52), ByteArray(19))
        }
    }

    // ----------------------------------------------------------------- AXML
    @Test fun manifestCarriesPackageLabelSdkAndLauncherIntent() {
        val bytes = Axml.manifest("dev.radek.converted.pabc", "Fixture", Converter.ENTRY_ACTIVITY, "0.1", 1, 26, 35)
        assertEquals(0x0003, u16(bytes, 0))                    // RES_XML
        assertEquals(bytes.size.toLong(), u32(bytes, 4))
        assertEquals(0x0001, u16(bytes, 8))                    // RES_STRING_POOL
        val count = u32(bytes, 16).toInt()
        val stringsStart = u32(bytes, 28).toInt()
        val strings = (0 until count).map { index ->
            val at = 8 + stringsStart + u32(bytes, 8 + 28 + index * 4).toInt()
            val length = bytes[at].toInt() and 0x7F
            val byteLength = bytes[at + 1].toInt() and 0x7F
            String(bytes, at + 2, byteLength, Charsets.UTF_8).also { assertEquals(length, it.length) }
        }
        assertTrue(strings.containsAll(listOf(
            "manifest", "application", "activity", "intent-filter", "android",
            Axml.ANDROID_NS, "dev.radek.converted.pabc", "Fixture", Converter.ENTRY_ACTIVITY,
            "android.intent.action.MAIN", "android.intent.category.LAUNCHER",
            "minSdkVersion", "targetSdkVersion", "versionCode", "extractNativeLibs"
        )))
        // The resource map follows the string pool and lists the framework ids of
        // the leading attribute-name strings.
        val at = 8 + u32(bytes, 12).toInt()
        assertEquals(0x0180, u16(bytes, at))
        val mapCount = (u32(bytes, at + 4) - 8) / 4
        val ids = (0 until mapCount.toInt()).map { u32(bytes, at + 8 + it.toInt() * 4) }
        assertTrue(ids.contains(0x0101021BL))                  // android:versionCode
        assertTrue(ids.contains(0x0101020CL))                  // android:minSdkVersion
        assertTrue(ids.contains(0x010104EAL))                  // android:extractNativeLibs
    }

    // ------------------------------------------------------------------ ZIP
    @Test fun zipWriterRoundTripsThroughJavaZipFile() {
        val root = Files.createTempDirectory("radek-zip").toFile()
        try {
            val writer = ZipWriter()
            val entries = listOf(
                "AndroidManifest.xml" to "manifest".toByteArray(),
                "lib/arm64-v8a/libconverted.so" to ByteArray(4096) { (it % 251).toByte() },
                "assets/conversion.json" to "{\"contract\":\"closed-integer-entry-v1\"}".toByteArray()
            )
            entries.forEach { (name, data) -> writer.add(name, data) }
            val file = File(root, "roundtrip.apk")
            file.writeBytes(writer.finish())
            ZipFile(file).use { zip ->
                for ((name, data) in entries) {
                    val entry = zip.getEntry(name)
                    assertNotNull("missing $name", entry)
                    assertArrayEquals(data, zip.getInputStream(entry!!).readBytes())
                }
                assertEquals(entries.size, zip.entries().toList().size)
            }
            val digests = writer.entries.map { it.name to hex(it.digest) }
            assertEquals(hex(sha256(entries[0].second)), digests[0].second)
        } finally {
            root.deleteRecursively()
        }
    }

    // --------------------------------------------------------------- signing
    @Test fun buildsSignedApkThatPassesStructuralVerification() {
        val root = Files.createTempDirectory("radek-sign").toFile()
        try {
            val identity = ApkSign.identity(File(root, "keys"))
            // The identity is persisted, so a second load returns the same key/certificate.
            assertArrayEquals(identity.certificate, ApkSign.identity(File(root, "keys")).certificate)

            val code = byteArrayOf(0x40, 0x05, 0x80.toByte(), 0x52, 0xC0.toByte(), 0x03, 0x5F, 0xD6.toByte())
            val library = Elf.build(code, MessageDigest.getInstance("SHA-1").digest(code))
            val report = JSONObject().put("outputBytes", code.size)
                .put("machineCodeSha256", hex(sha256(code)))
            val pkg = Converter.packageName("0".repeat(64))
            val manifest = Axml.manifest(pkg, "Fixture", Converter.ENTRY_ACTIVITY, "0.1", 1, 26, 35)
            val conversion = JSONObject().put("package", pkg).put("contract", Converter.CONTRACT)
                .put("conversion", report).put("source", JSONObject().put("sha256", "0".repeat(64)))
            val entries = listOf(
                "AndroidManifest.xml" to manifest,
                "classes.dex" to byteArrayOf(0x64, 0x65, 0x78, 0x0A, 0x30, 0x33, 0x35, 0x00),
                "lib/arm64-v8a/libconverted.so" to library,
                "assets/conversion.json" to conversion.toString(2).toByteArray()
            )
            val apk = ApkBuilder.build(entries, File(root, "keys"))
            val file = File(root, "generated.apk")
            file.writeBytes(apk)

            Converter.verifyStructure(file, pkg, code, report, library)

            ZipFile(file).use { zip ->
                assertNotNull(zip.getEntry("META-INF/MANIFEST.MF"))
                assertNotNull(zip.getEntry("META-INF/RADEK.SF"))
                assertNotNull(zip.getEntry("META-INF/RADEK.RSA"))
                val manifestText = zip.getInputStream(zip.getEntry("META-INF/MANIFEST.MF")!!).readBytes()
                    .toString(Charsets.UTF_8)
                assertTrue(manifestText.startsWith("Manifest-Version: 1.0"))
                assertTrue(manifestText.contains("Name: AndroidManifest.xml"))
                assertTrue(manifestText.contains("SHA-256-Digest: " +
                    java.util.Base64.getEncoder().encodeToString(sha256(manifest))))
                val signature = zip.getInputStream(zip.getEntry("META-INF/RADEK.SF")!!).readBytes()
                    .toString(Charsets.UTF_8)
                // Rollback protection: a v1+v2 APK must advertise scheme 2.
                assertTrue(signature.contains("X-Android-APK-Signed: 2"))
                assertTrue(signature.contains("SHA-256-Digest-Manifest: " +
                    java.util.Base64.getEncoder().encodeToString(sha256(manifestText.toByteArray()))))
                for ((name, data) in entries) {
                    val entry = zip.getEntry(name)
                    assertNotNull("missing $name", entry)
                    assertArrayEquals(data, zip.getInputStream(entry!!).readBytes())
                }
            }

            // The signing block sits immediately before the central directory.
            val directory = ApkSign.centralDirectory(apk)
            val magic = "APK Sig Block 42".toByteArray(Charsets.US_ASCII)
            assertArrayEquals(magic, apk.copyOfRange(directory[0] - 16, directory[0]))
            assertTrue(directory[0] > 100)
        } finally {
            root.deleteRecursively()
        }
    }

    @Test fun contentDigestIsChunkedLikeThePlatformVerifier() {
        val one = ApkSign.contentDigest(listOf(ByteArray(1)))
        val two = ApkSign.contentDigest(listOf(ByteArray(1) { 1 }))
        assertNotEquals(hex(one), hex(two))
        assertEquals(32, one.size)
        // Splitting a section into chunks must change the digest (chunks are hashed separately).
        val whole = ApkSign.contentDigest(listOf(ByteArray(3)))
        val split = ApkSign.contentDigest(listOf(ByteArray(1), ByteArray(2)))
        assertNotEquals(hex(whole), hex(split))
    }

    // ------------------------------------------------------------- providers
    @Test fun everyReportedFrameworkMapsToARealAndroidProvider() {
        val reported = listOf(
            "/System/Library/Frameworks/Foundation.framework/Foundation",
            "/System/Library/Frameworks/OpenGLES.framework/OpenGLES",
            "/System/Library/Frameworks/QuartzCore.framework/QuartzCore",
            "/System/Library/Frameworks/AVFoundation.framework/AVFoundation",
            "/System/Library/Frameworks/MediaPlayer.framework/MediaPlayer",
            "/System/Library/Frameworks/CoreAudio.framework/CoreAudio",
            "/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox",
            "/System/Library/Frameworks/UIKit.framework/UIKit",
            "/System/Library/Frameworks/OpenAL.framework/OpenAL",
            "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
            "/System/Library/Frameworks/CFNetwork.framework/CFNetwork",
            "/System/Library/Frameworks/CoreMedia.framework/CoreMedia",
            "/System/Library/Frameworks/CoreVideo.framework/CoreVideo",
            "/usr/lib/libiconv.2.dylib"
        )
        for (installName in reported) {
            val edge = Providers.classify(installName)
            assertNotEquals("no provider for $installName", Providers.STATUS_BLOCKED, edge.optString("status"))
            assertTrue(edge.optString("provider").isNotBlank())
            assertTrue(edge.optString("reason").length > 20)
            assertFalse(Providers.describe(installName).startsWith("BLOCKED"))
        }
    }

    @Test fun openGlesAndIconvBindToTheIdenticalAndroidAbi() {
        val gles = Providers.classify("/System/Library/Frameworks/OpenGLES.framework/OpenGLES")
        assertEquals(Providers.STATUS_PROVIDED, gles.optString("status"))
        assertEquals(Providers.KIND_LIBRARY, gles.optString("kind"))
        assertTrue(gles.optString("provider").contains("libGLESv2.so"))
        val iconv = Providers.classify("/usr/lib/libiconv.2.dylib")
        assertEquals(Providers.STATUS_PROVIDED, iconv.optString("status"))
        assertTrue(iconv.optString("provider").contains("iconv"))
    }

    @Test fun apisWithoutAnAndroidContractStayBlocked() {
        for (name in listOf("/System/Library/Frameworks/StoreKit.framework/StoreKit",
            "/System/Library/Frameworks/GameKit.framework/GameKit",
            "/System/Library/Frameworks/AdSupport.framework/AdSupport")) {
            assertEquals(Providers.STATUS_BLOCKED, Providers.classify(name).optString("status"))
        }
        assertEquals(Providers.STATUS_BLOCKED, Providers.classify("/usr/lib/libDoesNotExist.dylib").optString("status"))
    }

    @Test fun coverageIsHonestAboutWhatIsMapped() {
        val provided = JSONArray().put(JSONObject().put("path", "/System/Library/Frameworks/OpenGLES.framework/OpenGLES"))
            .put(JSONObject().put("path", "/usr/lib/libiconv.2.dylib"))
        val symbols = JSONArray().put(JSONObject().put("name", "_glClear")).put(JSONObject().put("name", "_iconv_open"))
        assertEquals(100, Providers.coverage(provided, symbols))
        assertEquals(100, Providers.coverage(JSONArray(), JSONArray()))

        val partial = JSONArray().put(JSONObject().put("path", "/System/Library/Frameworks/StoreKit.framework/StoreKit"))
        assertTrue(Providers.coverage(partial, JSONArray()) < 100)
        val unmapped = JSONArray().put(JSONObject().put("name", "_SomeVendorOnlySymbol"))
        assertTrue(Providers.coverage(JSONArray(), unmapped) < 100)
    }

    @Test fun nativeLibrariesUsedByGeneratedImagesAreAllowed() {
        assertTrue(Providers.NATIVE_LIBRARIES.containsAll(listOf("libGLESv2.so", "libEGL.so", "libaaudio.so", "libc.so")))
    }

    // -------------------------------------------------------------- SafeZip
    @Test fun lenientNamesAreSanitisedButTraversalStillThrows() {
        assertEquals("a/b", SafeZip.memberName("a\\b"))
        assertEquals("C_/drive", SafeZip.memberName("C:/drive"))
        assertEquals("a/b", SafeZip.memberName("a//b"))
        assertEquals("absolute", SafeZip.memberName("/absolute"))
        assertEquals("a/b", SafeZip.memberName("./a/./b"))
        for (name in listOf("../escape", "a/../b", "", "a\u0000b")) {
            assertThrows(IllegalArgumentException::class.java) { SafeZip.memberName(name) }
        }
        // The strict form is unchanged: it still rejects everything above.
        for (name in listOf("../escape", "/absolute", "a/../b", "a\\b", "a//b", "C:/drive")) {
            assertThrows(IllegalArgumentException::class.java) { SafeZip.validateName(name) }
        }
    }

    @Test fun extractionAcceptsOddButSafeMemberNames() {
        val root = Files.createTempDirectory("radek-lenient").toFile()
        try {
            val archive = File(root, "odd.ipa")
            java.util.zip.ZipOutputStream(archive.outputStream()).use { zip ->
                for ((name, value) in listOf("Payload/App.app/Info.plist" to "x".toByteArray(),
                    "Payload/App.app/Art\\Work/icon.png" to byteArrayOf(1, 2))) {
                    zip.putNextEntry(java.util.zip.ZipEntry(name))
                    zip.write(value)
                    zip.closeEntry()
                }
            }
            SafeZip.extract(archive, File(root, "out"))
            assertTrue(File(root, "out/Payload/App.app/Info.plist").isFile)
            assertTrue(File(root, "out/Payload/App.app/Art/Work/icon.png").isFile)
        } finally {
            root.deleteRecursively()
        }
    }

    // ---------------------------------------------------------------- icons
    @Test fun decodesAppleCgbiPngPayloads() {
        // CgBI stores premultiplied BGRA: (B=16, G=32, R=64, A=128) becomes
        // straight-alpha RGBA (R=128, G=64, B=32, A=128).
        val scanlines = byteArrayOf(0, 16, 32, 64, 128.toByte())
        val cgbi = png(1, 1, scanlines, cgbi = true)
        assertTrue(Icons.isCgbi(cgbi))
        val bitmap = Icons.decode(cgbi)
        assertNotNull("CgBI PNG must decode", bitmap)
        val pixel = bitmap!!.getPixel(0, 0)
        assertEquals(128, android.graphics.Color.alpha(pixel))
        assertEquals(128, android.graphics.Color.red(pixel))
        assertEquals(64, android.graphics.Color.green(pixel))
        assertEquals(32, android.graphics.Color.blue(pixel))
        bitmap.recycle()
        // A standard PNG is decoded by BitmapFactory and is not reported as CgBI.
        val standard = png(1, 1, scanlines, cgbi = false)
        assertFalse(Icons.isCgbi(standard))
        assertNotNull(Icons.decode(standard))
        assertNull(Icons.decode(byteArrayOf(1, 2, 3)))
    }

    @Test fun generatedIconsAreVisibleAndDeterministic() {
        val first = Icons.generate("My Game")
        val second = Icons.generate("My Game")
        assertTrue(Icons.isOpaque(first))
        assertEquals(first.getPixel(20, 20), second.getPixel(20, 20))
        assertFalse(Icons.isOpaque(android.graphics.Bitmap.createBitmap(
            16, 16, android.graphics.Bitmap.Config.ARGB_8888)))
        first.recycle()
        second.recycle()
    }

    @Test fun extractsPngPayloadsFromACompiledAssetCatalog() {
        val root = Files.createTempDirectory("radek-car").toFile()
        try {
            val payload = png(2, 2, ByteArray(16), cgbi = false)
            val catalog = File(root, "Assets.car")
            val out = ByteArrayOutputStream()
            out.write("BOMStore".toByteArray(Charsets.US_ASCII))
            out.write(ByteArray(64))
            out.write("ISTC".toByteArray(Charsets.US_ASCII))
            out.write(ByteArray(180))
            out.write(payload)
            out.write(ByteArray(32))
            out.write(payload)
            catalog.writeBytes(out.toByteArray())
            val found = Icons.catalogPayloads(catalog)
            assertEquals(2, found.size)
            assertArrayEquals(payload, found[0])
            // A non-catalog file yields nothing rather than garbage.
            assertTrue(Icons.catalogPayloads(File(root, "missing.car")).isEmpty())
            catalog.writeBytes("NotABOM".toByteArray())
            assertTrue(Icons.catalogPayloads(catalog).isEmpty())
        } finally {
            root.deleteRecursively()
        }
    }

    /** CgBI IDAT streams are raw DEFLATE; standard PNGs use zlib. */
    private fun rawDeflate(data: ByteArray): ByteArray {
        val deflater = Deflater(9, true)
        try {
            deflater.setInput(data)
            deflater.finish()
            val out = ByteArrayOutputStream()
            val buffer = ByteArray(4096)
            while (!deflater.finished()) out.write(buffer, 0, deflater.deflate(buffer))
            return out.toByteArray()
        } finally {
            deflater.end()
        }
    }

    private fun png(width: Int, height: Int, scanlines: ByteArray, cgbi: Boolean): ByteArray {
        val out = ByteArrayOutputStream()
        fun chunk(type: String, payload: ByteArray) {
            val name = type.toByteArray(Charsets.US_ASCII)
            out.write((payload.size ushr 24) and 0xFF); out.write((payload.size ushr 16) and 0xFF)
            out.write((payload.size ushr 8) and 0xFF); out.write(payload.size and 0xFF)
            out.write(name)
            out.write(payload)
            val crc = CRC32()
            crc.update(name); crc.update(payload)
            val value = crc.value.toInt()
            out.write((value ushr 24) and 0xFF); out.write((value ushr 16) and 0xFF)
            out.write((value ushr 8) and 0xFF); out.write(value and 0xFF)
        }
        out.write(byteArrayOf(0x89.toByte(), 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A))
        val header = ByteArrayOutputStream()
        header.write((width ushr 24) and 0xFF); header.write((width ushr 16) and 0xFF)
        header.write((width ushr 8) and 0xFF); header.write(width and 0xFF)
        header.write((height ushr 24) and 0xFF); header.write((height ushr 16) and 0xFF)
        header.write((height ushr 8) and 0xFF); header.write(height and 0xFF)
        header.write(byteArrayOf(8, 6, 0, 0, 0))                // 8-bit RGBA
        chunk("IHDR", header.toByteArray())
        if (cgbi) chunk("CgBI", byteArrayOf(0, 0, 0, 1))
        chunk("IDAT", if (cgbi) rawDeflate(scanlines) else Deflater(6).let { deflater ->
            deflater.setInput(scanlines); deflater.finish()
            val buffer = ByteArrayOutputStream(); val chunkBuffer = ByteArray(4096)
            while (!deflater.finished()) buffer.write(chunkBuffer, 0, deflater.deflate(chunkBuffer))
            deflater.end(); buffer.toByteArray()
        })
        chunk("IEND", ByteArray(0))
        return out.toByteArray()
    }

    // -------------------------------------------------------- provenance/etc
    @Test fun packageIdentityIsDerivedFromTheSourceHash() {
        val hash = "a".repeat(64)
        assertEquals("dev.radek.converted.p" + "a".repeat(20), Converter.packageName(hash))
    }

    @Test fun neutralEntryReturnsFortyTwoWhenForced() {
        val program = Ir.lift(byteArrayOf(0x40, 0x05, 0x80.toByte(), 0x52, 0xC0.toByte(), 0x03, 0x5F, 0xD6.toByte()), "arm64", false)
        assertEquals(2, program.instructions.size)
        assertEquals(Ir.Op.RETURN, program.instructions[1].op)
    }
}
