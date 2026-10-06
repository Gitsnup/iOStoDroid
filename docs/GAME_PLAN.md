# Angry Birds 1.0.0 → playable APK: architecture & build plan

Date: 2026-10-06. Difftest gate GREEN (`e6d54f6`, full3.log: 3081/57/0, EXIT:0).
Toolchain gate GREEN: `zig cc -target aarch64-linux-musl -shared -Wl,--gc-sections
-Wl,--as-needed` builds a self-contained ET_DYN (4.8KB, NEEDED=[], UND=[]) offline.

## Game facts (census, all verified from the IPA)

- ARMv6 MH_EXECUTE, not encrypted, entry `start` 0x4320, 1.24MB `__text`
  (2837 lifted funcs), 80KB `__DATA`, 51KB bss. NOT PIE (fixed link addrs).
- Dylibs: Foundation, UIKit, OpenGLES, QuartzCore, CoreGraphics(unused!),
  OpenAL, AudioToolbox, libstdc++, libgcc_s, libSystem, libobjc, CoreFoundation.
- 254 imports: libSystem 124 (C/POSIX+math+pthread), OpenGLES 56 (ES 1.1
  fixed-function + VBO + FBO-OES + PVRTC + multitexture + EAGL consts),
  OpenAL 19 (buffers/sources/queueing), libstdc++ 15 (new/delete/SjLj cxa),
  libgcc_s 12 (SjLj unwind + div/mod builtins), libobjc 7, Foundation 7,
  UIKit 7, CoreFoundation 4, AudioToolbox 2 (stubs).
- ObjC: 2 game classes (AppController: delegate+mainloop/update/present;
  MyEAGLView: EAGL view, touches, framebuffer mgmt), 54 selectors, 15
  classrefs, 483 msgSend sites, classic (non-compressed) binding:
  348 external relocs + indirect symbols. No categories, no NIB.
- 10 C++ mod_inits (Rovio `lang/pf/gr/io` engine + Box2D globals).
- Game statically links: Box2D (C++), libpng, mpg123. NO Lua interpreter
  (4 vestigial data syms only; .lua files ship as inert data).
- UIAcceleration x/y/z, NSThread (game thread + performSelectorOnMainThread),
  NSFastEnumeration (touches), msgSend_stret (CGRect), objc_setProperty.
- Landscape-right, status bar hidden, bundle com.clickgamer.AngryBirds.
- Assets (`data/`, 14.8MB): 139 wav + 68 lua + 24 dat + 14 png + 9 pvr +
  9 mp3 + plists. Root: Info.plist (bplist), Icon.png, PkgInfo, ...

## Key design decisions (locked)

- **Memory**: game u32 addrs index a host `malloc` arena (`MEMBASE[a]`,
  cpu.h model, no mmap tricks). Heap = dlmalloc port over arena carve
  (game pointers must be u32!). Host pointers NEVER enter game memory:
  FILE*/pthread/mutex/errno-tm use side tables or arena copies.
- **GLES**: Android has native GLESv1_CM (libGLESv1_CM.so) — NO GL
  translation. EAGLContext/CAEAGLLayer → EGL bridge. PVRTC (IMG-only)
  decoded at runtime in the bridge (PVRTC1 4bpp+2bpp → RGBA upload).
- **Present scaling**: game renders offscreen 480x320 (exact iPhone
  behavior); bridge letterboxes to the device surface (textured quad,
  full state save/restore). Touch coords mapped through the letterbox.
- **Audio**: OpenAL → OpenSL ES bridge (buffer-queue streaming).
  AudioSession* = stubs. Game decodes mp3 itself (mpg123).
- **Files**: own stdio core over host fds (fopen/fread/... + O_FLAG
  translation). VFS: `/bundle` → APK assets / host dir (case-insensitive
  fallback!); `/files` → writable app files (saves). Game cannot list
  dirs (no opendir import) — only known paths served.
- **printf/scanf**: own printf core on ARM varargs (no host va_list
  synthesis). fscanf: minimal, driven by call-site census.
- **setjmp/longjmp**: side table + host sigsetjmp (ARM jmp_buf layout
  never touched; size-irrelevant).
- **pthread**: side tables (Darwin mutex_t/pthread_t shapes differ);
  lifted start routines run on host threads with per-thread CPU.
- **errno**: `___error` bridge → bionic `__errno` / musl
  `__errno_location`; `__errno_location` stub for musl-compiled refs.
- **ctype**: provide BSD rune tables (`__DefaultRuneLocale` etc.).
- **C++**: own SjLj runtime (`__gxx_personality_sj0`, cxa_throw/catch,
  `_Unwind_SjLj_*` over a side table + sigsetjmp; LSDA parsed at
  runtime from `__gcc_except_tab`). Typeinfo/vtable objects live in the
  arena (loader-bound). new/delete → arena dlmalloc. atexit list.
- **ObjC**: legacy 32-bit runtime layouts in-arena (exact
  Darwin offsets); classes/strings/kEAGL consts/CFString class/empty
  cache+vtable materialized by the loader; msgSend/Super2/stret,
  retain/release pools (drained per frame), fast-enum, properties,
  performSelectorOnMainThread queue (drained per frame).
- **Threading**: game NSThread = host pthread + per-thread CPU +
  autorelease pool; GL `present` happens on main thread via the queue.
- **APK (fully offline, no SDK)**: hand-rolled DEX (NativeActivity
  subclass), hand-rolled AXML manifest, v1 JAR signing in pure Python,
  zipalign in the writer. NEEDED libc.so resolves to bionic (same
  SONAME!); GLESv1_CM/EGL/OpenSLES/android/log via zig-built stub libs
  (NDK trick). minSdk 30 (statx-class symbols), arm64 only.
- **Test strategy**: host harness (gcc/x86_64) runs the REAL lifted game
  + runtime with null GL/AL drivers + scripted input; asserts frames,
  GL/AL streams, sim progress. Cross/JNI/APK parts validated
  structurally (ELF/JNI/DEX/AXML/signature round-trips).

## Build order

1. `rt/arena+loader+bridges-skeleton+main`: reach `main()`; relocs,
   mod_inits verified. (lift.py runtime mode: stubs → direct bridge calls.)
2. `rt/objc+foundation`: game classes registered; reach
   `applicationDidFinishLaunching:` + first `mainloop` with null GL.
3. `rt/uikit+eagl(null)+vfs+stdio+posix+cxx`: full game to N frames;
   assert GL/AL streams + sim advance.
4. Scripted input (slingshot drag, tilt) → gameplay assertions.
5. Real GLES/OpenSL/JNI paths (compile-checked) → zig cross-build
   `libgame.so` → ELF/JNI validation.
6. DEX+AXML+sign+assemble → APK → structural validation.
7. Docs, README, full gate, commit. NO PR until finished.
