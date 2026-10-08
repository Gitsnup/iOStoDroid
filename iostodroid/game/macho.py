"""Exact parser for classic 32-bit Mach-O MH_EXECUTE images.

Only what the game loader needs is modelled: segments/sections, symbol table,
dynamic symbol table (indirect symbols, external relocations), load commands
we explicitly understand, and the LC_UNIXTHREAD entry state. Anything outside
the understood subset raises :class:`UnsupportedImage` so conversion fails
closed instead of misloading a game.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field


class MachOError(ValueError):
    """Base class for Mach-O parse failures."""


class UnsupportedImage(MachOError):
    """The image is well-formed but outside the loadable subset."""


MH_MAGIC = 0xFEEDFACE
MH_EXECUTE = 0x2
CPU_TYPE_ARM = 12

# Load commands the game loader understands (everything else fails closed).
LC_SEGMENT = 0x1
LC_SYMTAB = 0x2
LC_DYSYMTAB = 0xB
LC_LOAD_DYLIB = 0xC
LC_LOAD_DYLINKER = 0xE
LC_UUID = 0x1B
LC_CODE_SIGNATURE = 0x1D
LC_ENCRYPTION_INFO = 0x21
LC_UNIXTHREAD = 0x5
LC_TWOLEVEL_HINTS = 0x16
LC_PREBIND_CKSUM = 0x17

_KNOWN_COMMANDS = frozenset(
    {
        LC_SEGMENT,
        LC_SYMTAB,
        LC_DYSYMTAB,
        LC_LOAD_DYLIB,
        LC_LOAD_DYLINKER,
        LC_UUID,
        LC_CODE_SIGNATURE,
        LC_ENCRYPTION_INFO,
        LC_UNIXTHREAD,
    }
)

# Section types (low byte of flags).
S_REGULAR = 0x0
S_ZEROFILL = 0x1
S_CSTRING_LITERALS = 0x2
S_4BYTE_LITERALS = 0x3
S_8BYTE_LITERALS = 0x4
S_LITERAL_POINTERS = 0x5
S_NON_LAZY_SYMBOL_PTR = 0x6
S_LAZY_SYMBOL_PTR = 0x7
S_SYMBOL_STUBS = 0x8
S_MOD_INIT_FUNC_POINTERS = 0x9
S_MOD_TERM_FUNC_POINTERS = 0xA

SECTION_TYPE_MASK = 0xFF

# Symbol-table constants.
N_STAB = 0xE0
N_TYPE = 0x0E
N_SECT = 0x0E
N_UNDF = 0x0
N_ABS = 0x2
N_PBUD = 0xC
N_INDR = 0xA
N_EXT = 0x01
REFERENCE_FLAG_UNDEFINED_LAZY = 1

INDIRECT_SYMBOL_LOCAL = 0x80000000
INDIRECT_SYMBOL_ABS = 0x40000000

RELOC_VANILLA = 0


@dataclass
class Section:
    segment: str
    name: str
    address: int
    size: int
    file_offset: int
    align: int
    reloc_offset: int
    reloc_count: int
    flags: int
    reserved1: int
    reserved2: int
    index: int  # 1-based section number across the image

    @property
    def type(self) -> int:
        return self.flags & SECTION_TYPE_MASK

    def contains(self, address: int) -> bool:
        return self.address <= address < self.address + self.size


@dataclass
class Segment:
    name: str
    vmaddr: int
    vmsize: int
    file_offset: int
    file_size: int
    max_prot: int
    init_prot: int
    sections: list = field(default_factory=list)


@dataclass
class Symbol:
    index: int
    name: str
    n_type: int
    n_sect: int
    n_desc: int
    value: int

    @property
    def is_stab(self) -> bool:
        return bool(self.n_type & N_STAB)

    @property
    def is_external(self) -> bool:
        return bool(self.n_type & N_EXT)

    @property
    def is_undefined(self) -> bool:
        return (self.n_type & N_TYPE) == N_UNDF and not self.is_stab

    @property
    def is_defined_in_section(self) -> bool:
        return (self.n_type & N_TYPE) == N_SECT and not self.is_stab

    @property
    def library_ordinal(self) -> int | None:
        if not self.is_undefined:
            return None
        return (self.n_desc >> 8) & 0xFF


@dataclass
class Relocation:
    address: int
    symbol_index: int
    pcrel: bool
    length: int  # 0=byte,1=word16,2=long32,3=quad
    external: bool
    type: int


@dataclass
class Image:
    data: bytes
    cputype: int
    cpusubtype: int
    filetype: int
    flags: int
    segments: list = field(default_factory=list)
    sections: list = field(default_factory=list)  # flat, in section-number order
    symbols: list = field(default_factory=list)
    indirect_symbols: list = field(default_factory=list)
    external_relocations: list = field(default_factory=list)
    local_relocations: list = field(default_factory=list)
    dependencies: list = field(default_factory=list)
    dyld_path: str | None = None
    uuid: bytes | None = None
    encrypted: bool = False
    thread_entry: dict = field(default_factory=dict)
    undefined_symbols: list = field(default_factory=list)

    def section_at(self, address: int) -> Section | None:
        for section in self.sections:
            if section.contains(address):
                return section
        return None

    def section_named(self, segment: str, name: str) -> Section:
        for section in self.sections:
            if section.segment == segment and section.name == name:
                return section
        raise MachOError(f"missing section {segment},{name}")

    def file_offset(self, address: int) -> int:
        for segment in self.segments:
            if segment.vmaddr <= address < segment.vmaddr + segment.vmsize:
                offset = address - segment.vmaddr + segment.file_offset
                if offset > len(self.data):
                    raise MachOError(f"address {address:#x} maps outside the file")
                return offset
        raise MachOError(f"address {address:#x} is not inside any segment")

    def read(self, address: int, size: int) -> bytes:
        if size < 0 or address < 0:
            raise MachOError("negative read")
        section = self.section_at(address)
        if section is not None and section.type == S_ZEROFILL:
            if address + size > section.address + section.size:
                raise MachOError("zerofill read overruns its section")
            return bytes(size)
        start = self.file_offset(address)
        end = self.file_offset(address + size - 1) + 1 if size else start
        return self.data[start:end]

    def read_u32(self, address: int) -> int:
        return struct.unpack("<I", self.read(address, 4))[0]

    def read_cstring(self, address: int, limit: int = 4096) -> bytes:
        out = bytearray()
        cursor = address
        while len(out) < limit:
            byte = self.read(cursor, 1)
            if byte == b"\x00":
                return bytes(out)
            out += byte
            cursor += 1
        raise MachOError(f"unterminated string at {address:#x}")

    def symbol_by_name(self, name: str) -> Symbol | None:
        for symbol in self.symbols:
            if symbol.name == name and not symbol.is_stab:
                return symbol
        return None

    def defined_symbols_at(self, address: int) -> list:
        return [
            symbol
            for symbol in self.symbols
            if symbol.value == address and symbol.is_defined_in_section
        ]


def _read_command_string(data: bytes, offset: int, cmd_offset: int, cmd_size: int) -> str:
    start = offset
    end = cmd_offset + cmd_size
    terminator = data.index(b"\x00", start, end)
    return data[start:terminator].decode("utf-8")


def parse(data: bytes) -> Image:
    """Parse a classic 32-bit Mach-O executable or raise."""
    data = bytes(data)
    if len(data) < 28:
        raise MachOError("file is too small for a Mach-O header")
    magic, cputype, cpusubtype, filetype, ncmds, _, flags = struct.unpack("<7I", data[:28])
    if magic != MH_MAGIC:
        raise MachOError(f"not a 32-bit Mach-O image (magic {magic:#x})")
    if cputype != CPU_TYPE_ARM:
        raise UnsupportedImage(f"unsupported CPU type {cputype}")
    if filetype != MH_EXECUTE:
        raise UnsupportedImage(f"only MH_EXECUTE images load (filetype {filetype})")
    image = Image(data=data, cputype=cputype, cpusubtype=cpusubtype, filetype=filetype, flags=flags)
    symtab = None
    dysymtab = None
    offset = 28
    for _ in range(ncmds):
        if offset + 8 > len(data):
            raise MachOError("load command table overruns the file")
        cmd, cmdsize = struct.unpack("<II", data[offset : offset + 8])
        if cmd not in _KNOWN_COMMANDS and cmd not in (LC_TWOLEVEL_HINTS, LC_PREBIND_CKSUM):
            raise UnsupportedImage(f"unsupported load command {cmd:#x}")
        if cmdsize < 8 or offset + cmdsize > len(data):
            raise MachOError(f"load command {cmd:#x} overruns the file")
        if cmd == LC_SEGMENT:
            _parse_segment(data, offset, image)
        elif cmd == LC_SYMTAB:
            symtab = struct.unpack("<IIII", data[offset + 8 : offset + 24])
        elif cmd == LC_DYSYMTAB:
            dysymtab = struct.unpack("<18I", data[offset + 8 : offset + 80])
        elif cmd == LC_LOAD_DYLIB:
            _parse_dylib(data, offset, cmdsize, image)
        elif cmd == LC_LOAD_DYLINKER:
            image.dyld_path = _read_command_string(data, offset + 12, offset, cmdsize)
        elif cmd == LC_UUID:
            image.uuid = data[offset + 8 : offset + 24]
        elif cmd == LC_ENCRYPTION_INFO:
            _, _, cryptid = struct.unpack("<III", data[offset + 8 : offset + 20])
            image.encrypted = image.encrypted or cryptid != 0
        elif cmd == LC_UNIXTHREAD:
            image.thread_entry = _parse_unix_thread(data, offset, cmdsize)
        offset += cmdsize
    if symtab is None or dysymtab is None:
        raise MachOError("image lacks LC_SYMTAB/LC_DYSYMTAB")
    _parse_symtab(data, symtab, dysymtab, image)
    return image


def _parse_segment(data: bytes, offset: int, image: Image) -> None:
    segname = data[offset + 8 : offset + 24].split(b"\x00")[0].decode("ascii")
    vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, _ = struct.unpack(
        "<IIIIIIII", data[offset + 24 : offset + 56]
    )
    segment = Segment(
        name=segname,
        vmaddr=vmaddr,
        vmsize=vmsize,
        file_offset=fileoff,
        file_size=filesize,
        max_prot=maxprot,
        init_prot=initprot,
    )
    cursor = offset + 56
    for _ in range(nsects):
        sectname = data[cursor : cursor + 16].split(b"\x00")[0].decode("ascii")
        segname2 = data[cursor + 16 : cursor + 32].split(b"\x00")[0].decode("ascii")
        addr, size, file_offset, align, reloff, nreloc, flags, r1, r2 = struct.unpack(
            "<IIIIIIIII", data[cursor + 32 : cursor + 68]
        )
        if segname2 != segname:
            raise MachOError("section segment mismatch")
        section = Section(
            segment=segname,
            name=sectname,
            address=addr,
            size=size,
            file_offset=file_offset,
            align=align,
            reloc_offset=reloff,
            reloc_count=nreloc,
            flags=flags,
            reserved1=r1,
            reserved2=r2,
            index=len(image.sections) + 1,
        )
        segment.sections.append(section)
        image.sections.append(section)
        cursor += 68
    image.segments.append(segment)


def _parse_dylib(data: bytes, offset: int, cmdsize: int, image: Image) -> None:
    name_offset = struct.unpack("<I", data[offset + 8 : offset + 12])[0]
    path = _read_command_string(data, offset + name_offset, offset, cmdsize)
    image.dependencies.append(path)


def _parse_unix_thread(data: bytes, offset: int, cmdsize: int) -> dict:
    # LC_UNIXTHREAD: flavor/count pairs followed by thread state. ARM_THREAD_STATE
    # (flavor 1) carries r0-r12, sp, lr, pc, cpsr (17 words).
    cursor = offset + 8
    end = offset + cmdsize
    while cursor + 8 <= end:
        flavor, count = struct.unpack("<II", data[cursor : cursor + 8])
        cursor += 8
        state = data[cursor : cursor + count * 4]
        if flavor == 1 and count == 17 and len(state) == 68:
            regs = struct.unpack("<17I", state)
            return {
                "flavor": flavor,
                "registers": regs[:13],
                "sp": regs[13],
                "lr": regs[14],
                "pc": regs[15],
                "cpsr": regs[16],
            }
        cursor += count * 4
    return {}


def _parse_symtab(data: bytes, symtab, dysymtab, image: Image) -> None:
    symoff, nsyms, stroff, strsize = symtab
    if symoff + nsyms * 12 > len(data) or stroff + strsize > len(data):
        raise MachOError("symbol/string tables overrun the file")
    (
        ilocalsym,
        nlocalsym,
        iextdefsym,
        nextdefsym,
        iundefsym,
        nundefsym,
        _tocoff,
        _ntoc,
        _modtaboff,
        _nmodtab,
        _extrefsymoff,
        _nextrefsyms,
        indirectsymoff,
        nindirectsyms,
        extreloff,
        nextrel,
        locreloff,
        nlocrel,
    ) = dysymtab

    def string_at(index: int) -> str:
        if index >= strsize:
            raise MachOError("string index outside the string table")
        end = data.index(b"\x00", stroff + index, stroff + strsize)
        return data[stroff + index : end].decode("utf-8", "replace")

    for i in range(nsyms):
        entry = data[symoff + i * 12 : symoff + (i + 1) * 12]
        strx, n_type, n_sect, n_desc, value = struct.unpack("<IBBHI", entry)
        name = "" if strx == 0 else string_at(strx)
        image.symbols.append(
            Symbol(index=i, name=name, n_type=n_type, n_sect=n_sect, n_desc=n_desc, value=value)
        )
    image.undefined_symbols = image.symbols[iundefsym : iundefsym + nundefsym]
    if len(image.undefined_symbols) != nundefsym:
        raise MachOError("undefined symbol range overruns the symbol table")

    if indirectsymoff + nindirectsyms * 4 > len(data):
        raise MachOError("indirect symbol table overruns the file")
    image.indirect_symbols = list(
        struct.unpack(f"<{nindirectsyms}I", data[indirectsymoff : indirectsymoff + nindirectsyms * 4])
    )

    def relocations(offset: int, count: int, expect_external: bool | None) -> list:
        out = []
        for i in range(count):
            raw, word = struct.unpack("<iI", data[offset + i * 8 : offset + (i + 1) * 8])
            address = raw & 0x7FFFFFFF
            pcrel = bool(word & 0x01000000)
            length = (word >> 25) & 0x3
            external = bool(word & 0x08000000)
            symbol_or_value = word & 0xFFFFFF
            reloc_type = (word >> 28) & 0xF
            if expect_external is not None and external != expect_external:
                raise MachOError("relocation external-bit mismatch")
            out.append(
                Relocation(
                    address=address,
                    symbol_index=symbol_or_value,
                    pcrel=pcrel,
                    length=length,
                    external=external,
                    type=reloc_type,
                )
            )
        return out

    if extreloff + nextrel * 8 > len(data) or locreloff + nlocrel * 8 > len(data):
        raise MachOError("relocation tables overrun the file")
    image.external_relocations = relocations(extreloff, nextrel, True)
    image.local_relocations = relocations(locreloff, nlocrel, False)


def import_name(image: Image, symbol_index: int) -> str:
    """Resolve an indirect/reloc symbol index to its import name."""
    if symbol_index & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS):
        raise MachOError(f"symbol index {symbol_index:#x} is not an import reference")
    symbol = image.symbols[symbol_index]
    if not symbol.is_undefined:
        raise MachOError(f"symbol index {symbol_index} does not reference an import")
    return symbol.name


def stub_map(image: Image) -> list:
    """Map every __symbol_stub slot to (stub_addr, lazy_slot_addr, import_name)."""
    stubs = image.section_named("__TEXT", "__symbol_stub4")
    lazy = image.section_named("__DATA", "__la_symbol_ptr")
    if stubs.reserved2 not in (12,):
        raise UnsupportedImage(f"unexpected stub size {stubs.reserved2}")
    count = stubs.size // stubs.reserved2
    if lazy.size // 4 < count:
        raise MachOError("lazy pointer section is smaller than the stub table")
    entries = []
    for i in range(count):
        symbol_index = image.indirect_symbols[stubs.reserved1 + i]
        entries.append(
            (
                stubs.address + i * stubs.reserved2,
                lazy.address + i * 4,
                import_name(image, symbol_index),
            )
        )
    return entries


def non_lazy_map(image: Image) -> list:
    """Map every __nl_symbol_ptr slot to (slot_addr, import_name | None)."""
    section = image.section_named("__DATA", "__nl_symbol_ptr")
    entries = []
    for i in range(section.size // 4):
        symbol_index = image.indirect_symbols[section.reserved1 + i]
        if symbol_index & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS):
            entries.append((section.address + i * 4, None))
        else:
            entries.append((section.address + i * 4, import_name(image, symbol_index)))
    return entries
