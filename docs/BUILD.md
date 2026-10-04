# Build, run, and test

## Clean Linux machine

Install Python 3.10+, a C++17 compiler (`g++`), Java 17 JDK, and the Android SDK command-line tools.
Set `ANDROID_HOME` or `ANDROID_SDK_ROOT` to the SDK directory.

```sh
sdkmanager 'platform-tools' 'platforms;android-35' 'build-tools;35.0.0' 'ndk;27.2.12479018' 'cmake;3.22.1'
sdkmanager --licenses
```

Run the native, host and Android importer checks:

```sh
python3 tools/build_native.py
python3 -m unittest discover -v
bash tools/test_sanitized.sh
./gradlew --no-daemon :app:testDebugUnitTest :app:lintDebug :app:assembleDebug
```

The Android importer APK is `app/build/outputs/apk/debug/app-debug.apk`; CI copies it to
`RadekiOSConventor-debug.apk`. There are no developer-local files to check in.

The host's `radek validate` command needs Android build-tools 35.0.0 (`aapt2`, `zipalign`, and
`apksigner`) to validate an already-built importer or future complete-game APK. It does not build a
game APK.

## Import and inspect on Android

Install the importer APK, choose **Choose IPA**, confirm authorization, and select a document. The
source IPA is stored in private app storage until the library entry is deleted. Import performs
bounded extraction and inspection, then removes its temporary extracted tree. It does not create a
game APK. Details include bundle name/version/identifier, declared `MinimumOSVersion` (not inferred),
recovered icon when decodable (including supported compiled `Assets.car` renditions), architectures,
API candidates, blockers, the raw JSON report and analysis logs.

The analysis progress bar measures input copying, extraction and analysis only. It is not code
translation, APK build progress, runtime validation or playability. The importer's runnable game
code remains at zero unless a complete host conversion is attached under the strict contract. The
red **Force convert to .apk** action does not bypass missing features or install a placeholder; it
explains that no complete game converter is available in this build.

A host APK attachment must match the IPA's SHA-256/package identity and safe IPA-derived basename,
carry `complete-game-v1` metadata, account for every reachable function and API implementation,
claim complete resources/lifecycle support, contain no original IPA or Apple executable, and pass
Android package/signature checks. This repository's host CLI currently emits no APK that satisfies
that contract. Restricted native-entry experiment APKs are rejected.

## Host IPA inspection

Build the native analyzer, then inspect an authorized IPA:

```sh
python3 tools/build_native.py
python3 -m radek analyze authorized.ipa --authorized --output workspace/analysis
```

The workspace must not already exist. Reports and logs persist; extracted workspaces are removed.
The host also reconstructs code metadata and records whether its narrow closed-integer leaf check
succeeds. That check is an experiment, not a complete app conversion. If `convert` is invoked on an
input that passes only this restricted check, it returns `BLOCKED` and produces no APK. This is
intentional: the experiment does not translate all game code, APIs, resources, or lifecycle.

ARM selection for the analysis is deterministic: automatic selection prefers ARM64 in a FAT IPA
containing both ARM32 and ARM64; supported ARM32-only inputs target 32-bit Android ARMv7 (`armeabi-v7a`).
This target selection is not evidence that an APK was built. Explicit `--target-abi` overrides must
have a matching input slice.

The API mapper currently reports candidate symbols and semantic targets only. It does not rewrite
Mach-O bindings, generate Android API implementations, or prove behavior. Unsupported reachable
APIs remain blockers; no conversion output is claimed.

## Validation and status codes

Validate the Android importer after building it:

```sh
python3 -m radek validate app/build/outputs/apk/debug/app-debug.apk \
  --package dev.radek.conventor \
  --entry dev.radek.conventor.MainActivity \
  --converter-app
```

For future converted results, validation requires the complete-game evidence contract; the old
`closed-integer-entry-v1` wrapper no longer qualifies. Runtime execution remains `NOT_TESTED` unless
separate device tests are supplied.

- `0 READY`: reserved for a future complete conversion that passed static validation; not currently
  produced by the IPA pipeline.
- `1 FAILED`: malformed input, tool failure or interrupted work.
- `2 PARTIAL`: inspection completed without a complete conversion.
- `3 BLOCKED`: requested conversion is unsupported or incomplete; no APK was emitted.

## Development signing and CI

Gradle signs the **importer app** using its normal Android debug build configuration. The repository
no longer embeds an APK-signing key or signs generated game stubs. There is no generated game APK to
install or distribute.

`.github/workflows/build.yml` runs native/Python/Android tests, builds and validates the importer,
and uploads only `RadekiOSConventor-debug.apk` plus validation/test reports. CI does not publish a
synthetic, placeholder, or restricted native-entry game APK. No iOS executable is run by tests, and
there is no device/emulator runtime smoke test at present.

## Optional source formatting

C++ uses `.clang-format` (LLVM style, four spaces, 110 columns). Python uses Black with
`pyproject.toml` (110 columns); formatters are developer-only, not build/runtime dependencies.
