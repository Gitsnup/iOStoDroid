# Honest support matrix

Statuses describe implemented behavior, not planned compatibility.

| Area | State | Implemented boundary |
|---|---|---|
| Android library/import/details | SUPPORTED | Persistent metadata, real logs, bounded ZIP import and **on-device conversion** to a signed APK with real percentage progress |
| ZIP extraction | SUPPORTED | Traversal, symlinks/special files, collisions, encryption, limits, CRC; Android rejects ZIP64 explicitly |
| Info.plist | SUPPORTED | XML and binary, required bundle fields, nested icon dictionaries; bounded parsing |
| Icons (host converter) | PARTIAL | Generic fallback chain: Info.plist names -> `@2x`/`@3x`/`~ipad`/`~iphone` variants -> compiled `Assets.car` renditions -> icon-like bundle images -> any remaining image. Decodes ordinary PNG (colour types, bit depths, interlace, palettes, `tRNS`) and Apple `CgBI`; records source, format, resolution, scale and every fallback attempt; emits a square transparent Android launcher icon. HEIF/HEIC renditions are reported as undecodable instead of faked |
| Icons (on-device import) | SUPPORTED | Name/variant chain, bounded `Assets.car` payload scanning, Apple `CgBI` PNG repair, transparent-artwork rejection and a deterministic generated fallback: no library entry is ever blank |
| Mach-O thin/FAT/FAT64 | SUPPORTED | CPU/subtype, endian headers, bounded load command/section/symbol parsing |
| Mach-O loader metadata | PARTIAL | Relocations, dynamic table fields, bind symbols/offsets/addends, export trie, chained import/page-start records, dependencies, LC_MAIN, signature blob metadata. No signature trust validation or chained pointer graph rewriting |
| ObjC/Swift/unwind/init metadata | PARTIAL | Host reconstruction recovers ObjC classes/categories/protocols, ivars, properties, selectors (big and relative-small method lists), message-send selectors, class/super references and Swift type/field descriptors plus demangling. Section identification on device |
| ARM64 reconstruction | PARTIAL | Verified closed integer leaf subset; original safe instruction bytes retained in Android ELF |
| ARM64e | BLOCKED | PAC/ABI adaptation not proven |
| ARM32 (v4t/v5tej/**v6**/v7/v7s/armv8-32)/Thumb/Thumb-2 | PARTIAL | Explicit decoder → single basic-block IR → real ARM64 emitter: MOV/MVN, ADD/SUB register and immediate, AND/ORR/EOR/BIC, MUL, LSL/LSR/ASR, MOVW/MOVT. Additions outside this proven subset are rejected, never stubbed |
| Offline source reconstruction | PARTIAL | Function discovery, basic-block CFGs, ARM64/ARMv7/Thumb disassembly, register/constant/reference tracking and pseudocode listings with explicit `?` markers for anything unproven. Engineering artifact only, never presented as original source; reconstructed code is never executed |
| Branching CFG / loads / calls / atomics | BLOCKED | IR vocabulary exists; no verified general lowering; rejected rather than stubbed |
| Objective-C binary ABI | BLOCKED | Portable host runtime is experimental, not an Apple runtime provider |
| Framework dependency classification | SUPPORTED | Every dependency edge now names the Android provider that implements it (identical C ABI, platform API or generated runtime), its status and a reason, plus an honest provider-coverage percentage. Reachability analysis still splits reachable APIs into natively implementable, compatibility-required and genuinely unsupported |
| Foundation/CoreFoundation / UIKit / CoreGraphics | COMPATIBILITY | Mapped onto JVM/Platform APIs and the generated runtime: `dev.radek.runtime.Compat` (Choreographer, AudioTrack/AAudio, MediaPlayer, HttpURLConnection, Canvas/Matrix, SharedPreferences, CMTime and CGAffineTransform math) and `dev.radek.generated.MainActivity` as the converted entry activity |
| Darwin C/C++ ABI / exceptions / TLS | COMPATIBILITY | `libioscompat.so` provides message dispatch, class registration and selector interning on bionic; `libc++_shared.so` carries the C++ runtime. Exceptions and TLS across the ABI boundary remain unverified |
| Swift | BLOCKED | Metadata/dependencies are detected, not silently dropped |
| OpenGL ES / EGL | PROVIDED | Android ships the identical Khronos C ABI (`libGLESv2.so`, `libGLESv3.so`, `libEGL.so`, `libGLESv1_CM.so`), so gl*/egl* symbols bind directly |
| AudioToolbox/CoreAudio/AVFoundation/MediaPlayer/OpenAL | COMPATIBILITY | AudioQueue/AudioServices over AAudio/`AudioTrack`, ExtAudioFile over MediaExtractor, AVPlayer/AVAudioSession over MediaPlayer + AudioManager, al*/alc* over an AAudio mixer |
| Touch / keyboard / gamepad / sensors / iOS lifecycle | BLOCKED | The converted entry activity exists; iOS input and lifecycle mapping are not implemented |
| Resources | PARTIAL | Relative paths, localization, plist/JSON/audio/texture files preserved as opaque assets; icon-like PNGs normalized. Compiled asset catalogs are decoded for icon recovery only. No shader, texture/audio codec or lookup ABI conversion |
| APK packaging/signing | SUPPORTED | Host: real SDK/NDK, JNI ARM64 ELF, compiled manifest/resources/DEX/assets with a reusable dev key. Device: hand-built ELF64, binary AXML manifest, store/deflate ZIP, self-signed X.509 + PKCS#7 CERT.RSA, JAR (v1, `X-Android-APK-Signed: 2`) and APK Signature Scheme v2, verified with Android's own package parser before the APK is offered for install |
| APK static validation | SUPPORTED | ZIP/manifest/package/entry/DEX/signature/ELF architecture/dependencies/resources/icon/alignment/provenance checks, DEX checksum/launcher definition and native JNI entry/code hash |
| Android runtime/device validation | BLOCKED | No device test is currently supplied; report says NOT_TESTED |

## Reconstruction report

`python3 -m radek convert app.ipa --authorized --output workspace/result` writes, next to
`report.json`:

- `reconstruction.json` — machine-readable: architectures, entry points, per-image and
  per-slice metrics, recovered functions with listings, call graph, Objective-C
  classes/selectors/message sends, Swift types and symbols, imported symbols, linked
  frameworks, reachable API attribution and capability areas.
- `reconstruction.md` — the same evidence in readable form, including the reconstructed
  function listings and the blockers derived from reachable APIs.

Nothing in the reconstruction is executed, and no listing is a claim about the original
source: unproven instructions, types and control flow are marked explicitly.

## Closed integer entry contract v1

This is deliberately narrow and independent of bundle ID or game name:

- An unencrypted, little-endian MH_EXECUTE image; LC_MAIN identifies an executable `__text` entry.
- No embedded Mach-O images, Darwin imports, dylib dependencies, relocations, nonempty dyld binding/rebase streams, chained fixups, runtime metadata or unknown loader semantics.
- Code is proved linearly from the entry until an unconditional return, within 4096 instructions. No source byte is executed during conversion.
- Every read register must have been initialized within the function. No input arguments, memory references, SP/platform/callee-saved register writes, PAC, traps or syscalls are accepted.
- ARM64: 32-bit MOVZ/MOVK, unshifted immediate ADD/SUB and RET. Original instruction bytes are placed into an ELF exported JNI entry. NDK linking supplies Android load information; source addresses are unused because the accepted code has no address dependencies.
- ARM/Thumb: immediate MOV/ADD/SUB and BX LR; Thumb-2 MOVW/MOVT. Selected Thumb mode requires a matching symbol with `N_ARM_THUMB_DEF`. Decoded operations are emitted as real ARM64 instructions. Flags can be recorded but no accepted instruction reads condition flags.
- The function returns a 32-bit integer. The standalone Android launcher invokes it and displays the returned value. There is no loop dispatch/interpreter/translation runtime.
- Source bundle resources are included under `assets/bundle/` after excluding Mach-O executables and Apple signatures. This is preservation, **not** a claim that iOS filesystem/resource APIs work.

Ordinary iOS apps do not fit this contract and must remain BLOCKED. It would be misleading to label this implementation a completed general-purpose IPA converter.

## Experimental portable runtime

`native/include/runtime.hpp` implements class/metaclass registration, selector interning, IMP lookup, inheritance, invalidated dispatch caches, category replacement, ivar slots, property/protocol metadata, atomic retain/release, thread-local autorelease pools, data/string/array/dictionary values, notification delivery and isolated test storage. Tests exercise allocation, threading, TLS, exceptions and RTTI.

It **does not** implement Apple's object layout, metadata registration, tagged pointers, weak references, Objective-C exception ABI or variadic/aggregate `objc_msgSend`. It exports no fake Apple symbols and is never linked as a dependency provider for converted apps. Portable data types are not advertised as NSString/NSArray implementations.
