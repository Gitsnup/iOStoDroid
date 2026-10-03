# Build / run / test

## Clean Linux machine

Install Python 3.10 or newer, a C++17 compiler (`g++`), and Java 17 JDK (`java`, `javac`, `keytool` on PATH). Install Google's Android SDK command-line tools using Android Studio's SDK manager or the official command-line tools distribution. Set `ANDROID_HOME` (or `ANDROID_SDK_ROOT`) to that SDK directory; nothing is tied to a developer's local path.

Install packages:

```sh
sdkmanager 'platform-tools' 'platforms;android-35' 'build-tools;35.0.0' 'ndk;27.2.12479018' 'cmake;3.22.1'
sdkmanager --licenses
```

The host packager currently supports Linux x86_64 toolchains. The generated APKs target Android 8.0+ ARM64 devices. No iPhone, macOS, server, iOS runtime or emulator is required by the output.

```sh
python3 tools/build_native.py
python3 -m unittest discover -v
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

Install the importer APK, tap **+ ADD IPA**, confirm authorization and select a document. The library persists inside app-private storage. Details include real bundle name/version/identifier, decoded icon where possible, architectures, import size, dependency blockers, raw JSON report and logs. Extraction progress reports actual completed/total entries, never invented percentages. Restarted incomplete jobs become FAILED.

The phone is an importer/analyzer; it is **not** currently an on-device native compiler. Transfer the original authorized input to the Linux host and run the CLI below. Attach the resulting APK to its matching library entry; source SHA-256/package identity must match. Android's installer verifies signing before installing. APK attachment does not falsely change the original analysis state to READY.

## Native conversion

```sh
python3 tools/make_fixture.py --arch thumb2 --output .local/test.ipa
python3 -m radek convert .local/test.ipa --authorized --output workspace/test
```

The workspace must not already exist. `report.json` and `conversion.jsonl` contain real steps and tool outputs. READY is only emitted after static APK validation. Runtime device execution is recorded separately as NOT_TESTED, not implied by static validation.

```sh
python3 -m radek validate workspace/test/RadekiOSConventor-debug.apk \
  --package dev.radek.converted.p<SOURCE_SHA256_FIRST_20_HEX> \
  --entry dev.radek.generated.MainActivity
```

Read the exact package from `report.json`. `adb install -r <apk>` installs the result on a physical ARM64 Android device. The native synthetic entry returns 42 and logs `RadekNative: native entry returned 42`.

## Development signing

The host creates `.local/signing/debug.keystore` on first use and reuses it. A configurable persistent development key can be supplied with `--debug-key`. Alias `androiddebugkey`, passwords `android`, RSA-2048, development-only certificate. This is a reproducible **configuration and reusable identity**, not a promise of bit-for-bit reproducible newly generated keys or APKs. A clean machine generates a different key; preserve your development key outside Git if updates must retain signing identity. Gradle uses its standard development signing key for the importer. Never use these keys/passwords for production.

## CI

`.github/workflows/build.yml` runs the entire SDK-enabled suite with `RADEK_REQUIRE_ANDROID=1`; absent SDKs cannot turn required integration tests into skips. It uploads importer and converted-program artifacts separately. Tests do not execute iOS binaries. No device/emulator runtime smoke test is currently part of CI.
