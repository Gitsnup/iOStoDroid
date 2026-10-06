package dev.radek.conventor

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.ByteBuffer
import java.util.zip.CRC32
import java.util.zip.Deflater
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

private fun be32(value: Int) = ByteBuffer.allocate(4).order(java.nio.ByteOrder.BIG_ENDIAN).putInt(value).array()
private fun le16(value: Int) = ByteBuffer.allocate(2).order(java.nio.ByteOrder.LITTLE_ENDIAN).putShort(value.toShort()).array()
private fun le32(value: Int) = ByteBuffer.allocate(4).order(java.nio.ByteOrder.LITTLE_ENDIAN).putInt(value).array()

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class IconDecoderTest {
    private fun chunk(name: String, payload: ByteArray): ByteArray {
        val type = name.toByteArray(Charsets.US_ASCII)
        val output = ByteArrayOutputStream()
        output.write(ByteBuffer.allocate(4).putInt(payload.size).array())
        output.write(type)
        output.write(payload)
        val crc = CRC32().apply { update(type); update(payload) }.value.toInt()
        output.write(ByteBuffer.allocate(4).putInt(crc).array())
        return output.toByteArray()
    }

    private fun cgbiPng(): ByteArray {
        val signature = byteArrayOf(0x89.toByte(), 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a)
        val header = ByteBuffer.allocate(13).putInt(1).putInt(1).put(8.toByte()).put(6.toByte())
            .put(0.toByte()).put(0.toByte()).put(0.toByte()).array()
        // CgBI stores premultiplied BGRA: (B=16, G=32, R=64, A=128)
        // decodes to straight-alpha RGBA (R=128, G=64, B=32, A=128).
        val deflater = Deflater(9, true)
        val compressed = try {
            deflater.setInput(byteArrayOf(0.toByte(), 16.toByte(), 32.toByte(), 64.toByte(), 128.toByte()))
            deflater.finish()
            val bytes = ByteArray(64)
            val count = deflater.deflate(bytes)
            bytes.copyOf(count)
        } finally {
            deflater.end()
        }
        return signature + chunk("CgBI", byteArrayOf(2, 0, 0, 0)) + chunk("IHDR", header) +
            chunk("IDAT", compressed) + chunk("IEND", byteArrayOf())
    }

    private fun platformPng(color: Int, width: Int = 8, height: Int = 8): ByteArray {
        val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        bitmap.eraseColor(color)
        return try {
            ByteArrayOutputStream().use { output ->
                assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, output))
                output.toByteArray()
            }
        } finally {
            bitmap.recycle()
        }
    }

    private class CarBuilder {
        private val blocks = mutableListOf(byteArrayOf())
        private val variables = mutableListOf<Pair<String, Int>>()

        fun add(value: ByteArray): Int { blocks += value; return blocks.lastIndex }
        fun variable(name: String, index: Int) { variables += name to index }

        fun tree(entries: List<Pair<ByteArray, ByteArray>>): Int {
            val leaf = ByteArrayOutputStream().apply {
                write(ByteBuffer.allocate(12).order(java.nio.ByteOrder.BIG_ENDIAN)
                    .putShort(1).putShort(entries.size.toShort()).putInt(0).putInt(0).array())
                entries.forEach { (key, value) ->
                    val keyIndex = this@CarBuilder.add(key)
                    val valueIndex = this@CarBuilder.add(value)
                    write(be32(keyIndex)); write(be32(valueIndex))
                }
            }.toByteArray()
            val leafIndex = add(leaf)
            return add("tree".toByteArray() + ByteBuffer.allocate(16).order(java.nio.ByteOrder.BIG_ENDIAN)
                .putInt(1).putInt(leafIndex).putInt(4096).putInt(entries.size).array() + byteArrayOf(0))
        }

        fun build(): ByteArray {
            val indexSize = 4 + 8 * blocks.size
            val variableSize = 4 + variables.sumOf { 5 + it.first.toByteArray().size }
            val dataStart = 32 + indexSize + variableSize
            val offsets = mutableListOf<Pair<Int, Int>>()
            val payload = ByteArrayOutputStream()
            blocks.forEach { block ->
                val offset = if (block.isEmpty()) 0 else dataStart + payload.size()
                offsets.add(offset to block.size)
                payload.write(block)
                while (payload.size() % 4 != 0) payload.write(0)
            }
            val output = ByteArrayOutputStream()
            output.write("BOMStore".toByteArray())
            output.write(ByteBuffer.allocate(24).order(java.nio.ByteOrder.BIG_ENDIAN)
                .putInt(1).putInt(blocks.size).putInt(32).putInt(indexSize).putInt(32 + indexSize).putInt(variableSize).array())
            output.write(be32(blocks.size))
            offsets.forEach { (offset, length) -> output.write(be32(offset)); output.write(be32(length)) }
            output.write(be32(variables.size))
            variables.forEach { (name, index) ->
                val bytes = name.toByteArray()
                output.write(be32(index)); output.write(bytes.size); output.write(bytes)
            }
            output.write(payload.toByteArray())
            return output.toByteArray()
        }
    }

    private fun assetCatalog(
        iconPng: ByteArray,
        backgroundPng: ByteArray,
        additionalIconPng: ByteArray? = null,
    ): ByteArray {
        val builder = CarBuilder()
        fun rendition(asset: String, id: Int, png: ByteArray, scale: Int = 3): Pair<ByteArray, ByteArray> {
            val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            BitmapFactory.decodeByteArray(png, 0, png.size, bounds)
            val width = bounds.outWidth.coerceAtLeast(1)
            val height = bounds.outHeight.coerceAtLeast(1)
            val key = le16(0x55) + le16(0xb5) + le16(scale) + le16(id)
            val header = ByteArray(184)
            "ISTC".toByteArray().copyInto(header)
            ByteBuffer.wrap(header).order(java.nio.ByteOrder.LITTLE_ENDIAN).apply {
                position(4); putInt(1); putInt(0); putInt(width); putInt(height); putInt(scale * 100); putInt(0x47425241); putInt(1)
                position(36); putShort(0)
                position(40); put(asset.toByteArray().copyOf(127))
                position(168); putInt(0); putInt(0); putInt(0); putInt(png.size)
            }
            return key to (header + png)
        }
        val renditions = mutableListOf(
            rendition("AppIcon@3x.png", 0x8019, iconPng, 3),
            rendition("Background.png", 0x1234, backgroundPng, 3),
        )
        additionalIconPng?.let { renditions += rendition("AppIcon@2x.png", 0x8019, it, 2) }
        val renditionTree = builder.tree(renditions)
        fun facet(id: Int) = le16(0) + le16(0) + le16(3) + le16(1) + le16(0x55) +
            le16(2) + le16(0xb5) + le16(16) + le16(id)
        val facetTree = builder.tree(listOf("AppIcon".toByteArray() to facet(0x8019), "Background".toByteArray() to facet(0x1234)))
        val carHeader = ByteArray(436); "CTAR".toByteArray().copyInto(carHeader)
        ByteBuffer.wrap(carHeader).order(java.nio.ByteOrder.LITTLE_ENDIAN).apply {
            position(4); putInt(804); putInt(17); putInt(1700000000); putInt(2); putInt(0)
        }
        builder.variable("CARHEADER", builder.add(carHeader))
        builder.variable("KEYFORMAT", builder.add("kfmt".toByteArray() + le32(1) + le32(4) + le32(1) + le32(2) + le32(11) + le32(16)))
        builder.variable("RENDITIONS", renditionTree)
        builder.variable("FACETKEYS", facetTree)
        return builder.build()
    }


    @Test fun decodesCgbiRawDeflateAndRestoresStraightAlpha() {
        val bitmap = IconDecoder.decodeCgbi(cgbiPng(), 512)
        try {
            assertEquals(1, bitmap.width)
            assertEquals(Color.argb(128, 128, 64, 32), bitmap.getPixel(0, 0))
        } finally {
            bitmap.recycle()
        }
    }

    @Test fun declaredScaledAppIconWinsOverLargerLooseTexture() {
        val root = createTempDir(prefix = "radek-icon-priority-test")
        try {
            val app = File(root, "Fixture.app").apply { mkdirs() }
            File(app, "AppIcon.png").writeBytes(byteArrayOf(1, 2, 3))
            File(app, "AppIcon@3x.png").writeBytes(platformPng(Color.MAGENTA, 8, 8))
            File(app, "AppIcon@2x.png").writeBytes(platformPng(Color.MAGENTA, 16, 16))
            File(app, "Background.png").writeBytes(platformPng(Color.BLACK, 128, 128))
            val output = File(root, "result").apply { mkdirs() }

            val result = extractIcon(app, listOf("AppIcon.png"), output)

            assertEquals("SUPPORTED", result.getString("status"))
            assertEquals("AppIcon@2x.png", result.getString("source"))
            assertEquals(16, result.getInt("width"))
            assertEquals(2.0, result.getDouble("scale"), 0.0)
        } finally {
            root.deleteRecursively()
        }
    }

    @Test fun compiledAssetCatalogProvidesTheGameIcon() {
        val root = createTempDir(prefix = "radek-car-icon-test")
        try {
            val app = File(root, "Fixture.app").apply { mkdirs() }
            File(app, "Assets.car").writeBytes(assetCatalog(platformPng(Color.MAGENTA), platformPng(Color.BLACK)))
            val output = File(root, "result").apply { mkdirs() }

            val result = extractIcon(app, listOf("AppIcon"), output)

            assertEquals("SUPPORTED", result.getString("status"))
            assertEquals("assets.car", result.getString("kind"))
            assertTrue(result.getString("source").contains("AppIcon"))
            assertTrue(result.getJSONArray("attempts").length() >= 1)
            val saved = BitmapFactory.decodeFile(File(output, "icon.png").path)
            assertNotNull(saved)
            assertEquals(Color.MAGENTA, saved!!.getPixel(0, 0))
            saved.recycle()
        } finally {
            root.deleteRecursively()
        }
    }

    @Test fun originalPixelDimensionsBreakTiesAfterDecodeDownsampling() {
        val root = createTempDir(prefix = "radek-original-resolution-test")
        try {
            val app = File(root, "Fixture.app").apply { mkdirs() }
            val threeX = File(app, "AppIcon@3x.png").apply {
                writeBytes(platformPng(Color.BLUE, 512, 512))
            }
            val twoX = File(app, "AppIcon@2x.png").apply {
                writeBytes(platformPng(Color.MAGENTA, 1024, 1024))
            }
            assertEquals(512 to 512, IconDecoder.dimensions(threeX))
            assertEquals(1024 to 1024, IconDecoder.dimensions(twoX))
            val output = File(root, "result").apply { mkdirs() }

            val result = extractIcon(app, listOf("AppIcon"), output)

            assertEquals("SUPPORTED", result.getString("status"))
            assertEquals("selection diagnostics: $result", "AppIcon@2x.png", result.getString("source"))
            // extractIcon exposes source dimensions as width/height and the
            // sampled bitmap size separately as decodedWidth/decodedHeight.
            assertEquals(1024, result.getInt("width"))
            assertEquals(1024, result.getInt("height"))
            assertEquals(512, result.getInt("decodedWidth"))
            assertEquals(512, result.getInt("decodedHeight"))
        } finally {
            root.deleteRecursively()
        }
    }

    @Test fun largerCatalogRenditionBeatsHigherScaleAndSmallerDeclaredFile() {
        val root = createTempDir(prefix = "radek-car-resolution-test")
        try {
            val app = File(root, "Fixture.app").apply { mkdirs() }
            File(app, "AppIcon@3x.png").writeBytes(platformPng(Color.BLUE, 8, 8))
            File(app, "Assets.car").writeBytes(
                assetCatalog(
                    platformPng(Color.MAGENTA, 8, 8),
                    platformPng(Color.BLACK, 128, 128),
                    platformPng(Color.MAGENTA, 16, 16),
                ),
            )
            val output = File(root, "result").apply { mkdirs() }

            val result = extractIcon(app, listOf("AppIcon"), output)

            assertEquals("SUPPORTED", result.getString("status"))
            assertEquals("assets.car", result.getString("kind"))
            assertTrue(result.getString("source").contains("AppIcon"))
            assertEquals(16, result.getInt("width"))
            assertEquals(2.0, result.getDouble("scale"), 0.0)
            val saved = BitmapFactory.decodeFile(File(output, "icon.png").path)
            assertNotNull(saved)
            assertEquals(Color.MAGENTA, saved!!.getPixel(0, 0))
            saved.recycle()
        } finally {
            root.deleteRecursively()
        }
    }

    @Test fun declaredBrokenIconFallsBackToAnotherBundleImage() {
        val root = java.nio.file.Files.createTempDirectory("radek-icon-test").toFile()
        try {
            val app = File(root, "Fixture.app").apply { mkdirs() }
            File(app, "DeclaredIcon.png").writeBytes(byteArrayOf(1, 2, 3, 4))
            File(app, "GameLogo.png").writeBytes(platformPng(Color.MAGENTA))
            val output = File(root, "result").apply { mkdirs() }

            // The bundle icon pipeline: declared names first, then ranked loose
            // images, then Assets.car payloads, then a generated icon.
            val result = Icons.recover(app, listOf("DeclaredIcon"), output, "Fixture", org.json.JSONArray())

            assertEquals("SUPPORTED", result.getString("status"))
            assertTrue("unexpected icon source: ${result.getString("source")}", result.getString("source").endsWith("GameLogo.png"))
            assertTrue(result.getJSONArray("attempts").length() >= 2)
            val saved = BitmapFactory.decodeFile(File(output, "icon.png").path)
            assertNotNull(saved)
            assertEquals(Color.MAGENTA, saved!!.getPixel(0, 0))
            saved.recycle()
        } finally {
            root.deleteRecursively()
        }
    }
}
