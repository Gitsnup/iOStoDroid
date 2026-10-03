"""Swift metadata recovery and partial symbol demangling.

Swift metadata is recovered from the ``__swift5_*`` sections. The demangler is
deliberately partial: it resolves module/decl name paths, common type
substitutions and entity kinds, and stops with an explicit marker whenever it
meets grammar it does not implement. It never invents a signature.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

from .image import MachOImage

KINDS = {
    0: "module",
    1: "extension",
    2: "anonymous",
    3: "protocol",
    4: "opaque-type",
    16: "class",
    17: "struct",
    18: "enum",
}

SUBSTITUTIONS = {
    "a": "Swift.Array",
    "b": "Swift.Bool",
    "c": "Swift.Character",
    "d": "Swift.Double",
    "D": "Swift.Dictionary",
    "f": "Swift.Float",
    "g": "Swift.StaticString",
    "i": "Swift.Int",
    "p": "Swift.UnsafeMutablePointer",
    "P": "Swift.UnsafePointer",
    "q": "Swift.Optional",
    "R": "Swift.UnsafeBufferPointer",
    "s": "Swift.String",
    "S": "Swift.String",
    "u": "Swift.UInt",
    "v": "Swift.UnsafeRawPointer",
    "V": "Swift.UnsafeMutableRawPointer",
}

ENTITY = {
    "C": "class",
    "V": "struct",
    "O": "enum",
    "P": "protocol",
    "F": "function",
    "fC": "init",
    "fD": "deinit",
    "fU": "explicit-closure",
    "fv": "variable",
    "TZ": "global",
    "Te": "extension",
    "TW": "witness-table",
    "TI": "protocol-conformance",
}

SWIFT_PREFIXES = ("_$s", "$s", "_$S", "$S", "__swift_", "_T0", "_T")


@dataclass
class SwiftType:
    name: str
    kind: str
    address: int
    field_count: int = 0
    fields: list[str] = field(default_factory=list)
    superclass: str | None = None

    def report(self) -> dict:
        return {
            "name": self.name,
            "kind": self.kind,
            "address": f"0x{self.address:x}",
            "fieldCount": self.field_count,
            "fields": self.fields[:64],
            "superclass": self.superclass,
        }


@dataclass
class SwiftRuntime:
    types: list[SwiftType] = field(default_factory=list)
    protocols: list[str] = field(default_factory=list)
    conformances: int = 0
    sections: list[str] = field(default_factory=list)
    symbols: list[dict] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    @property
    def present(self) -> bool:
        return bool(self.types or self.protocols or self.sections)

    def report(self) -> dict:
        return {
            "present": self.present,
            "sections": self.sections,
            "typeCount": len(self.types),
            "protocolCount": len(self.protocols),
            "conformanceCount": self.conformances,
            "types": [t.report() for t in self.types[:200]],
            "protocols": self.protocols[:200],
            "symbols": self.symbols[:200],
            "symbolCount": len(self.symbols),
            "notes": self.notes,
        }


def _relative(image: MachOImage, address: int) -> int | None:
    raw = image.try_read(address, 4)
    if raw is None:
        return None
    delta = struct.unpack("<i" if image.little_endian else ">i", raw)[0]
    return address + delta


def _offsets(section, image: MachOImage) -> list[int]:
    data = image.section_bytes(section)
    values = []
    for i in range(0, len(data) - 3, 4):
        raw = int.from_bytes(data[i : i + 4], "little" if image.little_endian else "big", signed=True)
        if raw == -1 or raw == 0:
            continue
        values.append(section.address + i + raw)
    return values


def _plausible(text: str, limit: int = 256) -> bool:
    if not text or len(text) > limit:
        return False
    return all(ch.isprintable() and ch not in "\x7f" for ch in text)


def _fields(image: MachOImage, address: int) -> tuple[str | None, list[str], int]:
    """Parse a field descriptor: (mangled type name, field names, count)."""
    mangled = _relative(image, address)
    name = image.cstring(mangled) if mangled else None
    if not _plausible(name or ""):
        return None, [], 0
    kind = image.read_uint(address + 8, 2) or 0
    record_size = image.read_uint(address + 10, 2) or 12
    count = image.read_uint(address + 12) or 0
    if not count or count > 4096 or record_size < 12 or record_size > 64:
        return name, [], count
    if count * record_size > (1 << 20):
        return name, [], count
    fields = []
    for i in range(count):
        record = address + 16 + i * record_size
        field_name = _relative(image, record + 8)
        label = image.cstring(field_name) if field_name else None
        if _plausible(label or "", 128):
            fields.append(label)
    return name, fields, count


def recover(image: MachOImage) -> SwiftRuntime:
    """Recover Swift type/protocol metadata and mangle statistics."""
    runtime = SwiftRuntime()
    sections = [s for s in image.sections if s.name.startswith("__swift5")]
    runtime.sections = sorted({s.name for s in sections})
    if not sections:
        return runtime

    # __swift5_fieldmd holds field descriptors directly; type descriptors point at
    # them, so they are parsed on demand (see _field_descriptor below).
    field_by_name: dict[str, tuple[list[str], int]] = {}
    field_types = {s.name for s in image.sections if s.name == "__swift5_fieldmd"}

    for section in image.sections_named("__swift5_types"):
        for address in _offsets(section, image):
            flags = image.read_uint(address)
            if flags is None:
                continue
            kind = KINDS.get(flags & 0x1F, f"kind-{flags & 0x1F}")
            name_address = _relative(image, address + 8)
            raw_name = image.cstring(name_address) if name_address else None
            mangled = demangle(raw_name) if raw_name else None
            item = SwiftType(name=mangled or raw_name or "?", kind=kind, address=address)
            if kind == "class":
                superclass = _relative(image, address + 20)
                if superclass:
                    item.superclass = image.cstring(superclass)
            # TypeContextDescriptor: flags, parent, name, accessFunction, fields.
            field_pointer = _relative(image, address + 16)
            fields: list[str] = []
            count = 0
            if field_pointer is not None:
                name, fields, count = _fields(image, field_pointer)
                if name and name == raw_name:
                    field_by_name[name] = (fields, count)
            if not fields:
                fields, count = field_by_name.get(raw_name or "", ([], 0))
            item.fields = fields
            item.field_count = count
            runtime.types.append(item)

    for section in image.sections_named("__swift5_protos"):
        for address in _offsets(section, image):
            name_address = _relative(image, address + 8)
            raw_name = image.cstring(name_address) if name_address else None
            if raw_name:
                runtime.protocols.append(demangle(raw_name))
    for section in image.sections_named("__swift5_protocols"):
        runtime.conformances += len(_offsets(section, image))

    for symbol in image.symbols:
        name = symbol.name
        if not name or not any(name.startswith(p) for p in SWIFT_PREFIXES):
            continue
        runtime.symbols.append({"mangled": name, "demangled": demangle(name)})
    if runtime.symbols and not runtime.types:
        runtime.notes.append("Swift symbols present but no type metadata section was decoded")
    return runtime


def demangle(name: str) -> str:
    """Best-effort Swift demangling. Unresolved grammar is marked, not guessed."""
    text = name
    for prefix in ("__swift_", "_$s", "$s", "_$S", "$S", "_T0", "_T", "__T"):
        if text.startswith(prefix):
            text = text[len(prefix) :]
            break
    else:
        return name
    if not text:
        return name
    parts: list[str] = []
    index = 0
    length = len(text)
    while index < length:
        char = text[index]
        if char.isdigit():
            end = index
            while end < length and text[end].isdigit():
                end += 1
            size = int(text[index:end])
            if size == 0 or end + size > length:
                break
            parts.append(text[end : end + size])
            index = end + size
            continue
        if char == "S" and index + 1 < length:
            substitution = SUBSTITUTIONS.get(text[index + 1])
            if substitution is None:
                break
            parts.append(substitution)
            index += 2
            continue
        if char == "y":
            parts.append("()")
            index += 1
            continue
        if text[index : index + 2] in ENTITY:
            parts.append(f"<{ENTITY[text[index : index + 2]]}>")
            index += 2
            continue
        if char in ENTITY:
            parts.append(f"<{ENTITY[char]}>")
            index += 1
            continue
        break
    if not parts:
        return name
    resolved = ".".join(p for p in parts if not p.startswith("<"))
    tags = " ".join(p for p in parts if p.startswith("<"))
    if index < length:
        return f"{resolved}{(' ' + tags) if tags else ''} /* partial: {text[index:]} */"
    if not tags:
        # No entity kind (function, variable, metadata, ...) followed the name.
        return f"{resolved} /* partial: no entity kind */"
    return f"{resolved}{(' ' + tags) if tags else ''}"
