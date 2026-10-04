# Support matrix and conversion contract

“Supported” refers only to the operation listed. Inspection or candidate analysis is not evidence
that an iOS app can be converted. The repository currently has no complete iOS-to-Android game
translator and emits no game APK.

| Area | Status | Contract / limitation |
|---|---|---|
| IPA archive, plist, icon inspection | PARTIAL | Bounded, authorized, offline inspection; source retained in Android private storage until entry deletion. No original IPA is embedded in any APK |
| Icons (host inspection) | PARTIAL | Info.plist names, scale/device variants, compiled `Assets.car` raster renditions, then ranked loose images; unsupported formats are reported, not fabricated |
| Icons (Android library) | PARTIAL | Plist/scale variants, supported compiled `Assets.car` raster renditions, then ranked PNG/JPEG resources. Recovered icon is shown in the library; no icon-branded placeholder is built |
| Mach-O thin/FAT/FAT64 | SUPPORTED | CPU/subtype, endian headers, bounded load-command/section/symbol parsing |
| Mach-O loader metadata | PARTIAL | Relocations, dynamic tables, binds/imports/addends, export trie, chained-fixup metadata, dependencies, LC_MAIN and signature-blob metadata. Incomplete bind tables and unsupported loader semantics block conversion |
| ObjC/Swift/unwind/init metadata | PARTIAL | Host reconstruction recovers selected Objective-C/Swift metadata and reports limitations; it does not implement the Apple runtime ABI |
| ARM64 reconstruction | PARTIAL | A restricted closed integer leaf can be assessed/lowered in memory. No resulting game code or APK is emitted |
| ARM64e | BLOCKED | PAC/ABI adaptation is not proven |
| ARMv6/ARMv7/v7s/Thumb/Thumb-2 | PARTIAL | Selected immediate arithmetic/return instruction subsets can be lowered in memory to ARMv7 form. This does not make a game executable; no 32-bit APK is emitted |
| FAT ARM64 + ARM32 selection | SUPPORTED (selection only) | Automatic selection prefers ARM64. Explicit 32-bit target is accepted only when an ARM32 slice is present |
| Supported ARM32-only target | SUPPORTED (selection only) | Selects `armeabi-v7a`; no complete converter currently emits an APK |
| Offline source reconstruction | PARTIAL | Function discovery, CFGs, selected ARM disassembly, reference tracking and pseudocode with explicit uncertainty. Never presented as original source or executed |
| Actual iOS-to-Android API replacements | NOT IMPLEMENTED | Same-name NDK symbols and semantic targets are candidates only. No replacement implementation is generated, linked or tested |
| Objective-C binary ABI / Swift | BLOCKED | The experimental portable runtime is not Apple's ABI/runtime and is not linked into game outputs |
| UIKit, Foundation, graphics, audio, input, lifecycle | BLOCKED | No complete compatibility providers or game lifecycle/input translations exist |
| Resources | PARTIAL | Icons and bundle resources can be inventoried/read for analysis. There is no game APK into which they are packaged |
| Importer APK | SUPPORTED | Gradle builds the Android library/import/analyzer app for ARM64 devices |
| Game APK packaging | DISABLED | The former closed-integer launcher wrapper was a partial test artifact, not a complete game. `build_apk` refuses to emit it; no placeholder/stub path remains |
| Host APK attachment | CONTRACT-ONLY | The app accepts only `complete-game-v1` evidence with source/ABI/API/resource/lifecycle checks. No current repository converter produces this contract |
| APK static validation | SUPPORTED | The validator checks the importer APK; game APK validation additionally requires the complete-game contract, generated API implementation evidence, provenance and ABI checks |
| Android runtime/device validation | NOT TESTED | Static checks cannot prove execution or gameplay; reports say `NOT_TESTED` |

## Reconstruction report

`python3 -m radek analyze app.ipa --authorized --output workspace/analysis` writes, next to
`report.json`:

- `reconstruction.json` — machine-readable architectures, entry points, per-image/per-slice
  metrics, recovered functions and listings, call graph, Objective-C metadata, Swift types/symbols,
  imports, linked frameworks and reachable API attribution.
- `reconstruction.md` — readable evidence, reconstructed listings and reachable-API blockers.

The report may record an in-memory experimental leaf lowering, but marks it as an assessment only;
`portProgress` remains zero and `conversionProgress.status` remains `NOT_BUILT`. A candidate symbol
mapping is never counted as generated code.

## ABI preference

- With automatic selection, a FAT IPA containing ARM32 and ARM64 prefers the ARM64 slice and the
  future target ABI `arm64-v8a`.
- Supported ARM32-only input selects an ARMv6/ARMv7-family slice for the future 32-bit ABI
  `armeabi-v7a`.
- An explicit ABI must have a matching source slice. Target selection is not evidence of successful
  translation or APK output.

## Complete-game attachment contract

The `complete-game-v1` attachment gate rejects the old `closed-integer-entry-v1` output. It requires
host evidence for full reachable-function translation, complete reachable API accounting and
linked implementation artifacts, complete resource/lifecycle claims, ABI/package/source identity,
original icon hash when recovered, APK signature/package parsing, and exclusion of the original IPA
and Apple executable assets. This metadata is not a substitute for behavioral testing. The current
host pipeline has no producer for this contract, so no game APK can be attached from repository
outputs today.
