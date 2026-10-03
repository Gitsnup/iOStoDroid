"""Synthetic ``Assets.car`` (BOM) builder used only by the test-suite.

These files are generated fixtures, not Apple-produced catalogs, but they use
the same container structures (BOM block index, variables, B+ trees, ``csi``
rendition headers) so the parser is exercised against real layouts.
"""

import struct


class Builder:
    def __init__(self):
        self.blocks: list[bytes] = [b""]
        self.vars: list[tuple[str, int]] = []

    def add(self, data: bytes) -> int:
        self.blocks.append(data)
        return len(self.blocks) - 1

    def tree(self, entries: list[tuple[bytes, bytes]]) -> int:
        """Create a single-leaf B+ tree and return its block index."""
        node = bytearray(struct.pack(">HHII", 1, len(entries), 0, 0))
        for key, value in entries:
            node.extend(struct.pack(">II", self.add(key), self.add(value)))
        child = self.add(bytes(node))
        return self.add(b"tree" + struct.pack(">IIII", 1, child, 4096, len(entries)) + b"\x00")

    def var(self, name: str, index: int) -> None:
        self.vars.append((name, index))

    def build(self) -> bytes:
        header_size = 32
        index_size = 4 + 8 * len(self.blocks)
        vars_size = 4 + sum(5 + len(name.encode()) for name, _ in self.vars)
        data_start = header_size + index_size + vars_size
        offsets: list[tuple[int, int]] = []
        payload = bytearray()
        at = data_start
        for block in self.blocks:
            offsets.append((at if block else 0, len(block)))
            payload.extend(block)
            payload.extend(bytes((-len(payload)) % 4))
            at = data_start + len(payload)
        out = bytearray()
        out.extend(b"BOMStore")
        out.extend(struct.pack(">IIIIII", 1, len(self.blocks), header_size, index_size, header_size + index_size, vars_size))
        out.extend(struct.pack(">I", len(self.blocks)))
        for offset, length in offsets:
            out.extend(struct.pack(">II", offset, length))
        out.extend(struct.pack(">I", len(self.vars)))
        for name, index in self.vars:
            encoded = name.encode()
            out.extend(struct.pack(">I", index) + bytes([len(encoded)]) + encoded)
        out.extend(payload)
        return bytes(out)


def csi(width: int, height: int, scale: int, payload: bytes, filename: str, pixel_format: bytes = b"HEIF") -> bytes:
    """Build a 184-byte rendition header plus payload."""
    header = bytearray(184)
    header[0:4] = b"ISTC"
    struct.pack_into("<IIIII", header, 4, 1, 0, width, height, scale * 100)
    header[24:28] = pixel_format
    struct.pack_into("<I", header, 28, 1)
    struct.pack_into("<I", header, 32, 1700000000)
    struct.pack_into("<H", header, 36, 0)
    name = filename.encode()[:127]
    header[40 : 40 + len(name)] = name
    struct.pack_into("<IIII", header, 168, 0, 0, 0, len(payload))
    return bytes(header) + payload


def key(element: int, part: int, scale: int, identifier: int) -> bytes:
    return struct.pack("<HHHH", element, part, scale, identifier)


def facet(attributes: list[tuple[int, int]]) -> bytes:
    return struct.pack("<HHH", 0, 0, len(attributes)) + b"".join(
        struct.pack("<HH", name, value) for name, value in attributes
    )


def car_header(count: int) -> bytes:
    out = bytearray(436)
    out[0:4] = b"CTAR"
    struct.pack_into("<IIIII", out, 4, 804, 17, 1700000000, count, 0)
    out[24:152] = b"@(#)PROGRAM:CoreUI".ljust(128, b"\x00")
    out[152:408] = b"IBCocoaTouchImageTool".ljust(256, b"\x00")
    return bytes(out)


def key_format(tokens: list[int]) -> bytes:
    return b"kfmt" + struct.pack("<II", 1, len(tokens)) + b"".join(struct.pack("<I", t) for t in tokens)
