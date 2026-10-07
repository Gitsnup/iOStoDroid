# game-runtime-v1: boot-attempt APK contract

A `game-runtime-v1` APK packs a real iOS game executable and its bundle, runs
the actual guest boot on-device, and shows that boot as a minimal diagnostic
log. Guest execution stops at the first actually-used unimplemented import,
but the Android launcher remains open with the stop reason rather than crashing.
It is not a conversion, not a static recompilation, and not gameplay; it never
shows a preview or menu.

## Behavior contract

1. The APK embeds exactly one authorized 32-bit ARM Mach-O slice
   (`assets/gameboot/main-executable.bin`), the bundle resources
   (`assets/bundle/**`), boot metadata (`assets/gameboot.json`), and the
   tested `libcompat_runtime_v1.so` guest-CPU runtime.
2. The launcher (`dev.radek.gameruntime.GameBootActivity`) runs the boot once
   through `Java_dev_radek_gameruntime_GameBootActivity_runGameBootAttempt`.
3. Unimplemented imports are bound to abort-on-call traps. The guest executes
   real instructions from the Mach-O entry point until it calls (or touches
   data of) the first unimplemented import.
4. The launcher shows loader/trap/instruction progress as a scrolling boot
   log. When guest execution stops or setup fails, the launcher keeps the
   diagnostic screen open; it does not throw an Android crash or show a preview.
5. Every report keeps `status: "not_runnable"`. Executed instructions are
   loader/CPU progress, never evidence of a working game.

## Artifact names

| Item | Value |
|---|---|
| APK file | `<SanitizedIpaStem>-game.apk` |
| Package | `dev.radek.gameruntime.p<source-sha256[0:20]><cert-sha256[0:8]>` |
| Launcher | `dev.radek.gameruntime.GameBootActivity` |
| Report key (device) | `gameRuntimeConversion` |
| Manifest (host) | `game-runtime-manifest.json` |

The host (`radek/gameruntime.py`) and the device (`ArtifactNames`,
`GameRuntimeArtifactContract`, `GameRuntimeApkBuilder`) implement the same
naming and package rules; both sides pin them with tests.

## Host tooling

```
python3 -m radek gameboot input.ipa --authorized --output job-dir
```

- Extracts the IPA with the standard archive bounds, selects the boot slice
  (thin ARM file, or the first 32-bit ARM slice of a FAT image), and probes it
  with the `radek-gameboot` host binary (built by CMake with pinned Unicorn).
- Writes `game-runtime-manifest.json` (contract inputs + `hostProbe` summary),
  `gameboot-report.json` (full native report), and `main-executable.bin` (the
  exact staged slice the device builder packs).
- The probe never fails the command: without a built `radek-gameboot` binary
  the manifest records `hostProbe.status: "NOT_PROBED"` with a reason.

## On-device builder

`GameRuntimeApkBuilder` mirrors the bounded converter's packaging mechanics
(template patch, streamed entries, 16 KiB native alignment, v1+v2+v3 signing,
signature/package/label/install audits) with game-runtime inputs:

- Template `gameruntime-template` (package/label sentinels shared with the
  other templates; DEX must define the boot launcher class).
- Executable gate: thin ARM Mach-O, or FAT with a 32-bit ARM slice; anything
  else throws and nothing is built.
- Runtime gate: `libcompat_runtime_v1.so` and its required shared backend
  `libunicorn.so` are copied from the installed converter APK (including ABI
  splits) by `CompatibilityRuntime.extractGameRuntimeInstalled`. The converter
  build explicitly packages both CMake targets into its APK. The game APK
  includes both aligned libraries beside one another, so Android's linker can
  resolve the runtime's `DT_NEEDED` Unicorn dependency. Packaging fails closed
  if either required library is absent; `libc++_shared.so` is also copied when
  the installed build includes it.
- The report records `contract: "game-runtime-v1"`,
  `bootAttemptIncluded: true`, `completeGameConversion: false`,
  `gamePlayable: false`, `gameCodeRecompiled: false`, and the executable,
  runtime, signing, and install-audit evidence.

`ResultProvider` serves `*-game.apk` files only after
`GameRuntimeArtifactContract` re-validates name, package, source hash, signer,
and digest against the report.

## Angry Birds v1.0 status (tracked fixture)

`tests/data/AngryBirds_v1.0_os30.ipa` (thin ARMv6 Mach-O, 1,822,112 bytes,
267 bundle files):

- Loader: `LOADED_WITH_TRAPS`, 39 resolved / 539 trapped / 0 unresolved.
- Boot: entry point reached, 34 guest instructions executed, stopped at the
  first unimplemented call `_UIApplicationMain` (`trapCalls: 1`).
- The host suite (`tests/test_gameruntime.py`) and CI pin this behavior; the
  manifest from the CI run is uploaded as `angrybirds-gameboot-artifacts`.
