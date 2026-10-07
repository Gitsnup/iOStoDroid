# game-runtime-v1: boot-attempt APK contract

A `game-runtime-v1` APK packs a real iOS game executable and its bundle, runs
the actual guest boot on-device, and shows that boot as a minimal diagnostic
log. Guest execution stops at a documented boundary — the first actually-used
unimplemented import, or the bounded instruction/time budget when the guest
stays inside its own code — and the Android launcher remains open with the stop
reason rather than crashing.
It is not a conversion, not a static recompilation, and not gameplay; it never
shows a preview or menu.

## Native GL, not a reimplementation

Guest OpenGL ES 1.1 calls are **forwarded to the platform's own EGL/GLES driver**
(`libEGL.so`/`libGLESv1_CM.so`/`libGLESv2.so` opened at runtime); nothing is
rasterized in-process. `renderbufferStorage:fromDrawable:` creates an EGL window
surface on the launcher's Android surface (or an offscreen pbuffer when no surface
was supplied), `presentRenderbuffer:` is `eglSwapBuffers`, and every guest pointer
argument is translated through the mapped guest regions with a range check. A
surface that arrives after the first offscreen attach makes the GL layer recreate
its window surface, so late surfaces still receive frames. The report's `gles`
block states which driver was loaded, whether the drawable was handed to the
platform, `forwardedCalls`/`refusedCalls`/`framesPresented`, and every refusal as
a named diagnostic: a rendered frame is guest output, not gameplay evidence.

## Behavior contract

1. The APK embeds exactly one authorized 32-bit ARM Mach-O slice
   (`assets/gameboot/main-executable.bin`), the bundle resources
   (`assets/bundle/**`), boot metadata (`assets/gameboot.json`), and the
   tested `libcompat_runtime_v1.so` guest-CPU runtime.
2. The launcher (`dev.radek.gameruntime.GameBootActivity`) runs the boot once
   through `Java_dev_radek_gameruntime_GameBootActivity_runGameBootAttempt`,
   passing the app directory it extracts from `assets/bundle/**`. The runtime
   mounts that directory as the guest's own bundle (read-only) plus writable
   `/Documents` and `/Library` scratch directories, so the guest reads its real
   data files; refused accesses are listed in the report's `guestFileSystem`
   block instead of being invented.
3. Unimplemented imports are bound to abort-on-call traps. The guest executes
   real instructions from the Mach-O entry point until it calls (or touches
   data of) a documented boundary: the first unimplemented import it touches,
   or the bounded instruction/time budget when the guest stays inside its own
   code. Implemented adapters run for real
   instead of trapping: the native OpenGL ES 1.1 forwarding (below), libSystem
   memory/string/malloc and the file/stdio/math/time shims served by the virtual
   file system, the ARM EABI compiler-runtime helpers, the bounded Objective-C
   runtime, the AudioToolbox session state calls, and the bounded
   application-lifecycle chain
   (`UIApplicationMain` -> delegate instantiation -> `applicationDidFinishLaunching:`
   -> bounded service of the queued background-thread body).
4. The launcher is **fullscreen** (`SYSTEM_UI_FLAG_IMMERSIVE_STICKY` plus
   layout through the display cutout) and runs in **sensor landscape** while the
   guest boots, showing only the game: the recovered splash frames are shown
   **once each** (~0.9 s apart) and the sequence then stays on the last frame —
   it never cycles and touches never advance it. A `SurfaceView` above the
   splash receives the guest's frames: its surface is handed to the runtime
   (`setGameSurface` → `ANativeWindow` → EGL window surface) and the guest's
   `renderbufferStorage:fromDrawable:`/`presentRenderbuffer:` pairs become
   `eglCreateWindowSurface`/`eglSwapBuffers` on the platform GLES driver, so a
   frame the guest renders covers the boot screen. The diagnostic panel stays
   hidden while the guest runs and is revealed, after the launcher switches back
   to **portrait**, when the attempt stops.
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

## Launcher presentation

- The launcher runs fullscreen in **sensor landscape** (the device can be turned
  left or right) and shows **only the game**: the recovered splash frames are
  shown fullscreen, each one exactly once, and the sequence stays on the last
  frame instead of cycling; the guest's own EGL frames take over as soon as the
  guest renders. The diagnostics panel stays hidden while the guest runs.
- When the attempt stops for any reason (unimplemented import, budget,
  fault, unavailable backend), the launcher switches back to **portrait** and
  reveals the diagnostic log, so the stop reason is readable without touching
  anything. The activity declares `configChanges` for orientation so rotating
  the device never restarts the guest.

## Angry Birds v1.0 status (tracked fixture)

`tests/data/AngryBirds_v1.0_os30.ipa` (thin ARMv6 Mach-O, 1,822,112 bytes,
267 bundle files):

- Loader: `LOADED_WITH_TRAPS`, 138 resolved / 440 trapped / 0 unresolved.
- Boot: entry point reached, **2,000,000 guest instructions executed** (the
  bounded entry budget). The runtime enters `_main`, performs the
  `NSAutoreleasePool +new` setup, enters `UIApplicationMain`, instantiates the
  image's own `AppController` delegate, wires it into the `UIApplication`
  singleton, delivers `applicationDidFinishLaunching:` to the real guest
  implementation, and keeps running inside the app: it builds its UIKit
  window/EAGL view (`-[UIView layer]` -> `CAEAGLLayer`, `numberWithBool:`,
  `dictionaryWithObjectsAndKeys:`, `EAGLContext initWithAPI:` /
  `setCurrentContext:`, `addSubview:`, `makeKeyAndVisible`), starts its engine
  render setup (the GLES calls are forwarded to the host driver), and asks for
  its own bundle data through the guest filesystem.
- Stop: `INSTRUCTION_LIMIT` — the attempt ends at the bounded budget, not at an
  unimplemented call: `trappedImport` is empty and `trapCalls` is `0` for this
  image. When the guest does touch an unimplemented import the attempt still
  stops there with the trap named, exactly as before.
- Report: `lifecycle.applicationMainEntered: true`,
  `applicationMainReturned: false` (the boot was still running when the budget
  ended), `delegateClassName: "AppController"`, ten recorded startup-chain
  events, and the guest filesystem's refusals are named when no bundle mount is
  configured.
- The VFP unit is enabled for the guest (`CPACR` CP10/CP11 access and
  `FPEXC.EN`), because the ARMv6 image uses scalar VFP from its first delegate
  frame on; without it the attempt stopped on a decode fault at `vpush`.
- The host suite (`tests/test_gameruntime.py`) and CI pin the *shape* of this
  behavior (entry point reached, a documented stop boundary — a named trapped
  import or a bounded execution limit — and a non-empty startup chain); the
  manifest from the CI run is uploaded as
  `angrybirds-gameboot-artifacts`.
- Remaining honest gap: OpenGL ES (51 `_gl*` imports) is **not** implemented, so
  the attempt stops at the first GL call and nothing is rendered.
