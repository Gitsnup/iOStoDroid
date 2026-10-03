"""Shared native analyzer invocation, dependency graph, fail-closed conversion proof."""
from __future__ import annotations
import json
import os
import subprocess
from pathlib import Path
from .archive import InputError
from .resources import MACH_MAGICS
from .ir import Unsupported, Program, lift

ROOT = Path(__file__).resolve().parent.parent


def analyzer_path() -> Path:
    candidates = [Path(os.environ['RADEK_ANALYZER'])] if 'RADEK_ANALYZER' in os.environ else [ROOT / '.local/bin/radek-macho', ROOT / 'native/build/radek-macho']
    for path in candidates:
        if path.is_file():
            return path
    raise RuntimeError('native analyzer missing; run python3 tools/build_native.py')


def analyze(path: Path) -> dict:
    proc = subprocess.run([str(analyzer_path()), str(path)], text=True, capture_output=True, timeout=60)
    if proc.returncode:
        raise InputError('Mach-O analysis failed: ' + proc.stderr.strip())
    if len(proc.stdout) > 64 * 1024 * 1024:
        raise InputError('analysis output exceeds limit')
    return json.loads(proc.stdout)


def dependency_graph(app: Path, main: Path, report: dict) -> dict:
    nodes = [{'path': main.relative_to(app).as_posix(), 'analysis': report}]
    for path in sorted(app.rglob('*')):
        if not path.is_file() or path == main:
            continue
        with path.open('rb') as f:
            magic = f.read(4)
        if magic in MACH_MAGICS:
            nodes.append({'path': path.relative_to(app).as_posix(), 'analysis': analyze(path)})
    edges = []
    by_path = {n['path'] for n in nodes}
    for node in nodes:
        for sl in node['analysis']['slices']:
            for dep in sl['dependencies']:
                name = dep['path']
                target = None
                if name.startswith('@executable_path/'):
                    candidate = name[len('@executable_path/'):]
                    if candidate in by_path:
                        target = candidate
                elif name.startswith('@loader_path/'):
                    candidate = (Path(node['path']).parent / name[len('@loader_path/'):]).as_posix()
                    if candidate in by_path:
                        target = candidate
                elif name.startswith('@rpath/'):
                    for rpath in sl.get('rpaths', []):
                        prefix = rpath.replace('@executable_path', '').lstrip('/') if rpath.startswith('@executable_path') else rpath.replace('@loader_path', str(Path(node['path']).parent))
                        candidate = (Path(prefix) / name[len('@rpath/'):]).as_posix()
                        if candidate in by_path:
                            target = candidate
                edges.append({'from': node['path'], 'architecture': sl['architecture'], 'installName': name,
                              'resolvedBundlePath': target, 'classification': 'unsupported',
                              'reason': 'embedded binary ABI/linking not implemented' if target else 'no verified Darwin framework/ABI provider'})
    return {'nodes': nodes, 'edges': edges}


def prove_leaf(executable: Path, report: dict, graph: dict) -> tuple[dict, Program]:
    if any(s['encrypted'] for n in graph['nodes'] for s in n['analysis']['slices']):
        raise Unsupported('encrypted/FairPlay Mach-O: conversion is prohibited; obtain an unprotected authorized build')
    if len(graph['nodes']) != 1:
        raise Unsupported('embedded frameworks/plugins require a native linker backend that is not implemented')
    candidates = sorted(report['slices'], key=lambda s: {'arm64': 0, 'armv7s': 1, 'armv7': 2}.get(s['architecture'], 99))
    failures = []
    data = executable.read_bytes()
    for sl in candidates:
        try:
            if sl['architecture'] == 'arm64e' or sl['pacRequired']:
                raise Unsupported('ARM64e pointer authentication stripping/re-signing is not proven safe')
            if sl['architecture'] not in ('arm64', 'armv7', 'armv7s') or sl['bigEndian']:
                raise Unsupported('unsupported CPU/endian format')
            if sl['fileType'] != 2:
                raise Unsupported('entry must be an MH_EXECUTE program')
            if sl['dependencies'] or sl['imports']:
                raise Unsupported('Darwin imports/framework dependencies require unimplemented ABI providers')
            if 'chainedFixups' in sl:
                raise Unsupported('chained fixups require address reconstruction')
            if sl['metadata']:
                raise Unsupported('Objective-C/Swift/initializers/unwind metadata requires additional runtime support')
            for link in sl['linkedit']:
                if link.get('kind') in ('rebase', 'bind', 'weakBind', 'lazyBind') and link['size']:
                    raise Unsupported('dyld rebasing/binding is not implemented')
            dynamic = sl.get('dynamicSymbols', {})
            if dynamic.get('externalRelocationCount', 0) or dynamic.get('localRelocationCount', 0) or dynamic.get('undefinedCount', 0) or dynamic.get('indirectCount', 0):
                raise Unsupported('dynamic relocation/indirect symbol table requires linker adaptation')
            allowed_commands = {1, 0x19, 2, 0xb, 0x1b, 0x24, 0x25, 0x2f, 0x30, 0x32, 0x80000028, 0x1d, 0x26, 0x29, 0x21, 0x2c, 0x22, 0x80000022, 0x80000033}
            if any(lc['command'] not in allowed_commands for lc in sl['loadCommands']):
                raise Unsupported('load command requires an unsupported loader semantic')
            if 'entryOffset' not in sl:
                raise Unsupported('only LC_MAIN entry points are currently reconstructed')
            entry = sl['entryOffset']
            section = None
            for seg in sl['segments']:
                for sec in seg['sections']:
                    if sec['relocations']:
                        raise Unsupported('section relocations are not yet linkable')
                    if sec['offset'] <= entry < sec['offset'] + sec['size'] and sec['name'] == '__text' and seg['initialProtection'] & 4:
                        section = sec
            if section is None:
                raise Unsupported('entry point is not in an executable __text section')
            if entry % (4 if sl['architecture'] == 'arm64' else 2):
                raise Unsupported('unaligned entry point')
            address = section['address'] + entry - section['offset']
            thumb = any(sym['value'] == address and sym['description'] & 8 for sym in sl['symbols'])
            code = data[sl['offset']+entry:sl['offset']+section['offset']+section['size']]
            program = lift(code, sl['architecture'], thumb)
            return sl, program
        except Unsupported as exc:
            failures.append(sl['architecture'] + ': ' + str(exc))
    raise Unsupported('; '.join(failures))


def capabilities() -> list[dict]:
    return [
        {'component': 'ARM64 closed integer leaf code', 'status': 'PARTIAL', 'detail': 'MOVZ/MOVK, 32-bit immediate ADD/SUB, RET; preserved native instructions'},
        {'component': 'ARMv7/ARMv7s/Thumb/Thumb-2', 'status': 'PARTIAL', 'detail': 'offline straight-line MOV/ADD/SUB/BX and Thumb-2 MOVW/MOVT lowering only; no general branches/loads/calls'},
        {'component': 'Objective-C', 'status': 'BLOCKED', 'detail': 'experimental host runtime tests are not Apple metadata/objc_msgSend ABI compatibility'},
        *[{'component': name, 'status': 'BLOCKED', 'detail': 'no verified conversion provider; dependency blocks conversion'} for name in ('Foundation/CoreFoundation', 'UIKit/CoreGraphics', 'EAGL/OpenGL ES', 'AudioToolbox/AVFoundation/OpenAL', 'Swift', 'Metal', 'Darwin C/C++ exceptions/TLS/pthreads', 'iOS lifecycle/input/sensors')],
        {'component': 'resources', 'status': 'PARTIAL', 'detail': 'bundle paths retained, normal PNG and RGBA8 CgBI icons normalized; asset catalogs and shader translation unsupported'},
        {'component': 'APK packaging', 'status': 'SUPPORTED', 'detail': 'host SDK/NDK: ARM64 JNI ELF, aapt2, D8, zipalign, apksigner; no on-device compiler'},
    ]
