"""Bounded PNG/JPEG inspection and PNG decoding/encoding.

This is a decoder, never an executor: it only turns untrusted bytes into pixels.
Supported PNG inputs:

* color types 0 (gray), 2 (RGB), 3 (palette), 4 (gray+alpha), 6 (RGBA)
* bit depths 1, 2, 4, 8, 16
* interlaced (Adam7) and non-interlaced images
* tRNS transparency for gray, RGB and palette images
* Apple's ``CgBI`` variant (channel order swapped, premultiplied alpha, in some
  files stored as a raw deflate stream instead of a zlib stream)

Everything is decoded to 8-bit RGBA so downstream code (Android icon
generation, UI display) never has to know which variant it received. Failures
raise :class:`PngError` (a subclass of :class:`radek.archive.InputError`) so
callers fall back to another icon candidate instead of shipping a fake image.
"""

from __future__ import annotations

import binascii
import struct
import zlib
from dataclasses import dataclass

from .archive import InputError

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
JPEG_MAGIC = b"\xff\xd8\xff"
MAX_DIMENSION = 8192
MAX_PIXELS = 64 * 1024 * 1024
MAX_FILE_BYTES = 32 * 1024 * 1024
CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


class PngError(InputError):
    """Raised when image bytes cannot be decoded. Never fatal to a pipeline."""


@dataclass(frozen=True)
class Image:
    """8-bit straight-alpha RGBA image."""

    width: int
    height: int
    pixels: bytes

    def __post_init__(self):
        if len(self.pixels) != self.width * self.height * 4:
            raise PngError("pixel buffer does not match image geometry")


def png_chunk(kind: bytes, data: bytes) -> bytes:
    return (
        struct.pack(">I", len(data))
        + kind
        + data
        + struct.pack(">I", binascii.crc32(kind + data) & 0xFFFFFFFF)
    )


def is_png(data: bytes) -> bool:
    return data[:8] == PNG_MAGIC


def is_jpeg(data: bytes) -> bool:
    return data[:3] == JPEG_MAGIC


def sniff(data: bytes) -> str:
    """Classify a candidate image as png/jpeg/heif/unsupported."""
    if is_png(data):
        return "png"
    if is_jpeg(data):
        return "jpeg"
    if data[4:8] in (b"ftyp", b"ftyp"[::-1]):
        return "heif"
    return "unsupported"


def chunks(data: bytes) -> list[tuple[bytes, bytes]]:
    """Split a PNG into (type, payload) pairs with CRC verification."""
    if len(data) > MAX_FILE_BYTES:
        raise PngError("image exceeds size limit")
    if not is_png(data):
        raise PngError("not a PNG image")
    result: list[tuple[bytes, bytes]] = []
    p = 8
    while p < len(data):
        if p + 12 > len(data):
            raise PngError("truncated PNG chunk")
        n = struct.unpack_from(">I", data, p)[0]
        if n > len(data) - p - 12:
            raise PngError("PNG chunk outside image")
        kind = data[p + 4 : p + 8]
        value = data[p + 8 : p + 8 + n]
        crc = struct.unpack_from(">I", data, p + 8 + n)[0]
        if binascii.crc32(kind + value) & 0xFFFFFFFF != crc:
            raise PngError("invalid PNG CRC")
        result.append((kind, value))
        p += n + 12
    if not result or result[-1][0] != b"IEND":
        raise PngError("PNG without IEND")
    return result


def dimensions(data: bytes) -> tuple[int, int]:
    """Read width/height from IHDR without decoding pixels."""
    if len(data) > MAX_FILE_BYTES:
        raise PngError("image exceeds size limit")
    if not is_png(data):
        raise PngError("not a PNG image")
    if len(data) < 33:
        raise PngError("truncated PNG header")
    width, height = struct.unpack_from(">II", data, 16)
    if not width or not height or width > MAX_DIMENSION or height > MAX_DIMENSION:
        raise PngError("PNG dimensions out of range")
    return width, height


def jpeg_dimensions(data: bytes) -> tuple[int, int]:
    """Scan SOF markers for image size. No decoding, no dependencies."""
    if not is_jpeg(data):
        raise PngError("not a JPEG image")
    if len(data) > MAX_FILE_BYTES:
        raise PngError("image exceeds size limit")
    p = 2
    while p + 9 < len(data):
        if data[p] != 0xFF:
            p += 1
            continue
        marker = data[p + 1]
        if marker in (0xD8, 0xD9, 0x01) or 0xD0 <= marker <= 0xD7:
            p += 2
            continue
        length = struct.unpack_from(">H", data, p + 2)[0]
        if length < 2 or p + 2 + length > len(data):
            raise PngError("invalid JPEG segment")
        if marker in (
            0xC0,
            0xC1,
            0xC2,
            0xC3,
            0xC5,
            0xC6,
            0xC7,
            0xC9,
            0xCA,
            0xCB,
            0xCD,
            0xCE,
            0xCF,
        ):
            height, width = struct.unpack_from(">HH", data, p + 5)
            if not width or not height or width > MAX_DIMENSION or height > MAX_DIMENSION:
                raise PngError("JPEG dimensions out of range")
            return width, height
        if marker == 0xDA:
            break
        p += 2 + length
    raise PngError("JPEG start-of-frame marker not found")


# --- decoding -----------------------------------------------------------------

_ADAM7 = ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2))


def _inflate(payload: bytes, expected: int) -> bytes:
    """PNG image data is zlib; Apple CgBI files sometimes store raw deflate."""
    if expected > 512 * 1024 * 1024:
        raise PngError("PNG image data exceeds limit")
    for wbits in (15, -15):
        try:
            decoder = zlib.decompressobj(wbits)
            raw = decoder.decompress(payload, expected + 1)
            if len(raw) == expected and decoder.eof and not decoder.unused_data:
                return raw
        except zlib.error:
            continue
    raise PngError("PNG image data cannot be decompressed")


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def _unfilter(raw: bytes, height: int, stride: int, bpp: int) -> bytes:
    """Reverse PNG per-scanline filtering and return concatenated rows."""
    if len(raw) < height * (stride + 1):
        raise PngError("truncated PNG image data")
    out = bytearray()
    previous = bytes(stride)
    for y in range(height):
        base = y * (stride + 1)
        kind = raw[base]
        row = bytearray(raw[base + 1 : base + 1 + stride])
        if kind == 1:
            for x in range(bpp, stride):
                row[x] = (row[x] + row[x - bpp]) & 255
        elif kind == 2:
            row = bytearray((a + b) & 255 for a, b in zip(row, previous))
        elif kind == 3:
            for x in range(stride):
                left = row[x - bpp] if x >= bpp else 0
                row[x] = (row[x] + ((left + previous[x]) >> 1)) & 255
        elif kind == 4:
            for x in range(stride):
                left = row[x - bpp] if x >= bpp else 0
                upleft = previous[x - bpp] if x >= bpp else 0
                row[x] = (row[x] + _paeth(left, previous[x], upleft)) & 255
        elif kind:
            raise PngError("invalid PNG filter type")
        out += row
        previous = row
    return bytes(out)


def _bits_to_samples(rows: bytes, count: int, depth: int) -> bytearray:
    """Expand packed sub-byte samples into one byte per sample."""
    if depth == 8:
        return bytearray(rows[:count])
    if depth == 16:
        return bytearray(rows[: count * 2 : 2])
    per_byte = 8 // depth
    mask = (1 << depth) - 1
    scale = 255 // mask
    out = bytearray(count)
    for i in range(count):
        byte = rows[i // per_byte]
        shift = 8 - depth * (i % per_byte + 1)
        out[i] = ((byte >> shift) & mask) * scale
    return out


def _decode_header(parsed: list[tuple[bytes, bytes]]) -> tuple[int, int, int, int, int, bool]:
    ihdr = [v for k, v in parsed if k == b"IHDR"]
    if len(ihdr) != 1 or len(ihdr[0]) != 13:
        raise PngError("PNG without a single valid IHDR")
    width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", ihdr[0])
    if compression or filtering or interlace > 1:
        raise PngError("unsupported PNG compression/filtering")
    if color not in CHANNELS or depth not in (1, 2, 4, 8, 16):
        raise PngError("unsupported PNG color type/bit depth")
    if color == 3 and depth == 16:
        raise PngError("16-bit palette PNG is invalid")
    if color in (2, 4, 6) and depth not in (8, 16):
        raise PngError("unsupported bit depth for this color type")
    if not width or not height or width > MAX_DIMENSION or height > MAX_DIMENSION:
        raise PngError("PNG dimensions out of range")
    if width * height > MAX_PIXELS:
        raise PngError("PNG pixel count exceeds limit")
    return width, height, depth, color, interlace, b"CgBI" in [k for k, _ in parsed]


def _scatter(
    out: bytearray,
    rows: bytes,
    width: int,
    channels: int,
    depth: int,
    xstart: int,
    ystart: int,
    xstep: int,
    ystep: int,
    passes: int,
) -> None:
    """Copy one (possibly interlaced) pass of samples into the sample array."""
    pw = (width - xstart + xstep - 1) // xstep
    if not pw or not passes:
        return
    stride = (pw * channels * depth + 7) // 8
    bpp = max(1, (channels * depth) // 8)
    unfiltered = _unfilter(rows, passes, stride, bpp)
    for y in range(passes):
        row = unfiltered[y * stride : (y + 1) * stride]
        if depth == 8:
            line = row[: pw * channels]
        elif depth == 16:
            line = row[: pw * channels * 2 : 2]
        else:
            # Sub-byte rows are padded to a byte boundary: unpack this row only.
            line = _bits_to_samples(row, pw * channels, depth)
        dst = (ystart + y * ystep) * width * channels
        for x in range(pw):
            column = (xstart + x * xstep) * channels
            out[dst + column : dst + column + channels] = line[x * channels : (x + 1) * channels]


def _samples(width: int, height: int, depth: int, channels: int, interlace: bool, rows: bytes) -> bytearray:
    """Build a width*height*channels array of 8-bit samples."""
    out = bytearray(width * height * channels)
    if not interlace:
        _scatter(out, rows, width, channels, depth, 0, 0, 1, 1, height)
        return out
    offset = 0
    for xstart, ystart, xstep, ystep in _ADAM7:
        pw = (width - xstart + xstep - 1) // xstep
        ph = (height - ystart + ystep - 1) // ystep
        if not pw or not ph:
            continue
        size = ph * (((pw * channels * depth + 7) // 8) + 1)
        _scatter(out, rows[offset : offset + size], width, channels, depth, xstart, ystart, xstep, ystep, ph)
        offset += size
    return out


def _to_rgba(width: int, height: int, color: int, data: bytearray, palette, transparent) -> bytes:
    count = width * height
    out = bytearray(count * 4)
    if color == 6:
        out[:] = data
    elif color == 2:
        out[0::4] = data[0::3]
        out[1::4] = data[1::3]
        out[2::4] = data[2::3]
        out[3::4] = b"\xff" * count
        if isinstance(transparent, tuple):
            for i in range(count):
                if (data[i * 3], data[i * 3 + 1], data[i * 3 + 2]) == transparent:
                    out[i * 4 + 3] = 0
    elif color == 0:
        out[0::4] = data
        out[1::4] = data
        out[2::4] = data
        out[3::4] = b"\xff" * count
        if isinstance(transparent, int):
            for i in range(count):
                if data[i] == transparent:
                    out[i * 4 + 3] = 0
    elif color == 4:
        out[0::4] = data[0::2]
        out[1::4] = data[0::2]
        out[2::4] = data[0::2]
        out[3::4] = data[1::2]
    else:
        if palette is None:
            raise PngError("palette PNG without PLTE")
        table = []
        for index, (r, g, b) in enumerate(palette):
            alpha = transparent[index] if transparent is not None and index < len(transparent) else 255
            table.append(bytes((r, g, b, alpha)))
        for i in range(count):
            index = data[i]
            if index >= len(table):
                raise PngError("palette index outside palette")
            out[i * 4 : i * 4 + 4] = table[index]
    return bytes(out)


def _unpremultiply(pixels: bytes) -> bytes:
    """Convert premultiplied RGBA into straight alpha. Input and output are RGBA."""
    out = bytearray(pixels)
    for i in range(0, len(out), 4):
        r, g, b, a = out[i], out[i + 1], out[i + 2], out[i + 3]
        if a == 255:
            continue
        if a:
            out[i : i + 4] = bytes(
                (
                    min(255, (r * 255 + a // 2) // a),
                    min(255, (g * 255 + a // 2) // a),
                    min(255, (b * 255 + a // 2) // a),
                    a,
                )
            )
        else:
            out[i : i + 4] = bytes((0, 0, 0, 0))
    return bytes(out)


def decode(data: bytes) -> Image:
    """Decode any supported PNG (including Apple CgBI) into 8-bit RGBA."""
    parsed = chunks(data)
    width, height, depth, color, interlace, apple = _decode_header(parsed)
    channels = CHANNELS[color]
    palette = None
    transparent: object = None
    for kind, value in parsed:
        if kind == b"PLTE" and palette is None:
            if not value or len(value) % 3:
                raise PngError("invalid PNG palette")
            palette = [tuple(value[i : i + 3]) for i in range(0, len(value), 3)]
        elif kind == b"tRNS" and transparent is None:
            if color == 0 and len(value) >= 2:
                key = struct.unpack(">H", value[:2])[0]
                transparent = (key * (255 // ((1 << depth) - 1))) if depth < 8 else value[1]
            elif color == 2 and len(value) >= 6:
                transparent = struct.unpack(">HHH", value[:6])
            elif color == 3:
                transparent = list(value)
    if color == 3 and palette is None:
        raise PngError("palette PNG without PLTE")

    payload = b"".join(v for k, v in parsed if k == b"IDAT")
    if interlace:
        expected = 0
        for xstart, ystart, xstep, ystep in _ADAM7:
            pw = (width - xstart + xstep - 1) // xstep
            ph = (height - ystart + ystep - 1) // ystep
            if pw and ph:
                expected += ph * (((pw * channels * depth + 7) // 8) + 1)
    else:
        expected = height * (((width * channels * depth + 7) // 8) + 1)
    rows = _inflate(payload, expected)
    samples = _samples(width, height, depth, channels, interlace, rows)
    pixels = _to_rgba(width, height, color, samples, palette, transparent)
    if apple:
        # Apple's pngcrush output stores BGR(A) with premultiplied alpha.
        swapped = bytearray(pixels)
        swapped[0::4] = pixels[2::4]
        swapped[2::4] = pixels[0::4]
        pixels = _unpremultiply(bytes(swapped)) if color in (4, 6) else bytes(swapped)
    return Image(width, height, pixels)


def encode(image: Image) -> bytes:
    """Encode 8-bit RGBA as a non-interlaced PNG with unfiltered rows."""
    stride = image.width * 4
    raw = bytearray()
    for y in range(image.height):
        raw.append(0)
        raw.extend(image.pixels[y * stride : (y + 1) * stride])
    return (
        PNG_MAGIC
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", image.width, image.height, 8, 6, 0, 0, 0))
        + png_chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + png_chunk(b"IEND", b"")
    )


# --- resampling ---------------------------------------------------------------


def _decimate(image: Image, step: int) -> Image:
    """Cheap nearest-neighbour reduction by an integer factor."""
    width = max(1, image.width // step)
    height = max(1, image.height // step)
    xmap = [min(image.width - 1, x * step) for x in range(width)]
    out = bytearray(width * height * 4)
    stride = image.width * 4
    for y in range(height):
        base = (y * step) * stride
        target = y * width * 4
        for x, source_x in enumerate(xmap):
            index = base + source_x * 4
            out[target + x * 4 : target + x * 4 + 4] = image.pixels[index : index + 4]
    return Image(width, height, bytes(out))


def resize(image: Image, size: int) -> Image:
    """Box-filter downscale so the longest edge fits ``size`` pixels."""
    if size < 1:
        raise PngError("invalid target size")
    longest = max(image.width, image.height)
    if longest <= size:
        return image
    if longest >= size * 2:
        # Decimate first: the box filter is O(pixels) in pure Python.
        image = _decimate(image, max(2, int(longest / (size * 2))))
        longest = max(image.width, image.height)
        if longest <= size:
            return image
    factor = longest / size
    width = max(1, int(image.width / factor))
    height = max(1, int(image.height / factor))
    out = bytearray(width * height * 4)
    x_ratio = image.width / width
    y_ratio = image.height / height
    for y in range(height):
        top = int(y * y_ratio)
        bottom = min(image.height, max(top + 1, int((y + 1) * y_ratio)))
        for x in range(width):
            left = int(x * x_ratio)
            right = min(image.width, max(left + 1, int((x + 1) * x_ratio)))
            r = g = b = a = 0
            for sy in range(top, bottom):
                base = sy * image.width * 4
                for sx in range(left, right):
                    i = base + sx * 4
                    alpha = image.pixels[i + 3]
                    # Accumulate premultiplied so transparent pixels do not bleed colour.
                    r += image.pixels[i] * alpha
                    g += image.pixels[i + 1] * alpha
                    b += image.pixels[i + 2] * alpha
                    a += alpha
            o = (y * width + x) * 4
            if a:
                out[o : o + 4] = bytes((r // a, g // a, b // a, a // ((right - left) * (bottom - top))))
            else:
                out[o : o + 4] = bytes((0, 0, 0, 0))
    return Image(width, height, bytes(out))


def scale_to(image: Image, size: int) -> Image:
    """Scale so the longest edge is exactly ``size`` pixels (box filter down, nearest up)."""
    longest = max(image.width, image.height)
    if longest == size:
        return image
    if longest > size:
        return resize(image, size)
    width = max(1, round(image.width * size / longest))
    height = max(1, round(image.height * size / longest))
    xmap = [min(image.width - 1, x * image.width // width) for x in range(width)]
    ymap = [min(image.height - 1, y * image.height // height) for y in range(height)]
    rows = []
    stride = image.width * 4
    for source_y in ymap:
        base = source_y * stride
        row = bytearray(width * 4)
        for x, source_x in enumerate(xmap):
            index = base + source_x * 4
            row[x * 4 : x * 4 + 4] = image.pixels[index : index + 4]
        rows.append(bytes(row))
    return Image(width, height, b"".join(rows))


def square(image: Image, size: int, enlarge: bool = False) -> Image:
    """Fit into a transparent square canvas, preserving aspect ratio."""
    scaled = scale_to(image, size) if enlarge else resize(image, size)
    if scaled.width == size and scaled.height == size:
        return scaled
    canvas = bytearray(size * size * 4)
    left = (size - scaled.width) // 2
    top = (size - scaled.height) // 2
    stride = scaled.width * 4
    for y in range(scaled.height):
        row = scaled.pixels[y * stride : (y + 1) * stride]
        offset = ((top + y) * size + left) * 4
        canvas[offset : offset + len(row)] = row
    return Image(size, size, bytes(canvas))
