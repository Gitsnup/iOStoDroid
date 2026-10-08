"""Bounded DEX header, integrity and class identity validation (not a VM)."""

import hashlib
import struct
import zlib
from .archive import InputError


def classes(data: bytes) -> set[str]:
    if (
        len(data) < 112
        or data[:4] != b"dex\n"
        or data[7] != 0
        or data[4:7] not in (b"035", b"037", b"038", b"039", b"040")
    ):
        raise InputError("unsupported or truncated DEX header")
    checksum = struct.unpack_from("<I", data, 8)[0]
    if checksum != zlib.adler32(data[12:]) & 0xFFFFFFFF or data[12:32] != hashlib.sha1(data[32:]).digest():
        raise InputError("DEX checksum/signature mismatch")
    size, header, endian = struct.unpack_from("<III", data, 32)
    if size != len(data) or header != 112 or endian != 0x12345678:
        raise InputError("invalid DEX size/header/endian")

    def table(at, stride):
        count, offset = struct.unpack_from("<II", data, at)
        if count > 1000000 or offset > len(data) or count * stride > len(data) - offset:
            raise InputError("DEX table outside file")
        if count and (offset < 112 or offset % 4):
            raise InputError("invalid DEX table offset")
        return count, offset

    ns, strings = table(56, 4)
    nt, types = table(64, 4)
    nc, definitions = table(96, 32)
    result = set()
    for index in range(nc):
        type_index = struct.unpack_from("<I", data, definitions + index * 32)[0]
        if type_index >= nt:
            raise InputError("invalid DEX class type")
        string_index = struct.unpack_from("<I", data, types + type_index * 4)[0]
        if string_index >= ns:
            raise InputError("invalid DEX descriptor string")
        offset = struct.unpack_from("<I", data, strings + string_index * 4)[0]
        # MUTF8 size uses ULEB128. Descriptors for our fixed Java entry classes
        # are ASCII; retain other descriptors as escaped bytes without execution.
        for shift in range(0, 35, 7):
            if offset >= len(data):
                raise InputError("truncated DEX string")
            byte = data[offset]
            offset += 1
            if shift == 28 and byte & 0xF0:
                raise InputError("DEX ULEB128 overflow")
            if not byte & 0x80:
                break
        else:
            raise InputError("DEX ULEB128 overflow")
        end = data.find(b"\x00", offset, min(len(data), offset + 4097))
        if end < 0:
            raise InputError("oversized/unterminated DEX descriptor")
        descriptor = data[offset:end].decode("ascii", errors="backslashreplace")
        if not descriptor.startswith("L") or not descriptor.endswith(";"):
            raise InputError("invalid DEX class descriptor")
        if descriptor in result:
            raise InputError("duplicate DEX class definition")
        result.add(descriptor)
    return result
