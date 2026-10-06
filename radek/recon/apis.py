"""Attribute observed import callsites without overstating execution reachability.

The presence of a framework dependency alone is never treated as a blocker.
Symbols are attributed to a framework and an area, then classified:

``native``
    a direct Android/NDK equivalent is a candidate (libc/libm/zlib, pthread, GLES/EGL)
``compatibility``
    a compatibility implementation would be required
``blocked``
    no plausible mapping exists (Metal, Swift ABI, private Apple frameworks)

A decoded callsite is static evidence that a function references an import. It
is not proof that the callsite is reachable from the process entry or linked.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

from ..arch import is_arm32
from .disasm import Function, Instr
from .image import MachOImage
from .objc import selector_from_reference

FRAMEWORK_NAMES = {
    "Foundation": "Foundation",
    "UIKit": "UIKit",
    "CoreFoundation": "CoreFoundation",
    "CoreGraphics": "CoreGraphics",
    "QuartzCore": "QuartzCore",
    "CoreText": "CoreText",
    "CoreImage": "CoreImage",
    "CoreAnimation": "QuartzCore",
    "AVFoundation": "AVFoundation",
    "AudioToolbox": "AudioToolbox",
    "OpenAL": "OpenAL",
    "OpenGLES": "OpenGL ES",
    "Metal": "Metal",
    "MediaPlayer": "MediaPlayer",
    "CoreMedia": "CoreMedia",
    "CoreVideo": "CoreVideo",
    "CFNetwork": "CFNetwork",
    "GameKit": "GameKit",
    "GameController": "GameController",
    "CoreMotion": "CoreMotion",
    "SpriteKit": "SpriteKit",
    "SceneKit": "SceneKit",
    "GLKit": "GLKit",
    "Security": "Security",
    "StoreKit": "StoreKit",
    "SystemConfiguration": "SystemConfiguration",
    "CoreLocation": "CoreLocation",
    "MapKit": "MapKit",
    "WebKit": "WebKit",
    "libswift": "Swift runtime",
    "libobjc": "Objective-C runtime",
    "libSystem": "Darwin libc",
    "libdispatch": "libdispatch",
    "libc++": "C++ standard library",
    "libc++abi": "C++ ABI runtime",
    "libstdc++": "C++ standard library",
    "libgcc_s": "GCC/LLVM compiler runtime",
    # Common Darwin dylib install names in games. Naming them is attribution only:
    # each symbol still has to be classified and (if reachable) linked.
    "libz": "zlib",
    "libsqlite3": "SQLite",
    "libxml2": "libxml2",
    "libbz2": "libbz2",
    "libiconv": "iconv",
    "libresolv": "DNS resolver",
    "libicucore": "ICU",
    "libnetwork": "libnetwork",
    "libcompression": "libcompression",
    "libarchive": "libarchive",
    "JavaScriptCore": "JavaScriptCore",
    "CoreAudio": "AudioToolbox",
    "GameplayKit": "GameplayKit",
    "ReplayKit": "ReplayKit",
    "Photos": "Photos",
    "AssetsLibrary": "AssetsLibrary",
    "UserNotifications": "UserNotifications",
    "AdSupport": "AdSupport",
    "CoreData": "CoreData",
    "ImageIO": "ImageIO",
    "MobileCoreServices": "MobileCoreServices",
    "CoreTelephony": "CoreTelephony",
    "MessageUI": "MessageUI",
    "Social": "Social",
    "Accounts": "Accounts",
    "AddressBook": "AddressBook",
    "EventKit": "EventKit",
    "HealthKit": "HealthKit",
    "HomeKit": "HomeKit",
    "Intents": "Intents",
    "ARKit": "ARKit",
    "Vision": "Vision",
    "CoreML": "CoreML",
    "Network": "Network",
    "PDFKit": "PDFKit",
    "QuickLook": "QuickLook",
    "SafariServices": "SafariServices",
    "Twitter": "Twitter",
    "WatchConnectivity": "WatchConnectivity",
    "MultipeerConnectivity": "MultipeerConnectivity",
    "CoreBluetooth": "CoreBluetooth",
    "ExternalAccessory": "ExternalAccessory",
    "AVKit": "AVKit",
    "VideoToolbox": "VideoToolbox",
    "AudioUnit": "AudioToolbox",
    "CoreServices": "CoreServices",
    "Accelerate": "Accelerate",
    "ModelIO": "ModelIO",
    "MetalKit": "Metal",
    "PassKit": "PassKit",
    "LocalAuthentication": "LocalAuthentication",
    "UniformTypeIdentifiers": "UniformTypeIdentifiers",
    "FileProvider": "FileProvider",
    "BackgroundTasks": "BackgroundTasks",
    "DeviceCheck": "DeviceCheck",
    "AuthenticationServices": "AuthenticationServices",
}

C_RUNTIME = re.compile(
    r"^_?(malloc|calloc|realloc|free|memcpy|memset|memmove|memcmp|strlen|strcmp|strncmp|strcasecmp|strcpy"
    r"|strncpy|strcat|strdup|strstr|strchr|printf|fprintf|sprintf|snprintf|vsnprintf|puts|putchar|scanf|sscanf"
    r"|fopen|fclose|fread|fwrite|fseek|ftell|fgets|fputs|qsort|bsearch|abs|labs|llabs|rand|srand|time"
    r"|gettimeofday|clock|exit|abort|atoi|atof|atol|atoll|strtol|strtod|strtoul|strtoll|strtoull|sqrt|sqrtf"
    r"|sin|cos|tan|pow|powf|floor|ceil|fabs|log|log2|log10|exp|memchr|bcopy|bzero|longjmp|setjmp|isalpha"
    r"|isdigit|isspace|tolower|toupper|locale|setlocale)$"
)

RULES: list[tuple[re.Pattern[str], str, str, str]] = [
    (re.compile(r"^_objc_(msgSend|release|retain|autorelease|autoreleasePool|load|getClass|lookUpImp)"), "Objective-C runtime", "objc", "compatibility"),
    (re.compile(r"^__objc_"), "Objective-C runtime", "objc", "compatibility"),
    (re.compile(r"^_object_"), "Objective-C runtime", "objc", "compatibility"),
    (re.compile(r"^_OBJC_"), "Objective-C runtime", "objc", "compatibility"),
    (re.compile(r"^_gl[A-Z]|^_glu[A-Z]"), "OpenGL ES", "graphics", "native"),
    (re.compile(r"^_egl[A-Z]"), "EGL", "graphics", "native"),
    (re.compile(r"^_MTL|^_MT[A-Z]|^_metal_"), "Metal", "graphics", "blocked"),
    (re.compile(r"^_alc?[A-Z]|^_al[A-Z]|^_alut"), "OpenAL", "audio", "compatibility"),
    (re.compile(r"^_?Audio[A-Z]|^_AudioQueue|^_AudioUnit|^_AudioComponent|^_ExtAudioFile"), "AudioToolbox", "audio", "compatibility"),
    (re.compile(r"^_AV[A-Z]"), "AVFoundation", "audio/video", "compatibility"),
    (re.compile(r"^_NS[A-Z]"), "Foundation", "foundation", "compatibility"),
    (re.compile(r"^_UI[A-Z]|^_UIApplicationMain|^_UIGesture"), "UIKit", "ui", "compatibility"),
    (re.compile(r"^_CG[A-Z]|^_CGBitmap|^_CGColor"), "CoreGraphics", "graphics", "compatibility"),
    (re.compile(r"^_CF[A-Z]|^_CFRunLoop|^_CFString"), "CoreFoundation", "foundation", "compatibility"),
    (re.compile(r"^_CA[A-Z]|^_CATrans"), "QuartzCore", "graphics", "compatibility"),
    (re.compile(r"^_SC[A-Z]|^_SK[A-Z]"), "StoreKit/SpriteKit", "ui", "compatibility"),
    (re.compile(r"^_GK[A-Z]"), "GameKit", "input/social", "compatibility"),
    (re.compile(r"^_GC[A-Z]"), "GameController", "input", "compatibility"),
    (re.compile(r"^_CM[A-Z]"), "CoreMotion/CoreMedia", "sensors", "compatibility"),
    (re.compile(r"^_MP[A-Z]|^_MPMovie"), "MediaPlayer", "audio/video", "compatibility"),
    (re.compile(r"^_Sec[A-Z]|^_SSL[A-Z]|^_CC[A-Z]|^_kSec"), "Security", "crypto", "compatibility"),
    (re.compile(r"^_dispatch_"), "libdispatch", "concurrency", "native"),
    (re.compile(r"^_pthread|^_?pthread_"), "pthread", "concurrency", "native"),
    # Apple's libcompression API starts with `compression_`, which the zlib rule
    # below must not swallow; it has no NDK drop-in, so it stays a compatibility
    # candidate rather than being reported as native zlib.
    (re.compile(r"^_?compression_"), "libcompression", "compression", "compatibility"),
    (re.compile(r"^_deflate|^_inflate|^_crc32|^_zlib|^_compress|^_uncompress|^_adler32|^_gz"), "zlib", "compression", "native"),
    (re.compile(r"^_?sqlite3_"), "SQLite", "database", "native"),
    (re.compile(r"^_?iconv|^_?libiconv"), "iconv", "text", "compatibility"),
    (re.compile(r"^_?BZ2_|^_?bz(?:read|write|flush|close|open)"), "libbz2", "compression", "compatibility"),
    (re.compile(r"^_?archive_"), "libarchive", "filesystem", "compatibility"),
    (re.compile(r"^_?xml[A-Z_]|^_?html[A-Z]|^_?xpath_"), "libxml2", "text", "compatibility"),
    (re.compile(r"^_?res_9_|^_?dn_expand|^_?ns_parse|^_?res_query"), "DNS resolver", "network", "native"),
    (re.compile(r"^_?ucnv_|^_?u_str|^_?ubrk_|^_?uloc_|^_?unorm_"), "ICU", "text", "compatibility"),
    (re.compile(r"^_JS[A-Z]|^_?jsc_"), "JavaScriptCore", "language", "blocked"),
    (re.compile(r"^_?nw_[a-z]"), "libnetwork", "network", "compatibility"),
    (re.compile(r"^_{1,3}(?:Unwind_|gcc_personality_v0|gxx_personality_v0|aeabi_unwind_|gnu_unwind_|[u]?divdi3|[u]?moddi3|muldi3|ashldi3|ashrdi3|lshrdi3|udivmoddi4|aeabi_|[u]?divti3|[u]?modti3|multi3|muloti4|ash[lr]ti3|addvti3|subvti3|absvti2|cmpdi2|ucmpdi2|clear_cache|register_frame|deregister_frame)"), "GCC/LLVM compiler runtime", "language", "compatibility"),
    (re.compile(r"^__Z|std::"), "C++ standard library", "language", "compatibility"),
    (re.compile(r"^_swift_|^\$s|^\$S|^_?_swift"), "Swift runtime", "language", "blocked"),
    (C_RUNTIME, "Darwin libc/libm", "libc", "native"),
    (re.compile(r"^__NS|^\+?\["), "Objective-C", "objc", "compatibility"),
]

AREA_CAPABILITY = {
    "graphics": "graphics",
    "audio": "audio",
    "audio/video": "audio+video",
    "video": "video",
    "ui": "user interface",
    "input": "input",
    "input/social": "input",
    "sensors": "sensors",
    "network": "network",
    "filesystem": "filesystem",
    "crypto": "cryptography",
    "foundation": "core services",
    "objc": "Objective-C runtime",
    "concurrency": "threads",
    "compression": "compression",
    "database": "database",
    "text": "text/encoding",
    "libc": "C runtime",
    "language": "language runtime",
}


@dataclass
class SymbolUse:
    name: str
    framework: str
    area: str
    feasibility: str
    kind: str
    used: bool = False
    callers: list[str] = field(default_factory=list)
    call_sites: list[dict] = field(default_factory=list)
    relocation_sites: list[str] = field(default_factory=list)

    def report(self) -> dict:
        return {
            "name": self.name,
            "framework": self.framework,
            "area": self.area,
            "feasibility": self.feasibility,
            "kind": self.kind,
            "used": self.used,
            "callSiteCount": len(self.call_sites),
            "callSites": self.call_sites[:16],
            "relocationSiteCount": len(self.relocation_sites),
            "relocationSites": self.relocation_sites[:16],
            "callers": self.callers[:16],
        }


def framework_of(path: str) -> str:
    name = path.rsplit("/", 1)[-1]
    stem = name.split(".", 1)[0]
    return FRAMEWORK_NAMES.get(stem, stem)


def classify(name: str) -> tuple[str, str, str, str]:
    """Return (framework, area, feasibility, kind) for an imported symbol."""
    if name.startswith("_OBJC_CLASS_$_") or name.startswith("_OBJC_METACLASS_$_"):
        return "Objective-C class", "objc", "compatibility", "class"
    if name.startswith("_OBJC_IVAR_$_"):
        return "Objective-C ivar", "objc", "compatibility", "ivar"
    for pattern, framework, area, feasibility in RULES:
        if pattern.search(name):
            return framework, area, feasibility, "function"
    return "unattributed", "unknown", "compatibility", "function"


def _kind(name: str) -> str:
    if name.startswith("_OBJC_CLASS_$_"):
        return "class"
    if name.startswith("_OBJC_METACLASS_$_"):
        return "metaclass"
    if name.startswith("_OBJC_IVAR_$_"):
        return "ivar"
    return "function"


def _mapped_address(image: MachOImage, address: int) -> bool:
    return any(
        section.size and section.address <= address < section.address + section.size
        for section in image.sections
    )


def _external_relocations(image: MachOImage) -> dict[int, str]:
    """Map static external relocation slots to their imported symbol names."""
    result: dict[int, str] = {}
    dynamic = image.slice.get("dynamicSymbols") or {}
    for relocation in dynamic.get("externalRelocations", []) or []:
        slot = relocation.get("address")
        index = relocation.get("symbolIndex")
        if not isinstance(slot, int) or not isinstance(index, int):
            continue
        if 0 <= index < len(image.symbols) and image.symbols[index].name:
            result[slot] = image.symbols[index].name
    return result


def _call_arguments(image: MachOImage, function: Function, call_address: int) -> dict:
    """Resolve simple ARM32 register arguments at a direct callsite, statically."""
    if not is_arm32(image.architecture):
        return {
            "r0": None,
            "r1": None,
            "evidence": "Argument analysis is implemented only for ARM32 AAPCS callsites.",
        }
    relocations = _external_relocations(image)
    registers: dict[int, dict] = {}

    def value_for_register(register: int, instruction: Instr) -> dict | None:
        if register == 15 and image.architecture.startswith("arm"):
            return {"kind": "constant", "value": instruction.address + 8}
        return registers.get(register)

    def address_or_constant(value: int) -> dict:
        return {"kind": "address" if _mapped_address(image, value) else "constant", "value": value}

    def describe(value: dict | None) -> dict | None:
        if not value:
            return None
        kind = value.get("kind")
        if kind == "selector":
            return {"kind": "selector", "name": value.get("name"), "slot": f"0x{value['slot']:x}"}
        if kind == "symbol":
            return {"kind": "imported-symbol", "name": value.get("name"), "slot": f"0x{value['slot']:x}"}
        if kind == "address":
            address = value.get("value")
            section = next(
                (
                    item
                    for item in image.sections
                    if item.size and item.address <= address < item.address + item.size
                ),
                None,
            )
            return {
                "kind": "address",
                "address": f"0x{address:x}",
                "section": f"{section.segment},{section.name}" if section else None,
            }
        if kind == "constant":
            return {"kind": "constant", "value": value.get("value")}
        return {"kind": kind}

    for instruction in sorted(function.instructions, key=lambda item: item.address):
        if instruction.address == call_address:
            return {
                "r0": describe(registers.get(0)),
                "r1": describe(registers.get(1)),
                "evidence": "Static register flow from ARM instructions and Mach-O literals/relocations; not execution evidence.",
            }

        dst = instruction.dst
        if instruction.kind == "loadlit" and dst is not None and instruction.target is not None:
            literal = image.read_pointer(instruction.target)
            registers[dst] = address_or_constant(literal) if literal is not None else {"kind": "unknown"}
        elif instruction.kind == "load" and dst is not None and instruction.sources:
            base = value_for_register(instruction.sources[0], instruction)
            if base and base.get("kind") == "address":
                slot = int(base["value"]) + (instruction.immediate or 0)
                if slot in relocations:
                    registers[dst] = {"kind": "symbol", "name": relocations[slot], "slot": slot}
                elif any(
                    section.name == "__objc_selrefs"
                    and section.address <= slot < section.address + section.size
                    for section in image.sections
                ):
                    registers[dst] = {
                        "kind": "selector",
                        "name": selector_from_reference(image, slot),
                        "slot": slot,
                    }
                else:
                    pointer = image.read_pointer(slot)
                    registers[dst] = address_or_constant(pointer) if pointer is not None else {"kind": "unknown"}
            else:
                registers.pop(dst, None)
        elif instruction.kind == "move" and dst is not None:
            if instruction.immediate is not None:
                registers[dst] = {"kind": "constant", "value": instruction.immediate}
            elif instruction.sources:
                source = value_for_register(instruction.sources[0], instruction)
                if source is None:
                    registers.pop(dst, None)
                else:
                    registers[dst] = dict(source)
            else:
                registers.pop(dst, None)
        elif instruction.kind == "arith" and dst is not None and instruction.sources:
            name = instruction.mnemonic.lower()
            left = value_for_register(instruction.sources[0], instruction)
            result = None
            if name in ("add", "sub"):
                if instruction.immediate is not None and left:
                    delta = instruction.immediate if name == "add" else -instruction.immediate
                    result = address_or_constant(int(left.get("value", 0)) + delta)
                elif len(instruction.sources) > 1:
                    right = value_for_register(instruction.sources[1], instruction)
                    if left and right and left.get("kind") in ("address", "constant") and right.get("kind") in ("address", "constant"):
                        amount = int(right.get("value", 0))
                        result = address_or_constant(int(left.get("value", 0)) + (amount if name == "add" else -amount))
            if result is None:
                registers.pop(dst, None)
            else:
                registers[dst] = result
        elif dst is not None:
            registers.pop(dst, None)

        if instruction.kind == "call":
            # A32 callers may clobber r0-r3. This prevents stale arguments from
            # being mistaken for values at a later import call.
            for register in range(4):
                registers.pop(register, None)

    return {"r0": None, "r1": None, "evidence": "Callsite arguments could not be resolved statically."}


def _entry_reachability(image: MachOImage, functions: list[Function], imports: dict[str, SymbolUse]) -> dict:
    """Follow only recovered direct calls from the selected Mach-O entry."""
    if image.entry is None:
        return {
            "status": "unavailable",
            "entryFunction": None,
            "functionCount": 0,
            "importCount": 0,
            "functions": [],
            "imports": [],
            "note": "No static Mach-O entry address was recovered.",
        }

    by_address = {function.address & ~1: function for function in functions}
    by_instruction: dict[int, Function] = {}
    for function in functions:
        for instruction in function.instructions:
            by_instruction[instruction.address & ~1] = function
    entry_address = image.entry & ~1
    entry_function = by_address.get(entry_address) or by_instruction.get(entry_address)
    if entry_function is None:
        return {
            "status": "unavailable",
            "entryFunction": f"0x{image.entry:x}",
            "functionCount": 0,
            "importCount": 0,
            "functions": [],
            "imports": [],
            "note": "The entry address is not covered by a reconstructed function.",
        }

    names_by_normalized = {name.lstrip("_"): name for name in imports}
    reachable = {entry_function.address & ~1: entry_function}
    pending = [entry_function]
    observed_sites: dict[str, list[dict]] = {}
    while pending:
        caller = pending.pop()
        for call in caller.calls:
            call_name = call.get("name", "")
            imported_name = call_name if call_name in imports else names_by_normalized.get(call_name.lstrip("_"))
            if imported_name:
                observed_sites.setdefault(imported_name, []).append(
                    {
                        "caller": caller.name,
                        "callerAddress": f"0x{caller.address:x}",
                        "address": call.get("from"),
                        "target": call.get("target"),
                    }
                )
                continue
            try:
                target_address = int(call.get("target", "0"), 0) & ~1
            except (TypeError, ValueError):
                continue
            callee = by_address.get(target_address) or by_instruction.get(target_address)
            if callee is None or (callee.address & ~1) in reachable:
                continue
            reachable[callee.address & ~1] = callee
            pending.append(callee)

    reachable_imports = []
    for name, sites in observed_sites.items():
        use = imports[name]
        reachable_imports.append(
            {
                "name": name,
                "framework": use.framework,
                "area": use.area,
                "feasibility": use.feasibility,
                "callSiteCount": len(sites),
                "callSites": sorted(sites, key=lambda site: int(site.get("address", "0"), 0))[:16],
            }
        )
    reachable_imports.sort(key=lambda item: (-item["callSiteCount"], item["name"]))
    return {
        "status": "direct-call-graph-only",
        "entryFunction": {"name": entry_function.name, "address": f"0x{entry_function.address:x}"},
        "functionCount": len(reachable),
        "importCount": len(reachable_imports),
        "functions": [
            {"name": function.name, "address": f"0x{function.address:x}"}
            for function in sorted(reachable.values(), key=lambda item: item.address)[:256]
        ],
        "imports": reachable_imports[:400],
        "note": (
            "Only recovered direct calls are followed. Indirect calls, dyld transfers, Objective-C selector dispatch, "
            "callbacks, and loader initializers are not inferred; an empty result is not proof that the app makes no calls."
        ),
    }


def _entry_import_trace(image: MachOImage, functions: list[Function], imports: dict[str, SymbolUse]) -> dict:
    """Describe the first direct import call in the app's _main, not all-code reachability."""
    application_entry = next((f for f in functions if f.name in ("_main", "main")), None)
    if application_entry is None:
        return {"status": "unavailable", "reason": "No _main/main symbol was reconstructed."}

    imported_names = set(imports)
    candidates = [
        call
        for call in application_entry.calls
        if call.get("name") in imported_names
        or call.get("name", "").lstrip("_") in {name.lstrip("_") for name in imported_names}
    ]
    candidates.sort(key=lambda call: int(call.get("from", "0"), 0))
    first = candidates[0] if candidates else None

    entry = image.entry
    entry_function = next((f for f in functions if f.address == entry), None) if entry is not None else None
    indirect_transfers = []
    if entry_function:
        indirect_transfers = [
            {
                "address": f"0x{instruction.address:x}",
                "instruction": instruction.text,
                "kind": instruction.kind,
            }
            for instruction in entry_function.instructions
            if instruction.kind in ("call", "branch") and instruction.target is None
        ][:16]

    first_call = None
    if first:
        call_address = int(first.get("from", "0"), 0)
        first_call = {
            "symbol": first.get("name"),
            "caller": application_entry.name,
            "callsite": first.get("from"),
            "stubAddress": first.get("target"),
            "kind": "direct decoded call to a symbol stub",
            "arguments": _call_arguments(image, application_entry, call_address),
        }

    trace = {
        "status": "partial" if entry is not None and entry != application_entry.address else "static-entry-match",
        "imageEntry": {
            "symbol": image.name_of(entry) if entry is not None else None,
            "address": f"0x{entry:x}" if entry is not None else None,
        },
        "applicationEntry": {
            "symbol": application_entry.name,
            "address": f"0x{application_entry.address:x}",
        },
        "entryToApplicationEntry": {
            "status": "dyld-transfer-not-statically-resolved" if entry is not None and entry != application_entry.address else "same-address",
            "indirectTransfersAtImageEntry": indirect_transfers,
            "note": (
                "The Mach-O entry uses indirect transfers whose dyld-resolved targets are outside the static image; "
                "the handoff to _main is not claimed as a recovered direct branch."
                if entry is not None and entry != application_entry.address
                else "The selected image entry and _main address match."
            ),
        },
        "firstApplicationImportCall": first_call,
        "subsequentImportCalls": [
            {
                "symbol": call.get("name"),
                "caller": application_entry.name,
                "callsite": call.get("from"),
                "stubAddress": call.get("target"),
            }
            for call in candidates[1:9]
        ],
        "scopeNote": "The import call is statically present in _main; indirect runtime reachability and symbol execution are not proven.",
    }
    return trace


def analyze(image: MachOImage, functions: list[Function]) -> dict:
    """Attribute imports to deduplicated static callsites; do not imply entry reachability."""
    call_sites: dict[str, list[dict]] = {}
    seen_sites: set[tuple[str, str, str, str]] = set()
    for function in functions:
        for call in function.calls:
            name = call.get("name", "")
            site_key = (function.name, str(call.get("from")), str(call.get("target")), name)
            if site_key in seen_sites:
                continue
            seen_sites.add(site_key)
            call_sites.setdefault(name, []).append(
                {
                    "caller": function.name,
                    "callerAddress": f"0x{function.address:x}",
                    "address": call.get("from"),
                    "target": call.get("target"),
                }
            )

    imports: dict[str, SymbolUse] = {}
    for entry in image.imports:
        name = entry.get("name")
        if not name:
            continue
        if name in imports:
            continue
        framework, area, feasibility, _ = classify(name)
        imports[name] = SymbolUse(
            name=name,
            framework=framework,
            area=area,
            feasibility=feasibility,
            kind=_kind(name),
        )
    for symbol in image.symbols:
        if not symbol.undefined or not symbol.name:
            continue
        if symbol.name in imports:
            continue
        framework, area, feasibility, _ = classify(symbol.name)
        imports[symbol.name] = SymbolUse(
            name=symbol.name,
            framework=framework,
            area=area,
            feasibility=feasibility,
            kind=_kind(symbol.name),
        )
    relocation_sites: dict[str, list[str]] = {}
    for address, name in _external_relocations(image).items():
        relocation_sites.setdefault(name, []).append(f"0x{address:x}")

    for name, use in imports.items():
        matching_name = name if name in call_sites else next(
            (called for called in call_sites if called.lstrip("_") == name.lstrip("_")),
            None,
        )
        use.call_sites = sorted(
            call_sites.get(matching_name, []),
            key=lambda site: int(site.get("address", "0"), 0),
        ) if matching_name else []
        use.used = bool(use.call_sites)
        use.callers = list(dict.fromkeys(site["caller"] for site in use.call_sites))
        use.relocation_sites = sorted(relocation_sites.get(name, []), key=lambda address: int(address, 0))

    linked = {}
    for dependency in image.slice.get("dependencies", []):
        path = dependency.get("path", "")
        name = framework_of(path)
        linked[name] = {
            "installName": path,
            "command": dependency.get("command"),
            "weak": dependency.get("command") in (0x18, 0x80000018, 0x8000001F, 0x20),
            "symbolsUsed": 0,
        }
    for use in imports.values():
        if not use.used:
            continue
        for name, entry in linked.items():
            if name.lower() in use.framework.lower() or use.framework.lower() in name.lower():
                entry["symbolsUsed"] += 1

    used = sorted(
        (u for u in imports.values() if u.used),
        key=lambda use: (-len(use.call_sites), use.name),
    )
    unused = [u for u in imports.values() if not u.used]
    ranked_imports = []
    for rank, use in enumerate(used, start=1):
        item = use.report()
        item["rank"] = rank
        ranked_imports.append(item)
    entry_trace = _entry_import_trace(image, functions, imports)
    entry_reachability = _entry_reachability(image, functions, imports)
    import_details = sorted(
        imports.values(),
        key=lambda use: (-len(use.call_sites), -len(use.relocation_sites), use.name),
    )
    by_feasibility = {"native": [], "compatibility": [], "blocked": []}
    for use in used:
        by_feasibility.setdefault(use.feasibility, []).append(use.name)

    capabilities: dict[str, dict] = {}
    for use in used:
        capability = AREA_CAPABILITY.get(use.area, use.area)
        entry = capabilities.setdefault(
            capability,
            {
                "status": "BLOCKED",
                "symbols": [],
                "frameworks": [],
                "nativeEquivalent": True,
            },
        )
        if len(entry["symbols"]) < 40:
            entry["symbols"].append(use.name)
        if use.framework not in entry["frameworks"]:
            entry["frameworks"].append(use.framework)
        if use.feasibility != "native":
            entry["nativeEquivalent"] = False
        entry["status"] = "NATIVE" if entry["nativeEquivalent"] else "BLOCKED"

    return {
        "linkedFrameworks": linked,
        "importCount": len(imports),
        "usedImportCount": len(used),
        "unusedImportCount": len(unused),
        "byFeasibility": {k: v[:200] for k, v in by_feasibility.items()},
        "used": [u.report() for u in used[:400]],
        "rankedImports": ranked_imports[:400],
        "importDetails": [use.report() for use in import_details[:1024]],
        "unused": [u.name for u in unused[:400]],
        "entryImportTrace": entry_trace,
        "entryReachability": entry_reachability,
        "capabilities": capabilities,
        "summary": {
            "native": len(by_feasibility.get("native", [])),
            "compatibility": len(by_feasibility.get("compatibility", [])),
            "blocked": len(by_feasibility.get("blocked", [])),
        },
    }
