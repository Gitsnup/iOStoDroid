# RadekiOSConventor

An **offline native-reconstruction workbench**, not an iOS emulator. Kotlin Android importer + C++ Mach-O analyzer + Python host conversion/SDK packaging pipeline.

> **Important: this is NOT a general IPA/game converter.** Native conversion re-emits a
> *proved* closed integer entry leaf as Android ARM64 machine code: straight-line code with
> no imports, framework calls, memory access, relocations or runtime metadata. Anything
> outside that proven subset is reported honestly instead of being stubbed, and ARM64e (PAC)
> has no backend. Dependency lines are no longer simply "BLOCKED": every iOS framework and
> imported symbol is mapped to the real Android implementation that provides it (identical C
> ABIs such as OpenGL ES/EGL/iconv/SQLite, or platform APIs such as AAudio, MediaPlayer,
> Choreographer, `android.view`, Canvas, sockets), and the app reports the resulting
> percentage coverage. APIs with no Android contract (StoreKit, GameKit, AdSupport, MapKit,
> CoreLocation ...) stay blocked and say why.

## Offline reconstruction before conversion

Every import is analyzed before any conversion decision is made:

- Mach-O images (executable, embedded dylibs/frameworks): headers, load commands,
  segments/sections, symbols, relocations, exports/imports, dependencies and fixups.
- Disassembly and function discovery with basic-block CFGs (ARM64/ARM64e, ARMv7/Thumb/Thumb-2),
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

## Two distinct APKs

1. **Converter/importer app:** the Actions artifact `RadekiOSConventor-debug.apk` contains the
   Android library/import UI, the native analyzer **and the on-device converter**. It converts
   an imported IPA to a signed, installable APK without any host toolchain:
   `Ir.kt` proves the entry leaf and emits Android ARM64, `Elf.kt` builds a loadable ELF64
   image, `Axml.kt` writes a binary manifest, and `ApkBuilder`/`ApkSign` produce a ZIP signed
   with both JAR (v1) and APK Signature Scheme v2 using a key generated and kept on the device.
   A red **Force convert to .apk** button builds the APK even when coverage is incomplete or
   the entry leaf is unproven, and records `forced=true` in the report.
2. **Standalone converted program:** `python3 -m radek convert ...` produces a separate
   `RadekiOSConventor-debug.apk` with the same layout from the host CLI. Neither variant ships
   the IPA, the Mach-O executable, an emulator, an interpreter, a runtime translator or the
   converter. The supported entry contract returns an integer; the Android entry activity
   shows that result plus the conversion provenance. It does **not** recreate an iOS UI.

CI uploads the second APK separately as `standalone-native-fixture` to avoid confusing the products.

## Conversion percentage

The library and detail screens show the real build progress (each pipeline stage reports its
own percentage, 100% = the signed APK is written and verified). Support is reported separately
as **Android provider coverage**: the share of the slice's dependencies and imported symbols
with a real Android provider. The APK is built either way; 100% coverage means nothing in the
converted slice needs an unmapped Darwin API.

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
# workspace/fixture/RadekiOSConventor-debug.apk
```

`--arch armv6`, `armv6thumb`, `armv4t`, `armv7`, `armv7s`, `thumb`, and `thumb2` exercise
**offline ARM32 → ARM64 lowering**, including the Thumb-1-only code layout of iPhone OS 2-5
games. The fixtures are generated synthetic Mach-O programs, not installable signed iOS apps.
The resulting native routine returns 42. The 32-bit subset now covers register moves, ADD/SUB
(register and immediate), AND/ORR/EOR/BIC, MUL and LSL/LSR/ASR immediates; everything else is
rejected rather than mistranslated.

Icons are recovered through a generic fallback chain (Info.plist names, `@2x`/`@3x`/`~ipad`
variants, compiled `Assets.car` renditions, then other bundle images) with every attempt
recorded; the recovered icon becomes the generated APK's launcher icon. On device the same
chain runs on top of `BitmapFactory`, adds Apple `CgBI` PNG repair, rejects fully transparent
artwork, and finally generates a deterministic icon from the app name — so every library entry
shows a visible icon instead of a blank tile.

For authorized real IPAs, `analyze` reads actual metadata/dependencies without attempting to decrypt or execute the input:

```sh
python3 -m radek analyze authorized.ipa --authorized --output workspace/analysis
```

A new output directory is required. Reports/logs persist; extraction workspaces are removed. CLI exit codes: `0 READY`, `1 FAILED`, `2 PARTIAL` (analysis-only), `3 BLOCKED`. Nonzero statuses are intentional, not silent successes.

## GitHub APK builds

Open **Actions → Build and validate Android APKs → Run workflow**, select the branch containing this implementation. Pushes and pull requests also run the workflow. Download the **RadekiOSConventor-debug.apk** artifact after a successful run. CI installs the pinned toolchain, builds C++ and Android code, runs host/Kotlin tests and real signed synthetic conversions, validates both APK types, and fails on errors.

Only convert IPAs you own or are authorized to convert. FairPlay/encrypted images are blocked; no protection bypass is provided.
