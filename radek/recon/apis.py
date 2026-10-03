"""Symbol attribution: which framework APIs are *actually* reachable.

The presence of a framework dependency alone is never treated as a blocker.
Symbols are attributed to a framework and an area, then classified:

``native``
    a direct Android/NDK equivalent exists (libc/libm/zlib, pthread, GLES/EGL)
``compatibility``
    a new offline compatibility implementation would be required
``blocked``
    no plausible mapping exists (Metal, Swift ABI, private Apple frameworks)
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

from .disasm import Function
from .image import MachOImage

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
    (re.compile(r"^_deflate|^_inflate|^_crc32|^_zlib|^_compress|^_uncompress|^_adler32"), "zlib", "compression", "native"),
    (re.compile(r"^_?sqlite3_"), "SQLite", "database", "native"),
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

    def report(self) -> dict:
        return {
            "name": self.name,
            "framework": self.framework,
            "area": self.area,
            "feasibility": self.feasibility,
            "kind": self.kind,
            "used": self.used,
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


def analyze(image: MachOImage, functions: list[Function]) -> dict:
    """Attribute every import, then compute which are actually reachable."""
    callers: dict[str, list[str]] = {}
    called_names: set[str] = set()
    for function in functions:
        for call in function.calls:
            called_names.add(call["name"])
            callers.setdefault(call["name"], [])
            if function.name not in callers[call["name"]]:
                callers[call["name"]].append(function.name)
        for instruction in function.instructions:
            if instruction.kind in ("loadlit", "adrp", "adr") and instruction.target is not None:
                continue

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
    for name, use in imports.items():
        if name in called_names:
            use.used = True
            use.callers = callers.get(name, [])
        elif name.lstrip("_") in {c.lstrip("_") for c in called_names}:
            use.used = True
            use.callers = callers.get(name, [])

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

    used = [u for u in imports.values() if u.used]
    unused = [u for u in imports.values() if not u.used]
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
        "unused": [u.name for u in unused[:400]],
        "capabilities": capabilities,
        "summary": {
            "native": len(by_feasibility.get("native", [])),
            "compatibility": len(by_feasibility.get("compatibility", [])),
            "blocked": len(by_feasibility.get("blocked", [])),
        },
    }
