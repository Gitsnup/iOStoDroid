# Honest support matrix

Statuses describe implemented behavior, not planned compatibility.

| Area | State | Implemented boundary |
|---|---|---|
| Android library/import/details | PARTIAL | Persistent metadata, real logs and bounded ZIP import; host-only compilation |
| ZIP extraction | SUPPORTED | Traversal, symlinks/special files, collisions, encryption, limits, CRC; Android rejects ZIP64 explicitly |
| Info.plist | SUPPORTED | XML and binary, required bundle fields, nested icon dictionaries; bounded parsing |
| Icons | PARTIAL | Loose PNGs; host normalizes non-interlaced RGBA8 CgBI. Android standard decoder only. No Assets.car decoder |
| Mach-O thin/FAT/FAT64 | SUPPORTED | CPU/subtype, endian headers, bounded load command/section/symbol parsing |
| Mach-O loader metadata | PARTIAL | Relocations, dynamic table fields, bind symbols, export trie, chained import records, dependencies, LC_MAIN, signature blob metadata. No signature trust validation or chained pointer graph rewriting |
| ObjC/Swift/unwind/init metadata | PARTIAL | Section identification/ranges only, not full Apple metadata graph reconstruction |
| ARM64 reconstruction | PARTIAL | Verified closed integer leaf subset; original safe instruction bytes retained in Android ELF |
| ARM64e | BLOCKED | PAC/ABI adaptation not proven |
| ARMv7/v7s/Thumb/Thumb-2 | PARTIAL | Explicit decoder → single basic-block IR → ARM64 emitter; limited immediate arithmetic and return |
| Branching CFG / loads / calls / atomics | BLOCKED | IR vocabulary exists; no verified general lowering; rejected rather than stubbed |
| Objective-C binary ABI | BLOCKED | Portable host runtime is experimental, not an Apple runtime provider |
| Foundation/CoreFoundation / UIKit / CoreGraphics | BLOCKED | No converted-app API providers |
| Darwin C/C++ ABI / exceptions / TLS | BLOCKED | Host tests verify portable C++ mechanics only, not Darwin-to-Bionic adaptation |
| Swift | BLOCKED | Metadata/dependencies are detected, not silently dropped |
| EAGL/OpenGL ES, Metal/Vulkan | BLOCKED | No compatibility renderer implemented |
| AudioToolbox/AVFoundation/OpenAL | BLOCKED | No audio API mapping implemented |
| Touch / keyboard / gamepad / sensors / iOS lifecycle | BLOCKED | Android entry activity exists, not iOS input/lifecycle mapping |
| Resources | PARTIAL | Relative paths, localization, plist/JSON/audio/texture files preserved as opaque assets. No compiled asset catalog, shader, texture/audio codec or lookup ABI conversion |
| APK packaging/signing | SUPPORTED | Real SDK/NDK, JNI ARM64 ELF, compiled manifest/resources/DEX/assets, reusable dev key |
| APK static validation | SUPPORTED | ZIP/manifest/package/entry/DEX/signature/ELF architecture/dependencies/resources/icon/alignment/provenance checks |
| Android runtime/device validation | BLOCKED | No device test is currently supplied; report says NOT_TESTED |

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
