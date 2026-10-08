"""Objective-C v2 (non-fragile ABI) metadata parser.

The game defines two classes (`AppController`, `MyEAGLView`) and references
UIKit/Foundation classes. The static structures live in `__objc_data` (class
pairs) and `__objc_const` (read-only parts); dyld-bound slots (external
classes, superclasses, method caches) carry external relocations and are
owned by the converter's loader, exactly like lazy pointers.

Every word of every ObjC section is claimed by the parse and classified as
SLIDE (absolute local pointer), IMPORT (loader-bound slot, with the bound
symbol recorded for the runtime), or VALUE (integer, count, flags). Anything
unclaimed fails closed as a review item in `pointers`.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from . import macho


@dataclass
class ObjCModel:
    classes: list = field(default_factory=list)  # (class_addr, meta_addr, name)
    methods: list = field(default_factory=list)  # (class_name, sel, types, imp)
    protocols: list = field(default_factory=list)  # (addr, name)
    bound_symbols: dict = field(default_factory=dict)  # slot -> symbol name
    sites: dict = field(default_factory=dict)  # addr -> (kind, evidence)
    problems: list = field(default_factory=list)  # (addr, message)


def _cstring_ok(image: macho.Image, addr: int) -> bool:
    try:
        section = image.section_at(addr)
    except Exception:
        return False
    if section is None or section.name != "__cstring":
        return False
    end = section.address + section.size
    cursor = addr
    while cursor < end:
        byte = image.read(cursor, 1)[0]
        if byte == 0:
            return cursor > addr
        if byte < 32 or byte > 126:
            return False
        cursor += 1
    return False


class _Parser:
    def __init__(self, image: macho.Image):
        self.image = image
        self.model = ObjCModel()
        self.reloc_symbol = {
            reloc.address: image.symbols[reloc.symbol_index].name
            for reloc in image.external_relocations
            if reloc.length == 2 and not reloc.pcrel
        }
        self.claimed: set = set()

    def _word(self, addr: int) -> int:
        return self.image.read_u32(addr)

    def _claim(self, addr: int, kind: str, evidence: str) -> None:
        self.claimed.add(addr)
        self.model.sites[addr] = (kind, evidence)

    def _pointer(self, addr: int, what: str) -> int:
        value = self._word(addr)
        if value == 0:
            if addr in self.reloc_symbol:
                name = self.reloc_symbol[addr]
                self._claim(addr, "IMPORT", f"{what}:binds:{name}")
                self.model.bound_symbols[addr] = name
            else:
                self._claim(addr, "VALUE", f"{what}:null")
            return 0
        if addr in self.reloc_symbol:
            self.model.problems.append(
                (addr, f"{what}: nonzero reloc-bound slot"))
        self._claim(addr, "SLIDE", what)
        return value

    def _int(self, addr: int, what: str) -> int:
        value = self._word(addr)
        self._claim(addr, "VALUE", f"{what}={value:#x}")
        return value

    def _cstring(self, addr: int, what: str) -> int:
        value = self._pointer(addr, what)
        if value and not _cstring_ok(self.image, value):
            self.model.problems.append((addr, f"{what}: not a cstring"))
        return value

    def _method_list(self, addr: int, owner: str) -> None:
        entsize = self._int(addr, f"{owner}.mDesc.entsize")
        count = self._int(addr + 4, f"{owner}.mDesc.count")
        if entsize & ~3 != 12:
            self.model.problems.append((addr, f"{owner}: bad method entsize"))
            return
        if count > 1000:
            self.model.problems.append((addr, f"{owner}: absurd method count"))
            return
        for i in range(count):
            base = addr + 8 + i * 12
            name = self._cstring(base, f"{owner}.m{i}.name")
            types = self._cstring(base + 4, f"{owner}.m{i}.types")
            imp = self._pointer(base + 8, f"{owner}.m{i}.imp")
            sel = self._read_cstr(name) if name else ""
            self.model.methods.append((owner, sel, types, imp))

    def _read_cstr(self, addr: int) -> str:
        out = bytearray()
        while len(out) < 128:
            byte = self.image.read(addr + len(out), 1)[0]
            if byte == 0:
                break
            out.append(byte)
        return out.decode("ascii", "replace")

    def _ivar_list(self, addr: int, owner: str) -> None:
        entsize = self._int(addr, f"{owner}.ivarDesc.entsize")
        count = self._int(addr + 4, f"{owner}.ivarDesc.count")
        if entsize != 20 or count > 1000:
            self.model.problems.append((addr, f"{owner}: bad ivar list"))
            return
        for i in range(count):
            base = addr + 8 + i * 20
            offset_slot = self._pointer(base, f"{owner}.ivar{i}.offset")
            if offset_slot:
                try:
                    offset_value = self._word(offset_slot)
                except macho.MachOError:
                    offset_value = None
                if offset_value is None or offset_value >= 0x10000:
                    self.model.problems.append(
                        (base, f"{owner}.ivar{i}: bad offset value"))
                else:
                    self._claim(offset_slot, "VALUE",
                                f"{owner}.ivar{i}.offset={offset_value:#x}")
            self._cstring(base + 4, f"{owner}.ivar{i}.name")
            self._cstring(base + 8, f"{owner}.ivar{i}.type")
            self._int(base + 12, f"{owner}.ivar{i}.align")
            self._int(base + 16, f"{owner}.ivar{i}.size")

    def _protocol_list(self, addr: int, owner: str) -> None:
        count = self._int(addr, f"{owner}.protoDesc.count")
        if count > 1000:
            self.model.problems.append((addr, f"{owner}: absurd protocol count"))
            return
        for i in range(count):
            proto = self._pointer(addr + 4 + i * 4, f"{owner}.proto{i}")
            if proto:
                self._protocol(proto)

    def _protocol(self, addr: int) -> None:
        if any(addr == claimed for claimed in self.claimed
               if self.model.sites.get(claimed, ("", ""))[1].startswith("proto:")):
            return
        self._int(addr, "proto:isa")
        name = self._cstring(addr + 4, "proto:name")
        label = self._read_cstr(name) if name else f"@{addr:#x}"
        self.model.protocols.append((addr, label))
        nested = self._pointer(addr + 8, "proto:protocols")
        if nested:
            self._protocol_list(nested, f"proto:{label}")
        for offset, kind in ((12, "inst"), (16, "class"), (20, "optInst"),
                             (24, "optClass")):
            methods = self._pointer(addr + offset, f"proto:{label}.{kind}")
            if methods:
                self._method_list(methods, f"proto:{label}.{kind}")
        props = self._pointer(addr + 28, f"proto:{label}.props")
        if props:
            self._property_list(props, f"proto:{label}")
        self._int(addr + 32, "proto:size")
        self._int(addr + 36, "proto:flags")

    def _property_list(self, addr: int, owner: str) -> None:
        entsize = self._int(addr, f"{owner}.propDesc.entsize")
        count = self._int(addr + 4, f"{owner}.propDesc.count")
        if entsize != 8 or count > 1000:
            self.model.problems.append((addr, f"{owner}: bad property list"))
            return
        for i in range(count):
            base = addr + 8 + i * 8
            self._cstring(base, f"{owner}.prop{i}.name")
            self._cstring(base + 4, f"{owner}.prop{i}.attrs")

    def _class_ro(self, addr: int, owner: str) -> str:
        self._int(addr, f"{owner}.ro.flags")
        self._int(addr + 4, f"{owner}.ro.start")
        self._int(addr + 8, f"{owner}.ro.size")
        layout = self._pointer(addr + 12, f"{owner}.ro.layout")
        if layout and not _cstring_ok(self.image, layout):
            self.model.problems.append((addr + 12, f"{owner}: bad ivar layout"))
        name = self._cstring(addr + 16, f"{owner}.ro.name")
        label = self._read_cstr(name) if name else f"@{addr:#x}"
        methods = self._pointer(addr + 20, f"{owner}.ro.methods")
        if methods:
            self._method_list(methods, label)
        protocols = self._pointer(addr + 24, f"{owner}.ro.protocols")
        if protocols:
            self._protocol_list(protocols, label)
        ivars = self._pointer(addr + 28, f"{owner}.ro.ivars")
        if ivars:
            self._ivar_list(ivars, label)
        self._pointer(addr + 32, f"{owner}.ro.weakLayout")
        props = self._pointer(addr + 36, f"{owner}.ro.props")
        if props:
            self._property_list(props, label)
        return label

    def _class(self, addr: int, is_meta: bool) -> int:
        tag = "meta" if is_meta else "class"
        self._pointer(addr, f"{tag}@{addr:#x}.isa")
        self._pointer(addr + 4, f"{tag}@{addr:#x}.super")
        self._pointer(addr + 8, f"{tag}@{addr:#x}.cache")
        self._pointer(addr + 12, f"{tag}@{addr:#x}.vtable")
        data = self._pointer(addr + 16, f"{tag}@{addr:#x}.data")
        return data


def parse(image: macho.Image) -> ObjCModel:
    """Parse all ObjC metadata; every section word is claimed or reported."""
    parser = _Parser(image)
    model = parser.model
    try:
        classlist = image.section_named("__DATA", "__objc_classlist")
    except macho.MachOError:
        return model
    class_addrs = [
        image.read_u32(classlist.address + off)
        for off in range(0, classlist.size, 4)
    ]
    for index, addr in enumerate(class_addrs):
        slot = classlist.address + index * 4
        parser._pointer(slot, "classlist")
        meta = addr - 20
        meta_data = parser._class(meta, True)
        meta_name = parser._class_ro(meta_data, f"meta@{meta:#x}") if meta_data else ""
        class_data = parser._class(addr, False)
        class_name = parser._class_ro(class_data, f"class@{addr:#x}") if class_data else ""
        if meta_name != class_name or not class_name:
            model.problems.append(
                (addr, f"class/meta name mismatch: {class_name!r}/{meta_name!r}"))
        else:
            model.classes.append((addr, meta, class_name))
    for section_name, item, kind in (
        ("__objc_selrefs", 4, "selref"),
        ("__objc_superrefs", 4, "superref"),
        ("__objc_protolist", 4, "protolist"),
    ):
        try:
            section = image.section_named("__DATA", section_name)
        except macho.MachOError:
            continue
        for off in range(0, section.size, 4):
            slot = section.address + off
            if kind == "selref":
                parser._cstring(slot, kind)
            else:
                value = parser._pointer(slot, kind)
                if kind == "protolist" and value:
                    parser._protocol(value)
    try:
        info = image.section_named("__DATA", "__objc_imageinfo")
        parser._int(info.address, "imageinfo:version")
        parser._int(info.address + 4, "imageinfo:flags")
    except macho.MachOError:
        pass
    try:
        classrefs = image.section_named("__DATA", "__objc_classrefs")
        for off in range(0, classrefs.size, 4):
            parser._pointer(classrefs.address + off, "classref")
    except macho.MachOError:
        pass
    # Anything left unclaimed in an ObjC section fails closed.
    for section in image.sections:
        if section.segment != "__DATA" or "__objc" not in section.name:
            continue
        addr = section.address
        while addr < section.address + section.size:
            if addr not in parser.claimed:
                model.problems.append((addr, f"unclaimed {section.name} word"))
            addr += 4
    return model
