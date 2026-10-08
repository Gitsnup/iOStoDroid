"""Runtime build generator: lifted game + data tables for the native runtime.

Reads the IPA, lifts every function (same lifter the difftest proved), and
emits the C sources + blobs the ``iostodroid/game/rt`` runtime links against:

* ``game_all.c`` -- all lifted functions in one TU (decls + bodies).
* ``rt_gen.c``   -- DT_FUNCS/DT_ADDRS/DT_STUBS/CALLSITE_MAP tables,
  shim forwarders, mod-init table, region/relocation/binding tables.
* ``rt_gen.h``   -- shared declarations for the runtime.
* ``rt_mem.bin`` -- concatenated non-zerofill section bytes.
* ``rt_report.json`` -- stats for tests.
"""

from __future__ import annotations

import io
import json
import os
import re
import zipfile

from . import disasm, lift, macho
from .lift import sanitize


def _find_exec(zf: zipfile.ZipFile) -> str:
    for n in zf.namelist():
        if n.startswith("Payload/") and n.endswith(".app/AngryBirds"):
            return n
    raise ValueError("game executable not found in IPA")


def load_ipa(ipa_path: str):
    with open(ipa_path, "rb") as f:
        zf = zipfile.ZipFile(io.BytesIO(f.read()))
    raw = zf.read(_find_exec(zf))
    img = macho.parse(raw)
    funcs = disasm.disassemble_all(img)
    ctx = lift.build_context(img, funcs)
    return img, funcs, ctx


def _shim_name(symbol: str) -> str:
    return "shim_" + sanitize(symbol.lstrip("_"))


def _collect_shims(ctx, out) -> list[str]:
    allc = "\n".join(out.values())
    calls = set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", allc))
    defined = set(ctx.cname.values())
    helpers = {
        "if", "while", "for", "switch", "return", "goto", "sizeof",
        "rd32", "wr32", "rd16", "wr16", "rd8", "wr8", "rd64", "wr64",
        "sget", "sset", "u2f", "f2u", "u2d", "d2u", "cc_pass",
        "fl_nz", "fl_add", "fl_sub", "fl_adc", "fl_sbc", "sqrtf",
        "sqrt", "isnan", "rint", "vcvt_s32_f", "vcvt_s32_d",
        "vcvt_u32_f", "vcvt_u32_d", "vfp_nzcv_f", "vfp_nzcv_d",
        "memcpy", "tdispatch",
    }
    shims = sorted(n for n in calls - defined - helpers if n.startswith("shim_"))
    for sym in ctx.import_of_stub.values():
        nm = _shim_name(sym)
        if nm not in shims:
            shims.append(nm)
    return sorted(shims)


def _callsites(funcs, addrs) -> set[int]:
    sites = set()
    for a in addrs:
        for (x, m, _o, b) in funcs[a].instructions:
            w = int.from_bytes(b, "little")
            if ((w >> 25) & 7) == 5 and (w >> 24) & 1:
                sites.add(x)
            elif m == "blx":
                sites.add(x)
    return sites


def generate(ipa_path: str, out_dir: str) -> dict:
    img, funcs, ctx = load_ipa(ipa_path)
    out, failures = lift.lift_all(ctx)
    assert not failures, failures[:5]
    addrs = sorted(out)
    shims = _collect_shims(ctx, out)
    os.makedirs(out_dir, exist_ok=True)

    # ---- game_all.c ----
    with open(os.path.join(out_dir, "game_all.c"), "w") as f:
        f.write('#include "cpu.h"\n')
        for a in addrs:
            f.write(f"void {ctx.cname[a]}(CPU *cpu);\n")
        for n in shims:
            f.write(f"void {n}(CPU *cpu);\n")
        for a in addrs:
            f.write(out[a])
            f.write("\n")

    # ---- rt_mem.bin + region table ----
    regions = []  # (addr, size, blob_off, is_zero)
    blob = bytearray()
    for s in img.sections:
        if s.size == 0:
            continue
        if s.type == macho.S_ZEROFILL:
            regions.append((s.address, s.size, 0, 1))
        else:
            regions.append((s.address, s.size, len(blob), 0))
            blob += img.data[s.file_offset:s.file_offset + s.size]
    with open(os.path.join(out_dir, "rt_mem.bin"), "wb") as f:
        f.write(bytes(blob))

    # ---- ext-reloc table (addr -> symbol) ----
    extrel = []
    for r in img.external_relocations:
        sym = img.symbols[r.symbol_index]
        name = sym.name if hasattr(sym, "name") else str(sym)
        extrel.append((r.address, name))

    # ---- non-lazy pointer syms (slot addr -> symbol) ----
    nl_sec = img.section_named("__DATA", "__nl_symbol_ptr")
    nl_syms = []
    if nl_sec is not None:
        base = nl_sec.address
        idx0 = nl_sec.reserved1
        nslots = nl_sec.size // 4
        for k in range(nslots):
            si = img.indirect_symbols[idx0 + k]
            if si is None or si == 0x80000000 or (isinstance(si, int) and si < 0):
                continue
            try:
                sym = img.symbols[si]
            except Exception:
                continue
            name = sym.name if hasattr(sym, "name") else str(sym)
            nl_syms.append((base + 4 * k, name))

    # ---- la_ptr syms (slot addr -> symbol), for table completeness ----
    la_sec = img.section_named("__DATA", "__la_symbol_ptr")
    la_syms = []
    if la_sec is not None:
        base = la_sec.address
        idx0 = la_sec.reserved1
        nslots = la_sec.size // 4
        for k in range(nslots):
            si = img.indirect_symbols[idx0 + k]
            if si is None or si == 0x80000000 or (isinstance(si, int) and si < 0):
                continue
            try:
                sym = img.symbols[si]
            except Exception:
                continue
            name = sym.name if hasattr(sym, "name") else str(sym)
            la_syms.append((base + 4 * k, name))

    # ---- mod-init addrs ----
    mod_sec = img.section_named("__DATA", "__mod_init_func")
    modinits = []
    if mod_sec is not None:
        for k in range(mod_sec.size // 4):
            modinits.append(img.read_u32(mod_sec.address + 4 * k))

    # ---- main addr ----
    main_addr = None
    for s in img.symbols:
        if getattr(s, "name", "") == "_main" and getattr(s, "n_sect", 0) != 0:
            main_addr = s.value
            break
    assert main_addr is not None, "no defined _main"

    # ---- stub map (stub addr -> symbol) ----
    stub_syms = sorted(macho.stub_map(img))

    # ---- rt_gen.c ----
    sites = _callsites(funcs, addrs)
    with open(os.path.join(out_dir, "rt_gen.c"), "w") as f:
        f.write('#include "rt_gen.h"\n')
        f.write('#include "cpu.h"\n')
        for a in addrs:
            f.write(f"void {ctx.cname[a]}(CPU *cpu);\n")
        f.write("void (*DT_FUNCS[])(CPU *cpu) = {\n")
        for a in addrs:
            f.write(f"    {ctx.cname[a]},\n")
        f.write("};\nunsigned DT_NFUNCS = sizeof(DT_FUNCS)/sizeof(DT_FUNCS[0]);\n")
        f.write("const uint32_t DT_ADDRS[] = {\n")
        for a in addrs:
            f.write(f"    0x{a:x}u,\n")
        f.write("};\n")
        for n in shims:
            f.write(f"void {n}(CPU *cpu);\n")
        f.write("void (*DT_SHIM_FUNCS[])(CPU *cpu) = {\n")
        for n in shims:
            f.write(f"    {n},\n")
        f.write("};\n")
        f.write("const DT_STUB DT_STUBS[] = {\n")
        for sa, _slot, sym in stub_syms:
            nm = _shim_name(sym)
            f.write(f"    {{0x{sa:x}u, {shims.index(nm)}u}},\n")
        f.write("};\nunsigned DT_NSTUBS = sizeof(DT_STUBS)/sizeof(DT_STUBS[0]);\n")
        if sites:
            lo = min(sites) & ~3
            hi = (max(sites) + 7) & ~3
            f.write(f"unsigned DT_CALL_LO = 0x{lo:x}u;\n")
            f.write(f"unsigned DT_CALL_HI = 0x{hi:x}u;\n")
            f.write("const unsigned char CALLSITE_MAP[] = {\n")
            row = []
            for s in range(lo, hi, 4):
                row.append("1" if s in sites else "0")
                if len(row) == 32:
                    f.write("    " + ",".join(row) + ",\n")
                    row = []
            if row:
                f.write("    " + ",".join(row) + ",\n")
            f.write("};\n")
        f.write("static const char *SHIM_SYMS[] = {\n")
        for n in shims:
            sym = n[len("shim_"):]
            f.write(f"    \"{sym}\",\n")
        f.write("};\n")
        # shim forwarders: every shim calls into the runtime by default.
        for i, n in enumerate(shims):
            f.write(f"void {n}(CPU *cpu) {{ rt_shim(cpu, {i}); }}\n")
        f.write("const char *rt_shim_symbol(unsigned i) {\n")
        f.write(f"    if (i >= {len(shims)}u) return \"?\";\n")
        f.write("    return SHIM_SYMS[i];\n}\n")
        f.write(f"unsigned RT_NSHIMS = {len(shims)};\n")
        # regions
        f.write("const RT_REGION RT_REGIONS[] = {\n")
        for (a, ln, off, zero) in regions:
            f.write(f"    {{0x{a:x}u, 0x{ln:x}u, 0x{off:x}u, {zero}}},\n")
        f.write("};\n")
        f.write(f"unsigned RT_NREGIONS = {len(regions)};\n")
        # ext relocs
        f.write("const RT_BIND RT_EXTREL[] = {\n")
        for (a, name) in extrel:
            esc = name.replace("\\", "\\\\").replace('"', '\\"')
            f.write(f'    {{0x{a:x}u, "{esc}"}},\n')
        f.write("};\n")
        f.write(f"unsigned RT_NEXTREL = {len(extrel)};\n")
        # nl/la syms
        f.write("const RT_BIND RT_NLSYM[] = {\n")
        for (a, name) in nl_syms:
            esc = name.replace("\\", "\\\\").replace('"', '\\"')
            f.write(f'    {{0x{a:x}u, "{esc}"}},\n')
        f.write("};\n")
        f.write(f"unsigned RT_NNLSYM = {len(nl_syms)};\n")
        f.write("const RT_BIND RT_LASYM[] = {\n")
        for (a, name) in la_syms:
            esc = name.replace("\\", "\\\\").replace('"', '\\"')
            f.write(f'    {{0x{a:x}u, "{esc}"}},\n')
        f.write("};\n")
        f.write(f"unsigned RT_NLASYM = {len(la_syms)};\n")
        # mod inits
        f.write("void (*RT_MODINITS[])(CPU *cpu) = {\n")
        for a in modinits:
            assert a in ctx.cname, f"modinit {a:#x} not lifted"
            f.write(f"    {ctx.cname[a]},\n")
        f.write("};\n")
        f.write("const uint32_t RT_MODINIT_ADDRS[] = {\n")
        for a in modinits:
            f.write(f"    0x{a:x}u,\n")
        f.write("};\n")
        f.write(f"unsigned RT_NMODINITS = {len(modinits)};\n")
        # main
        assert main_addr in ctx.cname, "main not lifted"
        f.write(f"void (*RT_MAIN)(CPU *cpu) = {ctx.cname[main_addr]};\n")
        f.write(f"uint32_t RT_MAIN_ADDR = 0x{main_addr:x}u;\n")

    with open(os.path.join(out_dir, "rt_gen.h"), "w") as f:
        f.write("#ifndef IOSTODROID_GAME_RT_GEN_H\n")
        f.write("#define IOSTODROID_GAME_RT_GEN_H\n")
        f.write('#include <stdint.h>\n')
        f.write('typedef struct CPU CPU;\n')
        f.write("typedef struct { uint32_t addr; uint32_t len; uint32_t blob; int zero; } RT_REGION;\n")
        f.write("typedef struct { uint32_t addr; const char *sym; } RT_BIND;\n")
        f.write("extern void (*DT_FUNCS[])(CPU *cpu);\nextern unsigned DT_NFUNCS;\n")
        f.write("extern const uint32_t DT_ADDRS[];\n")
        f.write("extern void (*DT_SHIM_FUNCS[])(CPU *cpu);\n")
        f.write("typedef struct { uint32_t addr; unsigned shim; } DT_STUB;\n")
        f.write("extern const DT_STUB DT_STUBS[];\nextern unsigned DT_NSTUBS;\n")
        f.write("extern unsigned DT_CALL_LO;\nextern unsigned DT_CALL_HI;\n")
        f.write("extern const unsigned char CALLSITE_MAP[];\n")
        f.write("extern unsigned RT_NSHIMS;\n")
        f.write("const char *rt_shim_symbol(unsigned i);\n")
        f.write("void rt_shim(CPU *cpu, unsigned i);\n")
        f.write("extern const RT_REGION RT_REGIONS[];\nextern unsigned RT_NREGIONS;\n")
        f.write("extern const RT_BIND RT_EXTREL[];\nextern unsigned RT_NEXTREL;\n")
        f.write("extern const RT_BIND RT_NLSYM[];\nextern unsigned RT_NNLSYM;\n")
        f.write("extern const RT_BIND RT_LASYM[];\nextern unsigned RT_NLASYM;\n")
        f.write("extern void (*RT_MODINITS[])(CPU *cpu);\n")
        f.write("extern const uint32_t RT_MODINIT_ADDRS[];\n")
        f.write("extern unsigned RT_NMODINITS;\n")
        f.write("extern void (*RT_MAIN)(CPU *cpu);\nextern uint32_t RT_MAIN_ADDR;\n")
        f.write("#endif\n")

    report = {
        "functions": len(addrs),
        "shims": len(shims),
        "regions": len(regions),
        "blob_bytes": len(blob),
        "extrel": len(extrel),
        "nlsym": len(nl_syms),
        "lasym": len(la_syms),
        "modinits": len(modinits),
        "main_addr": main_addr,
    }
    with open(os.path.join(out_dir, "rt_report.json"), "w") as f:
        json.dump(report, f, indent=2)
    return report


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("ipa")
    ap.add_argument("out_dir")
    ns = ap.parse_args(argv)
    rep = generate(ns.ipa, ns.out_dir)
    print(json.dumps(rep, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
