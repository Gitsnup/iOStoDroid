# Build / run / test

## Clean Linux machine

Install Python 3.10 or newer, a C++17 compiler (`g++`), and Java 17 JDK (`java`, `javac`, `keytool` on PATH). Install Google's Android SDK command-line tools using Android Studio's SDK manager or the official command-line tools distribution. Set `ANDROID_HOME` (or `ANDROID_SDK_ROOT`) to that SDK directory; nothing is tied to a developer's local path.

Install packages:

```sh
sdkmanager 'platform-tools' 'platforms;android-35' 'build-tools;35.0.0' 'ndk;27.2.12479018' 'cmake;3.22.1'
sdkmanager --licenses
```

The host packager currently supports Linux x86_64 toolchains. It emits Android 8.0+ `arm64-v8a` APKs for ARM64 inputs and can emit `armeabi-v7a` APKs for the restricted ARM32 leaf subset. ARM32 APKs require a device that still supports 32-bit ARM apps; this is not ARMv6-device compatibility. No iPhone, macOS, server, iOS runtime or emulator is required by the output.

```sh
python3 tools/build_native.py
python3 -m unittest discover -v
bash tools/test_sanitized.sh
RADEK_REQUIRE_ANDROID=1 python3 -m unittest tests.test_apk -v
./gradlew --no-daemon :app:testDebugUnitTest :app:lintDebug :app:assembleDebug
```

The Android app APK is `app/build/outputs/apk/debug/app-debug.apk`. CI copies it to `RadekiOSConventor-debug.apk`. Android Studio can open the repository directly. There are no developer-local files to check in.

The first host command uses only a C++17 compiler. Alternatively:

```sh
cmake -S native -B native/build
cmake --build native/build
ctest --test-dir native/build --output-on-failure
```

## Import on Android

Install the importer APK, tap **+ ADD IPA**, confirm authorization and select a document. The library persists inside app-private storage; the source IPA stays private until the entry is deleted and is **never copied into the generated APK**. Import starts analysis automatically and shows workflow-stage progress. With the default Settings option enabled, it then attempts to build an IPA-named, signed APK whose launcher icon is the recovered IPA icon. The stub can include metadata and up to 64 MiB of allowlisted non-executable bundle resources; it excludes the original IPA and Mach-O code and is not a playable game. Details include real bundle name/version/identifier, declared `MinimumOSVersion` when present, recovered icon and method, architectures, API-name candidates, blockers, full JSON report and logs. The UI separates workflow completion from actual runnable game-code output (currently 0% on device); symbol candidates are not a conversion score. Extraction progress reports real archive entries. Restarted jobs become FAILED with their last recorded progress.

The **Force convert to .apk** action shows a clear nonplayable disclaimer, displays live build stages/percentage, and launches Android's installer after signing and verification. Its percentage measures workflow steps, not a prediction of compatibility or playability. Settings control automatic stub creation, detailed live steps, host ABI preference and APK-install permission.

The host CLI can emit a 32-bit `armeabi-v7a` APK for ARMv6/ARMv7 inputs that pass its closed integer-leaf proof; `--target-abi arm64-v8a` requests ARM64 lowering instead. This only handles verified straight-line immediate arithmetic and return instructions. Ordinary ARM games remain blocked when they require calls, branches, memory, frameworks or game-runtime services. `armeabi-v7a` requires a device that supports 32-bit ARM apps; it is not ARMv6-only hardware compatibility.

The phone is an importer/analyzer; it is **not** currently an on-device native compiler or general game converter. The automatic `<picked-ipa-name>.apk` is a signed, installable icon-branded stub; its launcher icon is the recovered IPA icon and its APK omits the source IPA and gameplay. It opens to a small information screen and is not playable. The red **Force convert to .apk** action rebuilds that stub and opens Android's package installer, which requires the user to confirm. Transfer the original authorized input to the Linux host and run the CLI below for the restricted verified leaf-conversion path; its output is also named after the IPA. Attach a host-built APK to its matching library entry; source SHA-256/package identity must match. APK attachment does not falsely change the original analysis state to READY.

## Native conversion

```sh
python3 tools/make_fixture.py --arch thumb2 --output .local/test.ipa
python3 -m radek convert .local/test.ipa --authorized --output workspace/test
```

The workspace must not already exist. `report.json` and `conversion.jsonl` contain real steps and tool outputs. READY is only emitted after static APK validation. Runtime device execution is recorded separately as NOT_TESTED, not implied by static validation.

```sh
python3 -m radek validate workspace/test/test.apk \
  --package dev.radek.converted.p<SOURCE_SHA256_FIRST_20_HEX> \
  --entry dev.radek.generated.MainActivity
```

Read the exact package and ABI from `report.json`. `adb install -r <apk>` installs the result on an ABI-compatible Android device. ARM32-only source fixtures use `armeabi-v7a`; that ABI is not available on ARM64-only devices. The synthetic native entry returns 42 and logs `RadekNative: native entry returned 42`.

## Development signing

The host creates `.local/signing/debug.keystore` on first use and reuses it. A configurable persistent development key can be supplied with `--debug-key`. Alias `androiddebugkey`, passwords `android`, RSA-2048, development-only certificate. This is a reproducible **configuration and reusable identity**, not a promise of bit-for-bit reproducible newly generated keys or APKs. A clean machine generates a different key; preserve your development key outside Git if updates must retain signing identity. Gradle uses its standard development signing key for the importer and game-stub template APKs; the importer embeds this key so it can sign generated stubs locally. Never distribute a release build with this key or use these keys/passwords for production.

## CI

`.github/workflows/build.yml` runs the entire SDK-enabled suite with `RADEK_REQUIRE_ANDROID=1`; absent SDKs cannot turn required integration tests into skips. It uploads importer and converted-program artifacts separately. Tests do not execute iOS binaries. No device/emulator runtime smoke test is currently part of CI.

## Optional source formatting

C++ uses `.clang-format` (LLVM style, four spaces, 110 columns). Python uses Black with `pyproject.toml` (110 columns); formatters are developer-only, not build/runtime dependencies. All business logic is separated from UI and SDK tool invocation for editing/testing.
