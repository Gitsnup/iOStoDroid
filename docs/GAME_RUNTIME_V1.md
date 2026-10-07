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
   data of) the first unimplemented import. Implemented adapters run for real
   instead of trapping: libSystem memory/string/malloc, the Itanium C++ ABI
   allocation entry points, the bounded Objective-C runtime, the AudioToolbox
   session state calls, and the bounded application-lifecycle chain
   (`UIApplicationMain` -> delegate instantiation -> `applicationDidFinishLaunching:`
   -> bounded service of the queued background-thread body).
4. The launcher is **fullscreen** (`SYSTEM_UI_FLAG_IMMERSIVE_STICKY` plus
   layout through the display cutout): the recovered bundle splash covers the
   whole display and the boot log sits in a translucent panel at the bottom.
   The splash advances **by itself** while the guest boots (one recovered frame
   every ~0.9 s); touches never cycle frames, and the sequence stops on a stable
   frame once the boot attempt ends.
5. The launcher shows loader/trap/instruction progress in that panel. When guest
   execution stops or setup fails, the launcher keeps the fullscreen diagnostic
   screen open; it does not throw an Android crash or show a preview. The stop
   reason is reported as what it is: a named unimplemented import trap, the
   bounded `TIME_LIMIT`/`INSTRUCTION_LIMIT` budget with the executed instruction
   count (explicitly *not* an unimplemented import), a guest exception, a memory
   or execution fault, or an unavailable CPU backend. A JSON `null` trap name is
   never printed as an import called `null`.
6. Every report keeps `status: "not_runnable"`. Executed instructions are
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

- Loader: `LOADED_WITH_TRAPS`, 60 resolved / 518 trapped / 0 unresolved.
- Boot: entry point reached, **340,309 guest instructions executed**. The
  runtime enters `_main`, performs the `NSAutoreleasePool +new` setup, enters
  `UIApplicationMain`, instantiates the image's own `AppController` delegate,
  wires it into the `UIApplication` singleton, and delivers
  `applicationDidFinishLaunching:` to the real guest implementation. Inside that
  method the app builds its UIKit window/EAGL view (including `-[UIView layer]`
  -> `CAEAGLLayer`, `numberWithBool:`, `dictionaryWithObjectsAndKeys:`,
  `EAGLContext initWithAPI:`/`setCurrentContext:`, `addSubview:`,
  `makeKeyAndVisible`) and then enters its engine's render setup, whose first
  OpenGL ES call (from guest text around `0xAC50C`) stops the attempt.
- Report: `lifecycle.applicationMainEntered: true`,
  `applicationMainReturned: false` (the boot stopped inside the nested delegate
  call), `delegateClassName: "AppController"`, nine recorded startup-chain
  events, and `trappedImport: "_glFrontFace"` with `trapCalls: 1`.
- The VFP unit is enabled for the guest (`CPACR` CP10/CP11 access and
  `FPEXC.EN`), because the ARMv6 image uses scalar VFP from its first delegate
  frame on; without it the attempt stopped on a decode fault at `vpush`.
- The host suite (`tests/test_gameruntime.py`) and CI pin the *shape* of this
  behavior (entry point reached, one named trapped import, a non-empty startup
  chain); the manifest from the CI run is uploaded as
  `angrybirds-gameboot-artifacts`.
- Remaining honest gap: OpenGL ES (51 `_gl*` imports) is **not** implemented, so
  the attempt stops at the first GL call and nothing is rendered.
