# RadekiOSConventor

An **offline native-reconstruction workbench**, not an iOS emulator. Kotlin Android importer + C++ Mach-O analyzer + Python host conversion/SDK packaging pipeline.

> **Important: this is NOT a general IPA/game converter.** Current native conversion is restricted to self-contained, straight-line integer-returning entry code with no reachable imports/framework calls, memory access, address references or unsupported runtime semantics. Linked-but-unused dylib/import records can be omitted only after that entry is proven; no no-op framework stubs are generated. UIKit, Foundation ABI, Swift, general Objective-C, graphics/audio/input and ordinary commercial apps are **BLOCKED** when required. An importer APK is not proof those apps can be converted.

## Offline reconstruction before conversion

Every import is analyzed before any conversion decision is made:

- Mach-O images (executable, embedded dylibs/frameworks): headers, load commands,
  segments/sections, symbols, relocations, exports/imports, dependencies and fixups.
- Disassembly and function discovery with basic-block CFGs (ARM64/ARM64e, ARMv6/ARMv7/Thumb/Thumb-2),
  register/constant/reference tracking and pseudocode listings.
- Objective-C classes, categories, protocols, ivars, properties, selectors, message-send
  targets; Swift type/field metadata and demangling.
- Reachability: which imported symbols are actually called, by which reconstructed functions,
  which linked frameworks are weak/optional, and which APIs are natively implementable on
  Android, need compatibility code, or are genuinely unsupported.

Results are written as `reconstruction.json` and `reconstruction.md` beside `report.json`.
A dependency that is merely linked is **never** reported as blocked: only reachable APIs are.
The reconstruction is an engineering artifact and is never claimed to be original source;
reconstructed code is never executed.

## APK outputs

1. **Converter/importer app:** the Actions artifact `RadekiOSConventor-debug.apk` contains the Android library/import UI and native analyzer. It has no on-device SDK/NDK or general game-conversion backend.
2. **Standalone converted program (host-only, narrow subset):** `python3 -m radek convert <game.ipa> ...` produces `<game>.apk` with a DEX launcher and the selected native ABI. ARM64 inputs produce `arm64-v8a`; proven ARMv6/ARMv7 leaf inputs produce a 32-bit `armeabi-v7a` APK. It preserves the icon and bounded bundle resources, but does **not** recreate a game's UI or general gameplay.
3. **On-device game-icon APK:** each imported IPA automatically attempts to produce a signed `<ipa-basename>.apk`. Its Android launcher icon is replaced with the recovered IPA icon. The stub includes metadata and bounded allowlisted non-executable bundle assets (up to 64 MiB), not the original IPA or native code. It can show basic metadata/resource previews, but it is not a playable game. Force rebuilds it and opens Android's installer.

CI uploads the importer APK and restricted standalone fixture separately to avoid confusing the products.

The Android library reports conservative Bionic symbol-name candidates separately from executable game code. Import automatically analyzes the IPA and shows step-based workflow progress plus **playable Android code emitted: 0%**. API-name candidates and APK packaging progress are not conversion/playability scores. When enabled in Settings, import also attempts to build a signed, IPA-named **icon-branded APK** with bounded non-executable assets; it omits the IPA and executable code. The red **Force convert to .apk** action explains the limitation, displays live packaging progress, and opens Android's installer. This app-side output remains a nonplayable stub. The Linux host converter can emit a 32-bit `armeabi-v7a` APK for a proven ARM32 integer-leaf subset; ordinary ARMv6/ARMv7 games with framework calls, branches, memory access or runtime needs remain blocked. Broad iOS API/runtime translation and arbitrary game conversion remain unimplemented.

## Build, test, edit

See [Build instructions](docs/BUILD.md), [support matrix and conversion contract](docs/SUPPORT.md), [architecture](docs/ARCHITECTURE.md), and [security](docs/SECURITY.md).

```sh
python3 tools/build_native.py
python3 -m unittest discover -v
./gradlew :app:testDebugUnitTest :app:lintDebug :app:assembleDebug
```

Java 17, Python 3.10+, C++17, Android platform 35, build-tools 35.0.0, NDK 27.2.12479018, CMake 3.22.1; Gradle wrapper included. No Python packages are required.

### Exercise actual offline conversion

With the documented Linux Android toolchain installed:

```sh
python3 tools/make_fixture.py --arch arm64 --output .local/fixture.ipa
python3 -m radek convert .local/fixture.ipa --authorized --output workspace/fixture
# workspace/fixture/fixture.apk
```

`--arch armv6`, `armv7`, `armv7s`, `thumb`, and `thumb2` exercise the proven ARM32 integer-leaf path. ARM32-only fixtures are packaged with a 32-bit `armeabi-v7a` native library; use `--target-abi arm64-v8a` to request a lowered ARM64 artifact where supported. This is not general game conversion: branches, memory access, framework APIs and game runtimes remain blocked. The synthetic fixtures are not signed iOS apps and their native routine only returns 42. `armeabi-v7a` requires Android devices that support 32-bit ARM apps and does not mean compatibility with ARMv6-only hardware.

Icons are recovered through a generic fallback chain (Info.plist names, `@2x`/`@3x`/`~ipad`
variants, compiled `Assets.car` renditions, then other bundle images) with every attempt
recorded. The Android library uses the same architecture-independent plist/scale/asset-catalog
fallback ordering for its displayed game icon; recovered host icons become the generated APK's
launcher icon.

For authorized real IPAs, `analyze` reads actual metadata/dependencies without attempting to decrypt or execute the input:

```sh
python3 -m radek analyze authorized.ipa --authorized --output workspace/analysis
```

A new output directory is required. Reports/logs persist; extraction workspaces are removed. CLI exit codes: `0 READY`, `1 FAILED`, `2 PARTIAL` (analysis-only), `3 BLOCKED`. Nonzero statuses are intentional, not silent successes.

## GitHub APK builds

Open **Actions → Build and validate Android APKs → Run workflow**, select the branch containing this implementation. Pushes and pull requests also run the workflow. Download the **RadekiOSConventor-debug.apk** artifact after a successful run. CI installs the pinned toolchain, builds C++ and Android code, runs host/Kotlin tests and real signed synthetic conversions, validates both APK types, and fails on errors.

Only convert IPAs you own or are authorized to convert. FairPlay/encrypted images are blocked; no protection bypass is provided.
