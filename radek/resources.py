"""Resource inventory and bounded CgBI (Apple PNG) normalization."""

import binascii
import hashlib
import shutil
import struct
import zlib
from pathlib import Path
from .archive import InputError

PNG = b"\x89PNG\r\n\x1a\n"
MACH_MAGICS = {
    bytes.fromhex(s)
    for s in ("cefaedfe", "cffaedfe", "feedface", "feedfacf", "cafebabe", "cafebabf", "bebafeca", "bfbafeca")
}


def png_chunk(kind: bytes, data: bytes) -> bytes:
    return (
        struct.pack(">I", len(data))
        + kind
        + data
        + struct.pack(">I", binascii.crc32(kind + data) & 0xFFFFFFFF)
    )


def normalize_png(data: bytes) -> bytes:
    if len(data) > 32 * 1024 * 1024 or not data.startswith(PNG):
        raise InputError("invalid/oversized PNG")
    p, chunks, cgbi = 8, [], False
    while p < len(data):
        if p + 12 > len(data):
            raise InputError("truncated PNG")
        n = struct.unpack_from(">I", data, p)[0]
        if n > len(data) - p - 12:
            raise InputError("PNG chunk outside image")
        kind, value = data[p + 4 : p + 8], data[p + 8 : p + 8 + n]
        crc = struct.unpack_from(">I", data, p + 8 + n)[0]
        if binascii.crc32(kind + value) & 0xFFFFFFFF != crc:
            raise InputError("invalid PNG CRC")
        cgbi |= kind == b"CgBI"
        chunks.append((kind, value))
        p += n + 12
    ihdr = [v for k, v in chunks if k == b"IHDR"]
    if len(ihdr) != 1 or len(ihdr[0]) != 13 or not chunks or chunks[-1][0] != b"IEND":
        raise InputError("invalid PNG structure")
    width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", ihdr[0])
    if not width or not height or width > 4096 or height > 4096:
        raise InputError("PNG dimensions exceed limit")
    if not cgbi:
        return data
    if (depth, color, compression, filtering, interlace) != (8, 6, 0, 0, 0):
        raise InputError("unsupported CgBI pixel format")
    stride = width * 4
    expected = (stride + 1) * height
    decoder = zlib.decompressobj(-15)
    raw = decoder.decompress(b"".join(v for k, v in chunks if k == b"IDAT"), expected + 1)
    if len(raw) != expected or not decoder.eof or decoder.unused_data:
        raise InputError("invalid CgBI decompressed size")
    result, previous = bytearray(), bytearray(stride)
    for y in range(height):
        row = bytearray(raw[y * (stride + 1) + 1 : (y + 1) * (stride + 1)])
        f = raw[y * (stride + 1)]
        if f > 4:
            raise InputError("invalid PNG filter")
        for x in range(stride):
            a = row[x - 4] if x >= 4 else 0
            b = previous[x]
            c = previous[x - 4] if x >= 4 else 0
            paeth = a + b - c
            distances = (abs(paeth - a), abs(paeth - b), abs(paeth - c))
            predictor = (0, a, b, (a + b) // 2, (a, b, c)[distances.index(min(distances))])[f]
            row[x] = (row[x] + predictor) & 255
        previous = row[:]
        for x in range(0, stride, 4):
            b, g, r, a = row[x : x + 4]
            row[x : x + 4] = bytes(
                (
                    min(255, (r * 255 + a // 2) // a) if a else 0,
                    min(255, (g * 255 + a // 2) // a) if a else 0,
                    min(255, (b * 255 + a // 2) // a) if a else 0,
                    a,
                )
            )
        result.extend(b"\x00" + row)
    return (
        PNG
        + png_chunk(b"IHDR", ihdr[0])
        + png_chunk(b"IDAT", zlib.compress(result))
        + png_chunk(b"IEND", b"")
    )


def fallback_icon() -> bytes:
    row = b"\x00" + bytes((25, 115, 100, 255)) * 48
    return (
        PNG
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 48, 48, 8, 6, 0, 0, 0))
        + png_chunk(b"IDAT", zlib.compress(row * 48))
        + png_chunk(b"IEND", b"")
    )


def copy_resources(app: Path, target: Path, executable: str) -> list[dict]:
    """Preserve relative resource names; never ship Apple binaries or signatures."""
    target.mkdir(parents=True, exist_ok=True)
    inventory = []
    for source in sorted(app.rglob("*")):
        if not source.is_file():
            continue
        rel = source.relative_to(app)
        if (
            rel.as_posix() == executable
            or any(p in ("_CodeSignature", "SC_Info") for p in rel.parts)
            or source.name == "embedded.mobileprovision"
        ):
            continue
        with source.open("rb") as f:
            if f.read(4) in MACH_MAGICS:
                continue
        dest = target / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        transformed = False
        if source.suffix.lower() == ".png":
            data = source.read_bytes()
            normalized = normalize_png(data)
            dest.write_bytes(normalized)
            transformed = data != normalized
        else:
            shutil.copyfile(source, dest)
        inventory.append(
            {
                "path": rel.as_posix(),
                "bytes": dest.stat().st_size,
                "sha256": hashlib.sha256(dest.read_bytes()).hexdigest(),
                "normalized": transformed,
            }
        )
    return inventory
