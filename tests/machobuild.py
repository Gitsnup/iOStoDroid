"""Synthetic but structurally real Mach-O builder for reconstruction tests.

The files produced here are not signed installable iOS binaries. They contain
the same loader structures the analyzer validates (segments, sections, symbol
table, indirect symbol table, function starts, Objective-C and Swift metadata
sections) so the reconstruction layer is exercised against realistic layouts.

ARM64 encodings are produced by the small encoder below, written from the
instruction encodings; the decoder under test must agree with it.
"""

import struct
from dataclasses import dataclass, field

# --- tiny ARM64 encoder -------------------------------------------------------


def ret() -> int:
    return 0xD65F03C0


def nop() -> int:
    return 0xD503201F


def movz(register: int, immediate: int) -> int:
    return 0xD2800000 | ((immediate & 0xFFFF) << 5) | register


def movk(register: int, immediate: int, shift: int = 0) -> int:
    return 0xF2800000 | ((shift // 16) << 21) | ((immediate & 0xFFFF) << 5) | register


def add_imm(dst: int, src: int, immediate: int) -> int:
    return 0x91000000 | ((immediate & 0xFFF) << 10) | (src << 5) | dst


def sub_sp(immediate: int) -> int:
    return 0xD1000000 | ((immediate & 0xFFF) << 10) | (31 << 5) | 31


def stp(src: int, src2: int, base: int, offset: int) -> int:
    return 0xA9000000 | (((offset // 8) & 0x7F) << 15) | (src2 << 10) | (base << 5) | src


def ldp(dst: int, dst2: int, base: int, offset: int) -> int:
    return 0xA9400000 | (((offset // 8) & 0x7F) << 15) | (dst2 << 10) | (base << 5) | dst


def ldr_imm(dst: int, base: int, offset: int) -> int:
    return 0xF9400000 | (((offset // 8) & 0xFFF) << 10) | (base << 5) | dst


def str_imm(src: int, base: int, offset: int) -> int:
    return 0xF9000000 | (((offset // 8) & 0xFFF) << 10) | (base << 5) | src


def branch(address: int, target: int) -> int:
    delta = (target - address) >> 2
    return 0x14000000 | (delta & 0x3FFFFFF)


def branch_link(address: int, target: int) -> int:
    delta = (target - address) >> 2
    return 0x94000000 | (delta & 0x3FFFFFF)


def branch_register(register: int) -> int:
    return 0xD61F0000 | (register << 5)


def branch_link_register(register: int) -> int:
    return 0xD63F0000 | (register << 5)


def cbz(register: int, address: int, target: int) -> int:
    delta = (target - address) >> 2
    return 0xB4000000 | ((delta & 0x7FFFF) << 5) | register


def adrp(register: int, address: int, page: int) -> int:
    delta = (page >> 12) - (address >> 12)
    if not -(1 << 20) <= delta < (1 << 20):
        raise ValueError("ADRP target page is out of range")
    delta &= 0x1FFFFF
    return 0x90000000 | ((delta & 3) << 29) | (((delta >> 2) & 0x7FFFF) << 5) | register


def ldr_literal(register: int, address: int, target: int) -> int:
    delta = (target - address) >> 2
    return 0x58000000 | ((delta & 0x7FFFF) << 5) | register


def cmp_reg(a: int, b: int) -> int:
    return 0xEB00001F | (a << 5) | (b << 16)


S_REGULAR = 0x0
S_ZEROFILL = 0x1
S_CSTRING_LITERALS = 0x2
S_LITERAL_POINTERS = 0x6
S_NON_LAZY_SYMBOL_POINTERS = 0x6
S_LAZY_SYMBOL_POINTERS = 0x7
S_SYMBOL_STUBS = 0x8
S_MOD_INIT_FUNC_POINTERS = 0x9
S_REGULAR_INSTRUCTIONS = 0x80000400
S_ATTR_PURE_INSTRUCTIONS = 0x80000000
S_ATTR_SOME_INSTRUCTIONS = 0x00000400


@dataclass
class Section:
    name: str
    segment: str
    data: bytearray
    address: int = 0
    offset: int = 0
    flags: int = S_REGULAR
    reserved1: int = 0
    reserved2: int = 0
    align: int = 3
    at: int | None = None  # explicit file offset, so tests can know addresses up front

    @property
    def size(self) -> int:
        return len(self.data)


@dataclass
class Builder:
    """Assemble a Mach-O with __TEXT/__DATA segments and realistic metadata."""

    cpu: int = 0x100000C
    subtype: int = 0
    filetype: int = 2
    segments: dict = field(default_factory=dict)
    sections: list = field(default_factory=list)
    symbols: list = field(default_factory=list)
    indirect_symbols: list = field(default_factory=list)
    function_starts: list = field(default_factory=list)
    entry: int | None = None
    dependencies: list = field(default_factory=list)

    TEXT_BASE = 0x100000000
    DATA_BASE = 0x100004000
    HEADER_SIZE = 0x1000
    TEXT_FILE = 0x0
    DATA_FILE = 0x4000

    def __init__(self):
        self.segments = {
            "__TEXT": {"vm": self.TEXT_BASE, "file": self.TEXT_FILE, "size": 0x4000, "prot": 5},
            "__DATA": {"vm": self.DATA_BASE, "file": self.DATA_FILE, "size": 0x2000, "prot": 3},
        }
        self.sections = []
        self.symbols = []
        self.indirect_symbols = []
        self.function_starts = []
        self.dependencies = []
        self.entry = None

    # --- sections -------------------------------------------------------------

    def section(self, segment: str, name: str, data: bytes = b"", flags: int = S_REGULAR, at: int | None = None, **extra) -> Section:
        item = Section(name=name, segment=segment, data=bytearray(data), flags=flags, at=at)
        for key, value in extra.items():
            setattr(item, key, value)
        self.sections.append(item)
        if at is not None:
            item.offset = at
            item.address = self._address(segment, at)
        return item

    def _address(self, segment: str, offset: int) -> int:
        base = self.segments[segment]["vm"] - self.segments[segment]["file"]
        return base + offset

    def pointer(self, section: Section, value: int) -> int:
        """Append a pointer to a section; returns its address."""
        address = self.address(section, len(section.data))
        section.data.extend(struct.pack("<Q", value))
        return address

    def int32(self, section: Section, value: int) -> int:
        address = self.address(section, len(section.data))
        section.data.extend(struct.pack("<i", value))
        return address

    def address(self, section: Section, index: int = 0) -> int:
        return section.address + index

    # --- symbols --------------------------------------------------------------

    def symbol(self, name: str, value: int, section_index: int = 0, external: bool = True, thumb: bool = False):
        self.symbols.append(
            {
                "name": name,
                "value": value,
                "section": section_index,
                "external": external,
                "thumb": thumb,
            }
        )
        return len(self.symbols) - 1

    # --- layout ---------------------------------------------------------------

    def build(self) -> bytes:
        cursor = {"__TEXT": self.HEADER_SIZE, "__DATA": self.DATA_FILE}
        for item in self.sections:
            if item.at is not None:
                cursor[item.segment] = max(cursor[item.segment], item.offset + len(item.data))
        for item in self.sections:
            if item.at is not None:
                continue
            segment = item.segment
            cursor[segment] = (cursor[segment] + 7) // 8 * 8
            item.offset = cursor[segment]
            item.address = self._address(segment, item.offset)
            cursor[segment] += len(item.data)
        for item in self.sections:
            limit = 0x4000 if item.segment == "__TEXT" else 0x6000
            assert item.offset + len(item.data) <= limit, f"{item.name} overflows {item.segment}"

        file_size = 0x8000
        blob = bytearray(file_size)
        for item in self.sections:
            blob[item.offset : item.offset + len(item.data)] = item.data

        # symbol table after the segments
        symoff = 0x6000
        strings = bytearray(b"\x00")
        table = bytearray()
        for item in self.symbols:
            index = len(strings)
            strings.extend(item["name"].encode() + b"\x00")
            n_type = 0x0F if item["external"] else 0x0E
            if item["section"] == 0:
                n_type = 0x01  # N_UNDF | N_EXT
            if item["thumb"]:
                item_desc = 0x0008
            else:
                item_desc = 0
            table.extend(
                struct.pack("<IBBHQ", index, n_type, item["section"], item_desc, item["value"])
            )
        stroff = symoff + len(table)
        blob[symoff : symoff + len(table)] = table
        blob[stroff : stroff + len(strings)] = strings
        strsize = len(strings)

        indirect_off = stroff + strsize + ((8 - (stroff + strsize) % 8) % 8)
        indirect_blob = b"".join(struct.pack("<I", index) for index in self.indirect_symbols)
        blob[indirect_off : indirect_off + len(indirect_blob)] = indirect_blob

        starts = self._uleb_starts()
        start_off = indirect_off + len(indirect_blob) + 8
        blob[start_off : start_off + len(starts)] = starts

        commands = []
        for name, info in (("__TEXT", self.segments["__TEXT"]), ("__DATA", self.segments["__DATA"])):
            items = [s for s in self.sections if s.segment == name]
            size = 72 + 80 * len(items)
            payload = struct.pack(
                "<II16sQQQQiiII",
                0x19,
                size,
                name.encode(),
                info["vm"],
                info["size"],
                info["file"],
                info["size"],
                info["prot"],
                info["prot"],
                len(items),
                0,
            )
            for item in items:
                payload += struct.pack(
                    "<16s16sQQIIIIIIII",
                    item.name.encode(),
                    name.encode(),
                    item.address,
                    len(item.data),
                    item.offset,
                    item.align,
                    0,
                    0,
                    item.flags,
                    item.reserved1,
                    item.reserved2,
                    0,
                )
            commands.append(payload)
        commands.append(struct.pack("<IIIIII", 2, 24, symoff, len(self.symbols), stroff, strsize))
        commands.append(
            struct.pack(
                "<IIIIIIIIIIIIIIIIIIII",
                0xB,
                80,
                # local, external, undefined, toc, module, reference pairs
                0, len(self.symbols),
                0, len(self.symbols),
                0, 0,
                0, 0,
                0, 0,
                0, 0,
                indirect_off,
                len(self.indirect_symbols),
                # external relocation pair, local relocation pair
                0, 0, 0, 0,
            )
        )
        commands.append(struct.pack("<IIII", 0x26, 16, start_off, len(starts)))
        if self.entry is not None:
            # LC_MAIN entryoff is an offset from the start of the __TEXT segment.
            entry_off = self.entry - self.TEXT_BASE + self.segments["__TEXT"]["file"]
            commands.append(struct.pack("<IIQQ", 0x80000028, 24, entry_off, 0))
        for dep in self.dependencies:
            raw = dep.encode() + b"\x00"
            size = (24 + len(raw) + 7) // 8 * 8
            commands.append(struct.pack("<IIIIII", 0xC, size, 24, 0, 0x10000, 0x10000) + raw.ljust(size - 24, b"\x00"))

        header_size = 32
        total = sum(len(c) for c in commands)
        assert header_size + total <= self.HEADER_SIZE, "load commands overflow the reserved page"
        header = struct.pack("<IIIIIII", 0xFEEDFACF, self.cpu, self.subtype, self.filetype, len(commands), total, 0)
        header += bytes(4)
        blob[0 : len(header)] = header
        cursor = len(header)
        for command in commands:
            blob[cursor : cursor + len(command)] = command
            cursor += len(command)
        return bytes(blob)

    def _uleb_starts(self) -> bytes:
        out = bytearray()
        previous = self.TEXT_BASE  # LC_FUNCTION_STARTS deltas start at the __TEXT base
        for address in sorted(a for a in self.function_starts if a >= previous):
            delta = address - previous
            while True:
                byte = delta & 0x7F
                delta >>= 7
                if delta:
                    out.append(byte | 0x80)
                else:
                    out.append(byte)
                    break
            previous = address
        return bytes(out)


def uleb(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)
