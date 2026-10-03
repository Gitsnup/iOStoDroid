"""Round-trip tests for the PNG decoder using generated reference images."""

import struct
import unittest
import zlib

from radek.archive import InputError
from radek.pngcodec import (
    Image,
    PngError,
    chunks,
    decode,
    dimensions,
    encode,
    is_jpeg,
    jpeg_dimensions,
    png_chunk,
    resize,
    scale_to,
    sniff,
    square,
)

MAGIC = b"\x89PNG\r\n\x1a\n"


def build(width, height, depth, color, samples, palette=None, trns=None, interlace=0, apple=False):
    """Encode raw 8-bit sample rows as a PNG (filter type 0, no interlace by default)."""
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    if interlace:
        rows = bytearray()
        passes = ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2))
        for xstart, ystart, xstep, ystep in passes:
            w = (width - xstart + xstep - 1) // xstep
            h = (height - ystart + ystep - 1) // ystep
            if not w or not h:
                continue
            if depth == 8:
                stride = w * channels
                for y in range(h):
                    src = (ystart + y * ystep) * width * channels
                    rows.append(0)
                    for x in range(w):
                        at = src + (xstart + x * xstep) * channels
                        rows.extend(samples[at : at + channels])
            else:
                stride = (w * channels * depth + 7) // 8
                for y in range(h):
                    rows.append(0)
                    line = bytearray(stride)
                    for x in range(w):
                        at = (ystart + y * ystep) * width * channels + (xstart + x * xstep) * channels
                        for c in range(channels):
                            value = samples[at + c] >> (8 - depth)
                            bit = x * channels * depth + c * depth
                            line[bit // 8] |= (value & ((1 << depth) - 1)) << (8 - depth - bit % 8)
                    rows.extend(line)
    elif depth == 8:
        rows = bytearray()
        stride = width * channels
        for y in range(height):
            rows.append(0)
            rows.extend(samples[y * stride : (y + 1) * stride])
    elif depth == 16:
        rows = bytearray()
        stride = width * channels * 2
        for y in range(height):
            rows.append(0)
            for x in range(width * channels):
                value = samples[y * width * channels + x]
                rows.extend((value, value))
    else:
        rows = bytearray()
        stride = (width * channels * depth + 7) // 8
        for y in range(height):
            rows.append(0)
            line = bytearray(stride)
            for x in range(width * channels):
                value = samples[y * width * channels + x] >> (8 - depth)
                bit = x * depth
                line[bit // 8] |= (value & ((1 << depth) - 1)) << (8 - depth - bit % 8)
            rows.extend(line)
    out = MAGIC
    out += png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, color, 0, 0, interlace))
    if apple:
        out += png_chunk(b"CgBI", b"")
    if palette is not None:
        out += png_chunk(b"PLTE", bytes(palette))
    if trns is not None:
        out += png_chunk(b"tRNS", bytes(trns))
    out += png_chunk(b"IDAT", zlib.compress(bytes(rows)))
    out += png_chunk(b"IEND", b"")
    return out


class PngCodecTests(unittest.TestCase):
    def test_rgba8_roundtrip(self):
        samples = bytes(range(256))  # 8x8 RGBA
        data = build(8, 8, 8, 6, samples)
        image = decode(data)
        self.assertEqual((image.width, image.height), (8, 8))
        self.assertEqual(image.pixels, samples)

    def test_gray1_and_gray4(self):
        for depth in (1, 2, 4):
            with self.subTest(depth=depth):
                samples = bytes([0 if (x + y) % 2 else 255 for y in range(4) for x in range(4)])
                image = decode(build(4, 4, depth, 0, samples))
                expected = bytearray()
                for value in samples:
                    expected.extend((value, value, value, 255))
                self.assertEqual(image.pixels, bytes(expected))

    def test_palette_with_transparency(self):
        samples = bytes([0, 1, 2, 3] * 4)
        data = build(4, 4, 8, 3, samples, palette=[10, 20, 30, 40, 50, 60, 70, 80, 90, 1, 2, 3], trns=[255, 0, 128])
        image = decode(data)
        self.assertEqual(image.pixels[:4], bytes((10, 20, 30, 255)))
        self.assertEqual(image.pixels[4:8], bytes((40, 50, 60, 0)))
        self.assertEqual(image.pixels[8:12], bytes((70, 80, 90, 128)))
        self.assertEqual(image.pixels[12:16], bytes((1, 2, 3, 255)))

    def test_rgb16_drops_low_byte(self):
        samples = bytes([0, 255, 128] * 16)
        image = decode(build(4, 4, 16, 2, samples))
        self.assertEqual(image.pixels[:8], bytes((0, 255, 128, 255, 0, 255, 128, 255)))

    def test_gray_alpha(self):
        samples = bytes([10, 20, 30, 40] * 8)
        image = decode(build(4, 4, 8, 4, samples))
        self.assertEqual(image.pixels[:4], bytes((10, 10, 10, 20)))

    def test_interlaced_matches_progressive(self):
        samples = bytes((x * 7 + y * 3) % 256 for y in range(9) for x in range(9 * 4))
        plain = decode(build(9, 9, 8, 6, samples))
        interlaced = decode(build(9, 9, 8, 6, samples, interlace=1))
        self.assertEqual(plain.pixels, interlaced.pixels)

    def test_interlaced_low_bit_depth(self):
        samples = bytes([0 if (x + y) % 3 else 255 for y in range(5) for x in range(5)])
        plain = decode(build(5, 5, 8, 0, samples))
        interlaced = decode(build(5, 5, 2, 0, samples, interlace=1))
        self.assertEqual(plain.pixels, interlaced.pixels)

    def test_apple_cgbi_unpremultiplies_and_swaps(self):
        # Stored as B,G,R,A premultiplied.
        stored = bytearray()
        for value in ((0x10, 0x20, 0x40, 0x80), (0x00, 0x00, 0x00, 0x00), (0x80, 0x80, 0x80, 0xFF)):
            stored.extend(value)
        image = decode(build(3, 1, 8, 6, bytes(stored), apple=True))
        self.assertEqual(image.pixels[:4], bytes((0x80, 0x40, 0x20, 0x80)))
        self.assertEqual(image.pixels[4:8], bytes((0, 0, 0, 0)))
        self.assertEqual(image.pixels[8:12], bytes((0x80, 0x80, 0x80, 0xFF)))

    def test_filters(self):
        """Manually encoded rows exercise Sub/Up/Average/Paeth reconstruction."""
        width, height = 3, 4
        pixels = bytes([(x * 40 + y * 10) % 256 for y in range(height) for x in range(width * 4)])
        rows = bytearray()
        previous = bytes(width * 4)
        for y in range(height):
            row = bytearray(pixels[y * width * 4 : (y + 1) * width * 4])
            if y == 0:
                out = bytearray((row[x] - (row[x - 4] if x >= 4 else 0)) & 255 for x in range(len(row)))
                rows.append(1)
            elif y == 1:
                out = bytearray((row[x] - previous[x]) & 255 for x in range(len(row)))
                rows.append(2)
            elif y == 2:
                out = bytearray(
                    (row[x] - (((row[x - 4] if x >= 4 else 0) + previous[x]) >> 1)) & 255 for x in range(len(row))
                )
                rows.append(3)
            else:
                out = bytearray(len(row))
                for x in range(len(row)):
                    a = row[x - 4] if x >= 4 else 0
                    b = previous[x]
                    c = previous[x - 4] if x >= 4 else 0
                    p = a + b - c
                    best = min((abs(p - a), a), (abs(p - b), b), (abs(p - c), c))[1]
                    out[x] = (row[x] - best) & 255
                rows.append(4)
            rows.extend(out)
            previous = row
        data = (
            MAGIC
            + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + png_chunk(b"IDAT", zlib.compress(bytes(rows)))
            + png_chunk(b"IEND", b"")
        )
        self.assertEqual(decode(data).pixels, pixels)

    def test_encode_decode_roundtrip(self):
        image = Image(3, 2, bytes(range(255, 231, -1)))
        self.assertEqual(decode(encode(image)), image)

    def test_rejects_broken_inputs(self):
        good = build(2, 2, 8, 6, bytes(16))
        broken = bytearray(good)
        broken[30] ^= 0x01
        for data in (b"", b"not a png", good[:20], bytes(broken)):
            with self.subTest(data=data[:12]):
                with self.assertRaises((PngError, InputError)):
                    decode(data)
                self.assertIsInstance(PngError("x"), InputError)

    def test_oversized_dimensions_rejected(self):
        data = MAGIC + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 99999, 99999, 8, 6, 0, 0, 0)) + png_chunk(
            b"IEND", b""
        )
        with self.assertRaises(PngError):
            dimensions(data)

    def test_chunk_crc_and_dimensions(self):
        data = build(7, 5, 8, 6, bytes(7 * 5 * 4))
        self.assertEqual(dimensions(data), (7, 5))
        self.assertEqual([k for k, _ in chunks(data)], [b"IHDR", b"IDAT", b"IEND"])
        self.assertEqual(sniff(data), "png")

    def test_resize_and_square_preserve_alpha(self):
        pixels = bytearray()
        for y in range(8):
            for x in range(8):
                pixels.extend((255, 0, 0, 255 if x < 4 else 0))
        image = Image(8, 8, bytes(pixels))
        small = resize(image, 4)
        self.assertEqual((small.width, small.height), (4, 4))
        self.assertEqual(small.pixels[:4], bytes((255, 0, 0, 255)))
        canvas = square(Image(8, 4, bytes(8 * 4 * 4)), 8)
        self.assertEqual((canvas.width, canvas.height), (8, 8))
        self.assertEqual(canvas.pixels[:4], bytes((0, 0, 0, 0)))  # padding stays transparent

    def test_scale_to_enlarges_and_preserves_pixels(self):
        image = Image(2, 2, bytes([10, 20, 30, 255]) * 4)
        scaled = scale_to(image, 8)
        self.assertEqual((scaled.width, scaled.height), (8, 8))
        self.assertEqual(tuple(scaled.pixels[:4]), (10, 20, 30, 255))
        self.assertEqual(tuple(scaled.pixels[-4:]), (10, 20, 30, 255))
        # Alpha is carried through, not flattened.
        faded = Image(2, 2, bytes([10, 20, 30, 0]) * 4)
        self.assertEqual(tuple(scale_to(faded, 4).pixels[:4]), (10, 20, 30, 0))

    def test_square_can_enlarge_small_artwork(self):
        image = Image(4, 2, bytes([1, 2, 3, 255]) * 8)
        squared = square(image, 8, enlarge=True)
        self.assertEqual((squared.width, squared.height), (8, 8))
        self.assertEqual(tuple(squared.pixels[:4]), (0, 0, 0, 0))  # transparent padding
        middle = (2 * 8 + 0) * 4
        self.assertEqual(tuple(squared.pixels[middle : middle + 4]), (1, 2, 3, 255))

    def test_large_downscale_is_decimated_then_filtered(self):
        size = 64
        image = Image(size, size, bytes([200, 10, 10, 255]) * (size * size))
        out = resize(image, 8)
        self.assertEqual((out.width, out.height), (8, 8))
        self.assertEqual(tuple(out.pixels[:4]), (200, 10, 10, 255))

    def test_jpeg_dimensions(self):
        jpeg = b"\xff\xd8\xff\xe0" + struct.pack(">H", 16) + b"JFIF\x00" + bytes(9)
        sof = struct.pack(">BHHB", 8, 480, 640, 3) + bytes(9)
        jpeg += b"\xff\xc0" + struct.pack(">H", len(sof)) + sof
        self.assertTrue(is_jpeg(jpeg))
        self.assertEqual(jpeg_dimensions(jpeg), (640, 480))
        self.assertEqual(sniff(jpeg), "jpeg")
        with self.assertRaises(PngError):
            jpeg_dimensions(b"\xff\xd8\xff")
