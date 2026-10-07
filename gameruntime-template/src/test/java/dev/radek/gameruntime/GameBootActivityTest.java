package dev.radek.gameruntime;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.zip.CRC32;
import java.util.zip.Deflater;

public final class GameBootActivityTest {
    @Test
    public void parsesBootReportAndStatesFirstUnimplementedImport() throws Exception {
        JSONObject manifest = new JSONObject()
                .put("executable", new JSONObject()
                        .put("name", "AngryBirds")
                        .put("sha256", "deadbeef"))
                .put("resources", new JSONObject()
                        .put("files", 259)
                        .put("bytes", 13231926L));

        JSONObject report = new JSONObject()
                .put("status", "not_runnable")
                .put("execution", new JSONObject()
                        .put("stopCategory", "IMPORT_CALLOUT_UNIMPLEMENTED")
                        .put("stopReason", "First unimplemented import callout: _UIApplicationMain")
                        .put("executedInstructions", 412L)
                        .put("lastProgramCounter", 0xf0000010L))
                .put("loader", new JSONObject()
                        .put("status", "LOADED_WITH_TRAPS")
                        .put("mappedSegments", 3)
                        .put("boundImports", 42)
                        .put("trappedImports", 212))
                .put("trappedImports", new JSONArray()
                        .put(new JSONObject()
                                .put("symbol", "_UIApplicationMain")
                                .put("library", "UIKit")
                                .put("lastCaller", 0x00002d94L)));

        GameBootActivity.BootSummary summary = GameBootActivity.summarizeBootReport(
                "Angry Birds",
                manifest,
                report.toString());

        assertEquals("Angry Birds", summary.headline);
        assertTrue(summary.failedClosed);
        assertTrue(summary.status.contains("IMPORT_CALLOUT_UNIMPLEMENTED"));
        assertTrue(summary.status.contains("_UIApplicationMain"));
        assertTrue(summary.details.contains("Splash viewport: active"));
        assertTrue(summary.details.contains("Instructions executed: 412"));
        assertTrue(summary.details.contains("Trapped import: _UIApplicationMain (UIKit) from lr=0x00002d94"));
        assertTrue(summary.details.contains("No gameplay or conversion is claimed by this artifact."));
    }

    @Test
    public void reportsCleanFailureWhenJsonIsMalformed() {
        GameBootActivity.BootSummary summary = GameBootActivity.summarizeBootReport(
                "Angry Birds",
                new JSONObject(),
                "{not-json");
        assertTrue(summary.failedClosed);
        assertTrue(summary.status.startsWith("BOOT FAILED CLOSED: malformed native report"));
        assertTrue(summary.details.contains("{not-json"));
    }

    @Test
    public void reportsNativeLoadFailureWithoutCrashing() {
        String text = GameBootActivity.summarizeNativeError(
                new UnsatisfiedLinkError("dlopen failed: library \"libcompat_runtime_v1.so\" not found"));
        assertTrue(text.contains("UnsatisfiedLinkError"));
        assertTrue(text.contains("libcompat_runtime_v1.so"));
    }

    @Test
    public void extractsBoundedPayloadsAndRefusesPathTraversal() throws Exception {
        assertEquals("data/SPLASHES.png", GameBootActivity.sanitizeRelativePath("data/SPLASHES.png"));
        assertEquals("levels/level1.lua", GameBootActivity.sanitizeRelativePath("levels\\level1.lua"));
        try {
            GameBootActivity.sanitizeRelativePath("../outside.txt");
            fail("Expected traversal path to be rejected");
        } catch (IOException expected) {
            assertTrue(expected.getMessage().contains("traversal"));
        }
        try {
            GameBootActivity.sanitizeRelativePath("/etc/passwd");
            fail("Expected absolute path to be rejected");
        } catch (IOException expected) {
            assertTrue(expected.getMessage().contains("unsafe"));
        }
    }

    @Test
    public void parsesAngryBirdsSplashSheetDescriptor() throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        writeU16String(out, "SPLASHES.png");
        writeU16Be(out, 3);
        writeSpriteEntry(out, "SPLASH_ROVIO", 1, 321, 121, 190, 60, 95);
        writeSpriteEntry(out, "SPLASH_ANGRY_BIRDS", 1, 511, 480, 320, 240, 160);
        writeSpriteEntry(out, "SPLASH_CLICKGAMER", 1, 1, 480, 320, 240, 160);

        GameBootActivity.SplashSheetDescriptor descriptor =
                GameBootActivity.parseSplashSheetDescriptor(out.toByteArray());
        assertNotNull(descriptor);
        assertEquals("SPLASHES.png", descriptor.sheetName);
        assertEquals(3, descriptor.entries.size());
        assertEquals("SPLASH_ANGRY_BIRDS", descriptor.entries.get(1).name);
        assertEquals(480, descriptor.entries.get(1).width);
        assertEquals(320, descriptor.entries.get(1).height);
        assertNull(GameBootActivity.parseSplashSheetDescriptor(new byte[] { 0x00, 0x01 }));
    }

    @Test
    public void decodesAppleCgbiPngWithBgraPremultipliedSamples() throws Exception {
        byte[] cgbiPng = buildSinglePixelCgbiPng(40, 80, 200, 255);
        assertTrue(GameBootActivity.isCgbiPng(cgbiPng));
        assertFalse(GameBootActivity.isCgbiPng(new byte[] { 1, 2, 3, 4 }));

        int[] dims = new int[2];
        int[] pixels = GameBootActivity.decodeCgbiRgbaPixels(cgbiPng, dims);
        assertEquals(1, dims[0]);
        assertEquals(1, dims[1]);
        assertEquals(1, pixels.length);
        int argb = pixels[0];
        assertEquals(255, (argb >>> 24) & 0xff);
        assertEquals(200, (argb >>> 16) & 0xff);
        assertEquals(80, (argb >>> 8) & 0xff);
        assertEquals(40, argb & 0xff);
    }

    private static void writeU16Be(ByteArrayOutputStream out, int value) {
        out.write((value >>> 8) & 0xff);
        out.write(value & 0xff);
    }

    private static void writeU16String(ByteArrayOutputStream out, String value) throws IOException {
        byte[] ascii = value.getBytes(StandardCharsets.US_ASCII);
        writeU16Be(out, ascii.length);
        out.write(ascii);
    }

    private static void writeSpriteEntry(
            ByteArrayOutputStream out,
            String name,
            int x,
            int y,
            int width,
            int height,
            int pivotX,
            int pivotY) throws IOException {
        writeU16String(out, name);
        writeU16Be(out, x);
        writeU16Be(out, y);
        writeU16Be(out, width);
        writeU16Be(out, height);
        writeU16Be(out, pivotX);
        writeU16Be(out, pivotY);
    }

    private static byte[] buildSinglePixelCgbiPng(int b, int g, int r, int a) throws IOException {
        ByteArrayOutputStream png = new ByteArrayOutputStream();
        png.write(new byte[] { (byte) 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a });
        writePngChunk(png, "CgBI", new byte[] { 0x50, 0x00, 0x20, 0x06 });
        writePngChunk(png, "IHDR", new byte[] {
            0, 0, 0, 1,
            0, 0, 0, 1,
            8, 6, 0, 0, 0
        });
        byte[] scanline = new byte[] { 0, (byte) b, (byte) g, (byte) r, (byte) a };
        Deflater deflater = new Deflater(Deflater.DEFAULT_COMPRESSION, true);
        deflater.setInput(scanline);
        deflater.finish();
        byte[] compressed = new byte[64];
        int len = deflater.deflate(compressed);
        deflater.end();
        byte[] idat = new byte[len];
        System.arraycopy(compressed, 0, idat, 0, len);
        writePngChunk(png, "IDAT", idat);
        writePngChunk(png, "IEND", new byte[0]);
        return png.toByteArray();
    }

    private static void writePngChunk(ByteArrayOutputStream out, String type, byte[] payload)
            throws IOException {
        int len = payload.length;
        out.write((len >>> 24) & 0xff);
        out.write((len >>> 16) & 0xff);
        out.write((len >>> 8) & 0xff);
        out.write(len & 0xff);
        byte[] typeBytes = type.getBytes(StandardCharsets.US_ASCII);
        out.write(typeBytes);
        out.write(payload);
        CRC32 crc = new CRC32();
        crc.update(typeBytes);
        crc.update(payload);
        long c = crc.getValue();
        out.write((int) ((c >>> 24) & 0xff));
        out.write((int) ((c >>> 16) & 0xff));
        out.write((int) ((c >>> 8) & 0xff));
        out.write((int) (c & 0xff));
    }
}
