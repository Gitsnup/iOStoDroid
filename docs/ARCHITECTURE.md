# Modules and extension points

- `app/`: Kotlin framework-only Android UI. `Library` owns private IPA storage, analysis reports,
  progress and architecture-independent icon recovery; `AssetCatalogIcon` reads bounded raster
  renditions from compiled `Assets.car`; `AndroidApiMapper` reports unimplemented symbol/API
  candidates without claiming a relink; `SafeZip` handles untrusted extraction; `Plist` handles
  XML/binary parsing; `NativeBridge` calls C++ through JNI; `ResultProvider` is read-only and only
  exposes a host APK accepted under the complete-game contract.
- `native/src/macho.cpp`: host/Android shared C++ Mach-O analyzer. It never executes the input.
  JSON describes source structure, not conversion success.
- `native/include/runtime.hpp`: experimental portable runtime and storage primitives with host tests.
  It is intentionally isolated from conversion dependency resolution until a tested Apple ABI
  adapter exists.
- `radek/archive.py`: authorized input staging, bounded archive/plist import and metadata.
- `radek/analysis.py`: native analyzer invocation, dependency graph and fail-closed leaf eligibility.
- `radek/ir.py`: explicit register/width/flag-aware instruction records, decoding, proof and
  ARM64/ARMv7 lowering. ARM32 inputs target `armeabi-v7a`; unsupported instruction classes raise
  `Unsupported`. Lowered bytes are currently only an in-memory experiment, never a game artifact.
- `radek/resources.py`: icon normalization, resource inventory, executable/signature exclusion.
- `radek/dex.py`: bounded DEX integrity and class-identity inspection.
- `radek/apk.py`: importer/host APK validation. Game-APK creation is deliberately disabled until
  a complete game-code and API-replacement backend exists.
- `radek/pipeline.py`: guarded analysis states, durable JSON/JSONL reports and isolated temporary
  workspace cleanup. A restricted leaf assessment never creates an APK or reaches `READY`.
- `tests/`, `app/src/test/`, `native/tests/`: synthetic fixtures, negative/security tests, native
  runtime tests and Robolectric importer tests.

## States

The importer's analysis path is:

`IMPORTED → ANALYZING → PARTIAL | BLOCKED | FAILED`

- `PARTIAL`: inspection completed, but complete game conversion is not implemented.
- `BLOCKED`: input protection, unsupported executable semantics or missing conversion capabilities
  prevent output. A restricted integer-entry assessment is also blocked as a game conversion.
- `FAILED`: malformed input, analysis/tool failure or interrupted work.
- `READY`: reserved for a future complete native game conversion whose APK passes independent
  static validation. No current IPA-to-game path can enter `READY`.

The app's **Force convert to .apk** button does not override these states. If complete conversion is
unavailable, it explains the limitation and emits no APK. A host result is shareable/installable
only after the `complete-game-v1` contract passes attachment checks; the CLI currently emits no
such result.

## Adding real support

Do not add bundle-ID exceptions or turn symbol matches into claimed implementations. Add parser,
decoder, IR, linker, API/runtime provider and lifecycle code with positive and negative tests, then
prove each reachable code path and API implementation is generated and linked. A dependency can
only be classified `converted`, `provided by compatibility layer`, or `Android equivalent` once a
real tested implementation is wired into the output. The existing closed-integer leaf proves only
a narrow function and does not implement its app, resources, lifecycle or APIs.

Future graphics providers should separate API command/state capture from an Android GLES backend;
Metal requires shader/type translation plus an appropriate Android rendering backend. Neither
exists, so unsupported APIs remain blocked rather than bound to no-ops. Framework providers should
be introduced behind a versioned ABI symbol registry and tests of observable behavior, not by
accepting symbol names alone.
