"""Small structural inspector for Android ARM ELF32/ELF64 shared libraries."""

from __future__ import annotations
import struct
from .archive import InputError


def inspect(data: bytes) -> dict:
    if len(data) < 52 or data[:4] != b"\x7fELF" or data[5] != 1:
        raise InputError("native library is not little-endian ELF")
    elf_class = data[4]
    if elf_class == 2:
        if len(data) < 64:
            raise InputError("truncated ELF64 header")
        etype, machine = struct.unpack_from("<HH", data, 16)
        phoff = struct.unpack_from("<Q", data, 32)[0]
        shoff = struct.unpack_from("<Q", data, 40)[0]
        phentsize, phnum = struct.unpack_from("<HH", data, 54)
        shentsize, shnum = struct.unpack_from("<HH", data, 58)
        phstride, shstride, dynstride, symstride = 56, 64, 16, 24
        arch = "arm64-v8a" if machine == 183 else None
    elif elf_class == 1:
        etype, machine = struct.unpack_from("<HH", data, 16)
        phoff = struct.unpack_from("<I", data, 28)[0]
        shoff = struct.unpack_from("<I", data, 32)[0]
        phentsize, phnum = struct.unpack_from("<HH", data, 42)
        shentsize, shnum = struct.unpack_from("<HH", data, 46)
        phstride, shstride, dynstride, symstride = 32, 40, 8, 16
        arch = "armeabi-v7a" if machine == 40 else None
    else:
        raise InputError("unsupported ELF class")
    if etype != 3 or arch is None:
        raise InputError("native library is not an ARM Android ET_DYN")
    if phentsize != phstride or not phnum or phnum > 1024 or phoff + phnum * phstride > len(data):
        raise InputError("malformed ELF program headers")

    loads: list[tuple[int, int, int]] = []
    dynamic: tuple[int, int] | None = None
    executable = False
    for index in range(phnum):
        offset = phoff + index * phstride
        if elf_class == 2:
            kind, flags, fileoff, va, _, filesz, memsz, _ = struct.unpack_from("<IIQQQQQQ", data, offset)
        else:
            kind, fileoff, va, _, filesz, memsz, flags, _ = struct.unpack_from("<IIIIIIII", data, offset)
        if fileoff + filesz > len(data) or filesz > memsz:
            raise InputError("ELF segment outside file")
        if kind == 1:
            if flags & 3 == 3:
                raise InputError("writable executable ELF segment")
            executable |= bool(flags & 1)
            loads.append((va, fileoff, filesz))
        elif kind == 2:
            dynamic = (fileoff, filesz)
        elif kind == 0x6474E551 and flags & 1:
            raise InputError("executable native stack")
    if not executable or dynamic is None:
        raise InputError("ELF has no code or dynamic loader information")

    tags: dict[int, list[int]] = {}
    if dynamic[1] % dynstride:
        raise InputError("truncated ELF dynamic table")
    for offset in range(dynamic[0], dynamic[0] + dynamic[1], dynstride):
        if elf_class == 2:
            tag, value = struct.unpack_from("<qQ", data, offset)
        else:
            tag, value = struct.unpack_from("<iI", data, offset)
        if tag == 0:
            break
        tags.setdefault(tag, []).append(value)
    needed: list[str] = []
    if 1 in tags:
        if 5 not in tags or 10 not in tags:
            raise InputError("ELF missing string table")
        address, size = tags[5][0], tags[10][0]
        mapping = next(
            ((base, fileoff, length) for base, fileoff, length in loads if base <= address and address + size <= base + length),
            None,
        )
        if mapping is None:
            raise InputError("ELF strings not mapped")
        base, fileoff, _ = mapping
        string_table = data[fileoff + address - base : fileoff + address - base + size]
        for index in tags[1]:
            end = string_table.find(b"\x00", index)
            if index >= size or end < 0:
                raise InputError("invalid ELF dependency")
            needed.append(string_table[index:end].decode("ascii"))

    if (
        shentsize != shstride
        or not shnum
        or shnum > 65535
        or shoff + shnum * shstride > len(data)
    ):
        raise InputError("missing/malformed ELF section table")
    sections = []
    for index in range(shnum):
        offset = shoff + index * shstride
        if elf_class == 2:
            sections.append(struct.unpack_from("<IIQQQQIIQQ", data, offset))
        else:
            sections.append(struct.unpack_from("<IIIIIIIIII", data, offset))

    exports: dict[str, dict] = {}
    undefined: list[str] = []
    for section in sections:
        _, kind, _, _, fileoff, size, link, _, _, stride = section
        if kind != 11:
            continue
        if (
            stride != symstride
            or size % symstride
            or size // symstride > 200000
            or fileoff + size > len(data)
            or link >= shnum
        ):
            raise InputError("invalid ELF dynamic symbols")
        string_section = sections[link]
        if string_section[1] != 3 or string_section[4] + string_section[5] > len(data):
            raise InputError("invalid ELF dynamic strings")
        string_table = data[string_section[4] : string_section[4] + string_section[5]]
        for symoff in range(fileoff + symstride, fileoff + size, symstride):
            if elf_class == 2:
                nameoff, info, visibility, shndx, address, length = struct.unpack_from("<IBBHQQ", data, symoff)
            else:
                nameoff, address, length, info, visibility, shndx = struct.unpack_from("<IIIBBH", data, symoff)
            end = string_table.find(b"\x00", nameoff)
            if nameoff >= len(string_table) or end < 0 or end - nameoff > 4096:
                raise InputError("invalid ELF symbol name")
            name = string_table[nameoff:end].decode("utf-8", errors="strict")
            if info >> 4 not in (1, 2):
                continue
            if shndx == 0:
                undefined.append(name)
            elif visibility & 3 in (0, 3):
                item = {"address": address, "size": length, "type": info & 15}
                mapping = next(
                    ((base, fileoff, amount) for base, fileoff, amount in loads if base <= address and address + length <= base + amount),
                    None,
                )
                if mapping is not None and length:
                    base, fileoff, _ = mapping
                    import hashlib
                    item["sha256"] = hashlib.sha256(data[fileoff + address - base : fileoff + address - base + length]).hexdigest()
                exports[name] = item
    return {"architecture": arch, "elfClass": 64 if elf_class == 2 else 32, "needed": needed, "exports": exports, "undefinedSymbols": undefined}
