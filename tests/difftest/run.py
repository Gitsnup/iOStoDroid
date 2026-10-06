#!/usr/bin/env python3
"""Differential test: lifted C vs unicorn ARM for integer-only functions.

Builds the whole lifted game, then runs every function whose transitive
direct-call closure is VFP-free under both the compiled C and unicorn with
identical random register/stack/memory state, and compares exit mode,
registers, CPSR and memory hashes.

VFP functions are excluded: this unicorn fork's VFP is observably broken
(transfers are no-ops, comparisons set exception bits, rounding is wrong),
so it cannot serve as float ground truth. VFP is covered by helper unit
tests, emission review, and the boot/play test instead.
"""
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import zipfile
import io

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from radek.game import macho, disasm, lift  # noqa: E402
from radek.game.lift import sanitize  # noqa: E402

STACK_BASE, STACK_SIZE = 0x30000000, 0x10000
SCR0_BASE, SCR1_BASE, SCR_SIZE = 0x10000000, 0x11000000, 0x10000
RET_MARKER = 0x80AD0000
INSN_LIMIT = 2000000

BUILD = os.path.join(os.path.dirname(__file__), "dtwork")


def is_vfp_mnemonic(mn: str) -> bool:
    return mn.startswith("v") or mn == "fmstat"


def load_all():
    with open("tests/data/AngryBirds_v1.0_os30.ipa", "rb") as f:
        data = f.read()
    z = zipfile.ZipFile(io.BytesIO(data))
    app = [n for n in z.namelist() if n.endswith(".app/AngryBirds")][0]
    img = macho.parse(z.read(app))
    funcs = disasm.disassemble_all(img)
    ctx = lift.build_context(img, funcs)
    return img, funcs, ctx


def clean_set(funcs):
    """Functions whose transitive direct-call closure has no VFP."""
    vfp = {a for a, f in funcs.items()
           if any(is_vfp_mnemonic(m) for _, m, _, _ in f.instructions)}
    dirty = set(vfp)
    changed = True
    while changed:
        changed = False
        for a, f in funcs.items():
            if a not in dirty and (f.calls & dirty):
                dirty.add(a)
                changed = True
    return {a for a in funcs if a not in dirty}, vfp


def write_build(img, funcs, ctx, out, shims):
    os.makedirs(BUILD, exist_ok=True)
    # memory manifest: all nonempty sections
    regions = []
    blob = bytearray()
    for s in img.sections:
        if s.size == 0:
            continue
        regions.append((s.address, s.size))
        if s.type == macho.S_ZEROFILL:
            blob += bytes(s.size)
        else:
            blob += img.data[s.file_offset:s.file_offset + s.size]
    with open(os.path.join(BUILD, "dt_mem.txt"), "w") as f:
        for a, ln in regions:
            f.write(f"{a:x} {ln:x}\n")
    with open(os.path.join(BUILD, "dt_mem.bin"), "wb") as f:
        f.write(bytes(blob))
    # generated C: function table + shims
    addrs = sorted(out)
    with open(os.path.join(BUILD, "dt_gen.c"), "w") as f:
        f.write('#include "cpu.h"\n')
        for a in addrs:
            f.write(f"void {ctx.cname[a]}(CPU *cpu);\n")
        f.write("void dt_shim_called(unsigned id);\n")
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
        f.write("typedef struct { uint32_t addr; unsigned shim; } DT_STUB;\n")
        f.write("const DT_STUB DT_STUBS[] = {\n")
        for sa, sym in sorted(ctx.import_of_stub.items()):
            nm = "shim_" + sanitize(sym.lstrip("_"))
            f.write(f"    {{0x{sa:x}u, {shims.index(nm)}u}},\n")
        f.write("};\nunsigned DT_NSTUBS = sizeof(DT_STUBS)/sizeof(DT_STUBS[0]);\n")
        # call-site bitmap: every bl/blx address in lifted code creates a
        # VRET continuation; vret_site_ok() uses it to reject forged ones.
        sites = set()
        for a in addrs:
            for (x, m, _o, b) in funcs[a].instructions:
                w = int.from_bytes(b, "little")
                if ((w >> 25) & 7) == 5 and (w >> 24) & 1:
                    sites.add(x)  # B/BL encoding with L bit
                elif m == "blx":
                    sites.add(x)  # register (and imm) blx
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
        f.write("static const char *NAMES[] = {\n")
        for n in shims:
            f.write(f"    \"{n}\",\n")
        f.write("};\nconst char **DT_SHIM_NAMES = NAMES;\n")
        f.write(f"unsigned DT_NSHIMS = {len(shims)};\n")
        for i, n in enumerate(shims):
            f.write(f"void {n}(CPU *cpu) {{ (void)cpu; dt_shim_called({i}); }}\n")
    # whole-game TU
    with open(os.path.join(BUILD, "game_all.c"), "w") as f:
        f.write('#include "cpu.h"\n')
        for a in addrs:
            f.write(f"void {ctx.cname[a]}(CPU *cpu);\n")
        for n in shims:
            f.write(f"void {n}(CPU *cpu);\n")
        f.write("void tdispatch(CPU *cpu, uint32_t x);\n")
        f.write("int vret_site_ok(uint32_t s);\n")
        for a in addrs:
            f.write(out[a])
            f.write("\n")
    shutil.copy("radek/game/rt/cpu.h", os.path.join(BUILD, "cpu.h"))
    shutil.copy(os.path.join(os.path.dirname(__file__), "dt_main.c"),
                os.path.join(BUILD, "dt_main.c"))
    # -O0 on purpose: fault-time CPU-struct readback must reflect the latest
    # register state. At -O1 gcc keeps cpu->r[] in host registers across the
    # inlined rd/wr builtins, so a SIGSEGV mid-instruction reports stale regs.
    r = subprocess.run(["gcc", "-O0", "-o", os.path.join(BUILD, "dt_run"),
                        os.path.join(BUILD, "dt_main.c"),
                        os.path.join(BUILD, "game_all.c"),
                        os.path.join(BUILD, "dt_gen.c"), "-lm"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-3000:])
        print(r.stderr[-3000:])
        raise SystemExit("dt_run build failed")
    return addrs, regions


def gen_regs(rng, data_addrs):
    regs = []
    for _ in range(15):
        pick = rng.random()
        if pick < 0.35:
            regs.append(rng.randrange(0, 0x1000) & ~1)  # small, even
        elif pick < 0.6:
            v = rng.randrange(0, 0x80000000) & ~3  # wild, bit31 clear
            if 0x1000 <= v < 0x200000:
                v += 0x200000  # keep out of the code range
            regs.append(v)
        else:
            base, ln = data_addrs[rng.randrange(len(data_addrs))]
            regs.append((base + rng.randrange(ln)) & ~3)  # mapped pointer
    cpsr = 0x10 | (rng.randrange(16) << 28)
    return regs, cpsr


def fnv(b: bytes) -> int:
    h = 1469598103934665603
    for x in b:
        h ^= x
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def vret_match(c: int, u: int) -> bool:
    """Register/word equality modulo the translator's VRET scheme.

    C call sites set lr to site|VRET_BIT; real ARM sets site+4. A value
    derived from lr (moved, pushed, stored) therefore legitimately differs
    by exactly this transform. Anything else must match bit-for-bit.
    """
    if c == u:
        return True
    return ((c & 0x80000000) and
            (c & 0x7FFFFFFF) == (u - 4) & 0xFFFFFFFF)


def run_c_side(cases):
    p = subprocess.Popen([os.path.join(BUILD, "dt_run"), BUILD],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         text=True)
    inp = "".join(f"{fi:x} {seed:x} {' '.join(f'{r:x}' for r in regs)} {cpsr:x}\n"
                  for fi, _a, seed, regs, cpsr in cases)
    out, _ = p.communicate(inp, timeout=1800)
    lines = out.strip().split("\n")
    assert len(lines) == len(cases), f"{len(lines)} != {len(cases)}"
    res = []
    for ln in lines:
        t = ln.split()
        res.append((t[0], int(t[1], 16), [int(x, 16) for x in t[2:17]],
                    int(t[17], 16), [int(x, 16) for x in t[18:]]))
    return res


def run_uc_side(img, funcs, ctx, regions, cases):
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM
    from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE
    from unicorn import UC_HOOK_MEM_READ_UNMAPPED, UC_HOOK_MEM_WRITE_UNMAPPED
    from unicorn import UC_HOOK_MEM_FETCH_UNMAPPED
    from unicorn.unicorn_const import UC_MEM_FETCH_UNMAPPED
    from unicorn import arm_const as AC
    from unicorn.arm_const import UC_ARM_REG_CPSR
    RIDS = [getattr(AC, f"UC_ARM_REG_R{k}") for k in range(13)]
    RIDS += [AC.UC_ARM_REG_R13, AC.UC_ARM_REG_R14]
    # NOTE: R13/R14 are SP/LR aliases (12/10), NOT R0+13/14.
    blob = open(os.path.join(BUILD, "dt_mem.bin"), "rb").read()
    pristine = {}
    off = 0
    for a, ln in regions:
        pristine[a] = blob[off:off + ln]
        off += ln
    # page unions to map
    pages = set()
    for a, ln in regions:
        s = a & ~0xFFF
        e = (a + ln + 0xFFF) & ~0xFFF
        for p in range(s, e, 0x1000):
            pages.add(p)
    for b in (STACK_BASE, SCR0_BASE, SCR1_BASE):
        for p in range(b, b + SCR_SIZE, 0x1000):
            pages.add(p)
    unions = []
    for p in sorted(pages):
        if unions and p == unions[-1][1]:
            unions[-1][1] += 0x1000
        else:
            unions.append([p, p + 0x1000])
    mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    for s, e in unions:
        mu.mem_map(s, e - s)
    for a, ln in regions:
        mu.mem_write(a, pristine[a])
    text_pages = set()
    for s in img.sections:
        if s.segment == "__TEXT" and s.size:
            for pg in range(s.address & ~0xFFF, (s.address + s.size + 0xFFF) & ~0xFFF, 0x1000):
                text_pages.add(pg)
    helper_addr = next((a for a, f in funcs.items() if f.name == "dyld_stub_binding_helper"), None)
    code_set = set()
    vfp_addrs = set()
    for a, f in funcs.items():
        isv = any(is_vfp_mnemonic(m) for _, m, _, _ in f.instructions)
        for w in f.code_words:
            code_set.add(w)
            if isv:
                vfp_addrs.add(w)
    stub_of = dict(ctx.import_of_stub)
    state = {}
    dirty = set()

    def h_code(u, pc, size, data):
        state["n"] += 1
        if state["n"] > INSN_LIMIT or pc in stub_of or pc in vfp_addrs or pc not in code_set:
            if u.reg_read(UC_ARM_REG_CPSR) & 0x20:
                state["mode"] = "THUMB"  # executed Thumb: unmodelable, skip
            elif state["n"] > INSN_LIMIT:
                state["mode"] = "LIMIT"
            elif pc in stub_of:
                state["mode"] = "SHIM"
                state["detail"] = pc
            elif pc in vfp_addrs:
                state["mode"] = "VFP"
            else:
                state["mode"] = "RET" if pc == RET_MARKER else "DISPATCH"
                state["detail"] = pc
            u.emu_stop()
            return

    def h_write(u, access, addr, size, value, data):
        # NOTE: unicorn also fires this hook for unmapped writes (the
        # unmapped hook fires too); only mapped pages need restoring.
        # Multi-byte accesses can straddle a page boundary: record both.
        for pg in (addr & ~0xFFF, (addr + size - 1) & ~0xFFF):
            if pg in pages:
                dirty.add(pg)

    def h_fault(u, access, addr, size, value, data):
        if access == UC_MEM_FETCH_UNMAPPED:
            state["mode"] = "RET" if addr == RET_MARKER else "DISPATCH"
            state["detail"] = addr & ~1
        elif u.reg_read(UC_ARM_REG_CPSR) & 0x20:
            state["mode"] = "THUMB"  # faulted out of Thumb code: skip
        else:
            state["mode"] = "FAULT"
            state["detail"] = addr
        u.emu_stop()
        return False

    mu.hook_add(UC_HOOK_CODE, h_code)
    mu.hook_add(UC_HOOK_MEM_WRITE, h_write)
    mu.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED |
                UC_HOOK_MEM_FETCH_UNMAPPED, h_fault)

    results = []
    sched_yield = None
    for ci, (fi, addr, seed, regs, cpsr) in enumerate(cases):
        for p in dirty:
            # restore 4k page from pristine sections (zeros elsewhere)
            buf = bytearray(4096)
            for a, ln in regions:
                if a < p + 4096 and p < a + ln:
                    s = max(a, p) - p
                    e = min(a + ln, p + 4096) - p
                    buf[s:e] = pristine[a][max(p, a) - a:max(p, a) - a + (e - s)]
            try:
                mu.mem_write(p, bytes(buf))
            except Exception:
                print(f"RESTORE-FAIL case={ci} page={p:#x} "
                      f"inpages={p in pages} "
                      f"unions={[ (f'{s:#x}',f'{e:#x}') for s,e in unions if s-0x1000 <= p <= e ]} "
                      f"dirty={[ f'{d:#x}' for d in sorted(dirty) ]}", flush=True)
                raise
        dirty.clear()
        s = seed if seed else 0x9E3779B9
        for base, count in ((STACK_BASE, STACK_SIZE // 4),
                            (SCR0_BASE, SCR_SIZE // 4),
                            (SCR1_BASE, SCR_SIZE // 4)):
            words = []
            for _ in range(count):
                s ^= (s << 13) & 0xFFFFFFFF
                s ^= s >> 17
                s ^= (s << 5) & 0xFFFFFFFF
                words.append(s & 0xFFFFFFFF)
            mu.mem_write(base, struct.pack(f"<{count}I", *words))
        # CPSR first: writing it switches register banks, so SP/LR must be
        # set after (fresh Uc boots privileged with a zero user bank).
        mu.reg_write(UC_ARM_REG_CPSR, cpsr)
        for k in range(15):
            mu.reg_write(RIDS[k], regs[k])
        mu.reg_write(RIDS[13], STACK_BASE + STACK_SIZE - 64)
        mu.reg_write(RIDS[14], RET_MARKER)
        state.clear()
        state["n"] = 0
        try:
            mu.emu_start(addr, 0)
        except Exception:
            pass
        if "mode" not in state:
            # emu_start(begin, 0) stops cleanly (no hook, no error) when
            # execution reaches pc == until == 0: a branch to NULL, which
            # the C side reports as DISPATCH/0 via tdispatch.
            pc = mu.reg_read(AC.UC_ARM_REG_PC) & 0xFFFFFFFF
            if pc == RET_MARKER:
                state["mode"] = "RET"
                state["detail"] = 0
            elif pc == 0:
                state["mode"] = "DISPATCH"
                state["detail"] = 0
            else:
                state["mode"] = "UCERR"
                state["detail"] = pc
        got = [mu.reg_read(RIDS[k]) & 0xFFFFFFFF for k in range(15)]
        gotc = mu.reg_read(UC_ARM_REG_CPSR) & 0xFFFFFFFF
        hashes = [fnv(bytes(mu.mem_read(a, ln))) for a, ln in regions]
        hashes.append(fnv(bytes(mu.mem_read(STACK_BASE, STACK_SIZE))))
        hashes.append(fnv(bytes(mu.mem_read(SCR0_BASE, SCR_SIZE))))
        hashes.append(fnv(bytes(mu.mem_read(SCR1_BASE, SCR_SIZE))))
        results.append((state["mode"], state.get("detail", 0), got, gotc, hashes,
                        bool(dirty & text_pages), helper_addr))
    return results


def run_c_dump(cases):
    """Re-run cases under dt_run with DT_DUMP; return {(fi,seed): bytes}."""
    spec = ";".join(f"{fi:x},{seed:x}" for fi, _a, seed, _r, _c in cases)
    env = dict(os.environ)
    env["DT_DUMP"] = spec
    p = subprocess.Popen([os.path.join(BUILD, "dt_run"), BUILD],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         text=True, env=env)
    inp = "".join(f"{fi:x} {seed:x} {' '.join(f'{r:x}' for r in regs)} {cpsr:x}\n"
                  for fi, _a, seed, regs, cpsr in cases)
    p.communicate(inp, timeout=600)
    dumps = {}
    for fi, _a, seed, _r, _c in cases:
        path = os.path.join(BUILD, f"dt_dump_{fi:x}_{seed:x}.bin")
        with open(path, "rb") as f:
            dumps[(fi, seed)] = f.read()
    return dumps


def uc_case_bytes(img, funcs, ctx, regions, case):
    """Fresh-unicorn single case rerun; return per-region final bytes."""
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM
    from unicorn import UC_HOOK_CODE
    from unicorn import UC_HOOK_MEM_READ_UNMAPPED, UC_HOOK_MEM_WRITE_UNMAPPED
    from unicorn import UC_HOOK_MEM_FETCH_UNMAPPED
    from unicorn.unicorn_const import UC_MEM_FETCH_UNMAPPED
    from unicorn import arm_const as AC
    from unicorn.arm_const import UC_ARM_REG_CPSR
    RIDS = [getattr(AC, f"UC_ARM_REG_R{k}") for k in range(13)]
    RIDS += [AC.UC_ARM_REG_R13, AC.UC_ARM_REG_R14]
    blob = open(os.path.join(BUILD, "dt_mem.bin"), "rb").read()
    pristine = {}
    off = 0
    for a, ln in regions:
        pristine[a] = blob[off:off + ln]
        off += ln
    pages = set()
    for a, ln in regions:
        for p in range(a & ~0xFFF, (a + ln + 0xFFF) & ~0xFFF, 0x1000):
            pages.add(p)
    for b in (STACK_BASE, SCR0_BASE, SCR1_BASE):
        for p in range(b, b + SCR_SIZE, 0x1000):
            pages.add(p)
    unions = []
    for p in sorted(pages):
        if unions and p == unions[-1][1]:
            unions[-1][1] += 0x1000
        else:
            unions.append([p, p + 0x1000])
    mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    for s, e in unions:
        mu.mem_map(s, e - s)
    for a, ln in regions:
        mu.mem_write(a, pristine[a])
    code_set = set()
    for f in funcs.values():
        code_set.update(f.code_words)
    stub_of = dict(ctx.import_of_stub)
    state = {"n": 0}

    def h_code(u, pc, size, data):
        state["n"] += 1
        if state["n"] > INSN_LIMIT or pc in stub_of or pc not in code_set:
            u.emu_stop()

    def h_fault(u, access, addr, size, value, data):
        u.emu_stop()
        return False

    mu.hook_add(UC_HOOK_CODE, h_code)
    mu.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED |
                UC_HOOK_MEM_FETCH_UNMAPPED, h_fault)
    fi, addr, seed, regs, cpsr = case
    s = seed if seed else 0x9E3779B9
    for base, count in ((STACK_BASE, STACK_SIZE // 4),
                        (SCR0_BASE, SCR_SIZE // 4),
                        (SCR1_BASE, SCR_SIZE // 4)):
        words = []
        for _ in range(count):
            s ^= (s << 13) & 0xFFFFFFFF
            s ^= s >> 17
            s ^= (s << 5) & 0xFFFFFFFF
            words.append(s & 0xFFFFFFFF)
        mu.mem_write(base, struct.pack(f"<{count}I", *words))
    mu.reg_write(UC_ARM_REG_CPSR, cpsr)
    for k in range(15):
        mu.reg_write(RIDS[k], regs[k])
    mu.reg_write(RIDS[13], STACK_BASE + STACK_SIZE - 64)
    mu.reg_write(RIDS[14], RET_MARKER)
    try:
        mu.emu_start(addr, 0)
    except Exception:
        pass
    out = [bytes(mu.mem_read(a, ln)) for a, ln in regions]
    out.append(bytes(mu.mem_read(STACK_BASE, STACK_SIZE)))
    out.append(bytes(mu.mem_read(SCR0_BASE, SCR_SIZE)))
    out.append(bytes(mu.mem_read(SCR1_BASE, SCR_SIZE)))
    return out


def main() -> int:
    subset = os.environ.get("DIFFTEST_SUBSET")
    seeds = int(os.environ.get("DIFFTEST_SEEDS", "3"))
    img, funcs, ctx = load_all()
    print(f"functions: {len(funcs)}", flush=True)
    deps = ["radek/game/lift.py", "radek/game/rt/cpu.h",
            "tests/difftest/dt_main.c", "tests/difftest/run.py",
            "tests/data/AngryBirds_v1.0_os30.ipa"]
    stamp = ";".join(f"{p}:{os.path.getmtime(p)}" for p in deps)
    stamp_path = os.path.join(BUILD, "dt_stamp")
    cache_path = os.path.join(BUILD, "dt_cache.pkl")
    fresh = (os.path.exists(os.path.join(BUILD, "dt_run")) and
             os.path.exists(cache_path) and
             os.path.exists(stamp_path) and
             open(stamp_path).read() == stamp)
    if fresh:
        import pickle
        with open(cache_path, "rb") as f:
            addrs, regions, shims = pickle.load(f)
        print("reusing cached build", flush=True)
    else:
        out, failures = lift.lift_all(ctx)
        print(f"lifted {len(out)}, failures {len(failures)}", flush=True)
        assert not failures, failures[:5]
        allc = "\n".join(out.values())
        calls = set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", allc))
        defined = set(ctx.cname.values())
        helpers = {"if", "while", "for", "switch", "return", "goto", "sizeof",
                   "rd32", "wr32", "rd16", "wr16", "rd8", "wr8", "rd64", "wr64",
                   "sget", "sset", "u2f", "f2u", "u2d", "d2u", "cc_pass",
                   "fl_nz", "fl_add", "fl_sub", "fl_adc", "fl_sbc", "sqrtf",
                   "sqrt", "isnan", "rint", "vcvt_s32_f", "vcvt_s32_d",
                   "vcvt_u32_f", "vcvt_u32_d", "vfp_nzcv_f", "vfp_nzcv_d",
                   "memcpy", "tdispatch"}
        shims = sorted(n for n in calls - defined - helpers if n.startswith("shim_"))
        # Every import stub gets a shim (not just directly-called ones) so
        # computed branches to stubs route correctly through tdispatch.
        for sym in ctx.import_of_stub.values():
            nm = "shim_" + sanitize(sym.lstrip("_"))
            if nm not in shims:
                shims.append(nm)
        shims.sort()
        print(f"shims: {len(shims)}", flush=True)
        addrs, regions = write_build(img, funcs, ctx, out, shims)
        import pickle
        with open(cache_path, "wb") as f:
            pickle.dump((addrs, regions, shims), f)
        with open(stamp_path, "w") as f:
            f.write(stamp)
        print(f"regions: {len(regions)}, build ok", flush=True)
    clean, vfp = clean_set(funcs)
    addrset = set(addrs)
    clean = {a for a in clean if a in addrset}
    print(f"VFP funcs: {len(vfp)}, clean-closure funcs: {len(clean)}", flush=True)
    stub_name = {}
    for sa, sym in ctx.import_of_stub.items():
        stub_name[sa] = "shim_" + sanitize(sym.lstrip("_"))
    data_regions = [(s.address, s.size) for s in img.sections
                    if s.size and s.segment != "__TEXT"]
    data_addrs = [(a, ln) for a, ln in data_regions]
    data_addrs += [(STACK_BASE, STACK_SIZE), (SCR0_BASE, SCR_SIZE),
                   (SCR1_BASE, SCR_SIZE)]
    targets = sorted(clean)
    addrs_env = os.environ.get("DIFFTEST_ADDRS")
    if addrs_env:
        want = {int(x, 16) for x in addrs_env.split(",")}
        targets = [a for a in targets if a in want]
    if subset:
        targets = targets[:int(subset)]
    fi_of = {a: i for i, a in enumerate(addrs)}
    cases = []
    for a in targets:
        fi = fi_of[a]
        for s in range(1, seeds + 1):
            rng = random.Random((a * 2654435761 + s * 40503) & 0xFFFFFFFF)
            regs, cpsr = gen_regs(rng, data_addrs)
            cases.append((fi, a, s, regs, cpsr))
    print(f"cases: {len(cases)}", flush=True)
    cres = run_c_side(cases)
    print("C side done", flush=True)
    ures = run_uc_side(img, funcs, ctx, regions, cases)
    print("unicorn side done", flush=True)
    npass = nskip = 0
    fails = []
    mem_probes = []
    text_idx = [i for i, (a, _ln) in enumerate(regions)
                if any(s.segment == "__TEXT" and s.address == a for s in img.sections)]
    _blob = open(os.path.join(BUILD, "dt_mem.bin"), "rb").read()
    _lens = [ln for _a, ln in regions]
    pristine_text = [fnv(_blob[sum(_lens[:i]):sum(_lens[:i + 1])]) for i in text_idx]
    for ci, ((cm, cd, cr, cc, ch), (um, ud, ur, uc, uh, u_textdirty, helper)) in enumerate(zip(cres, ures)):
        fi, addr, seed, _r, _c = cases[ci]
        if um in ("VFP", "THUMB"):
            nskip += 1
            continue
        c_textdirty = any(ch[i] != pristine_text[k] for k, i in enumerate(text_idx))
        if u_textdirty or c_textdirty:
            # Self-modifying code: translation cannot model executing
            # rewritten TEXT. Skip (unmodelable on at least one side).
            nskip += 1
            continue
        if um == "DISPATCH" and 0x2FE00000 <= ud < 0x30000000:
            # Unicorn ran the stub helper into dyld: unmodelable glue.
            if cm == "DISPATCH" and cd == helper:
                nskip += 1
                continue
        if (cm, um) in (("TIMEOUT", "LIMIT"),):
            nskip += 1
            continue
        ok = True
        why = []
        if cm != um:
            ok = False
            why.append(f"mode {cm}/{cd:#x} vs {um}/{ud:#x}")
        elif cm == "DISPATCH":
            if not (cd == ud or (cd & ~1) == (ud & ~1)):
                ok = False
                why.append(f"dis {cd:#x} vs {ud:#x}")
        elif cm == "SHIM":
            uname = stub_name.get(ud, "?")
            cname = shims[cd] if cd < len(shims) else "?"
            if cname != uname:
                ok = False
                why.append(f"shim {cname} vs {uname}")
        elif cm == "FAULT":
            if cd != ud:
                ok = False
                why.append(f"fault {cd:#x} vs {ud:#x}")
        if ok:
            reg_bad = [k for k in range(15) if not vret_match(cr[k], ur[k])]
            if reg_bad:
                ok = False
                for k in reg_bad[:6]:
                    why.append(f"r{k} {cr[k]:#x} vs {ur[k]:#x}")
            # Bit 5 (T) is masked: lifted code can never set it (no
            # MRS/MSR/CPS in the game; inputs always enter in ARM state),
            # while unicorn sets it when a wild odd branch enters Thumb.
            # The dispatch target itself is still compared exactly.
            if (cc & ~0x20) != (uc & ~0x20):
                ok = False
                why.append(f"cpsr {cc:#x} vs {uc:#x}")
            if ch != uh:
                if ok:
                    mem_probes.append(ci)
                    continue  # mode+regs agree; phase 2 word-diffs memory
                ok = False
                for i, (x, y) in enumerate(zip(ch, uh)):
                    if x != y:
                        rn = f"{regions[i][0]:x}" if i < len(regions) else ("stack", "scr0", "scr1")[i - len(regions)]
                        why.append(f"mem@{rn} {x:x} vs {y:x}")
                        if len(why) > 8:
                            break
        if ok:
            npass += 1
        else:
            fails.append((addr, funcs[addr].name, seed, cm, cd, um, ud, why))
    if mem_probes:
        print(f"phase 2: word-diffing {len(mem_probes)} mem mismatches", flush=True)
        probe_cases = [cases[ci] for ci in mem_probes]
        dumps = run_c_dump(probe_cases)
        rnames = [f"{a:x}" for a, _ln in regions] + ["stack", "scr0", "scr1"]
        rlens = [ln for _a, ln in regions] + [STACK_SIZE, SCR_SIZE, SCR_SIZE]
        for ci in mem_probes:
            fi, addr, seed, _r, _c = cases[ci]
            cbytes = dumps[(fi, seed)]
            # split dump: manifest regions, then stack, scr0, scr1
            coffs, cregs = [], []
            o = 0
            for ln in rlens:
                cregs.append(cbytes[o:o + ln])
                o += ln
            uregs = uc_case_bytes(img, funcs, ctx, regions, cases[ci])
            unexpl = []
            for i, (cb, ub) in enumerate(zip(cregs, uregs)):
                for w in range(0, len(cb) - 3, 4):
                    cw = int.from_bytes(cb[w:w + 4], "little")
                    uw = int.from_bytes(ub[w:w + 4], "little")
                    if not vret_match(cw, uw):
                        base = regions[i][0] if i < len(regions) else \
                            (STACK_BASE, SCR0_BASE, SCR1_BASE)[i - len(regions)]
                        unexpl.append(f"{rnames[i]}+{w:x} @{base + w:#x}: {cw:#x} vs {uw:#x}")
                        if len(unexpl) >= 6:
                            break
                if len(unexpl) >= 6:
                    break
            if unexpl:
                cm, cd, _cr, _cc, _ch = cres[ci]
                um, ud, _ur, _uc, _uh, _ut, _hh = ures[ci]
                fails.append((addr, funcs[addr].name, seed, cm, cd, um, ud, unexpl))
            else:
                npass += 1
    print(f"PASS {npass} SKIP {nskip} FAIL {len(fails)} / {len(cases)}", flush=True)
    for addr, name, seed, cm, cd, um, ud, why in fails:
        print(f"FAIL {addr:#x} {name} seed={seed} C={cm}/{cd:#x} U={um}/{ud:#x}")
        for w in why[:8]:
            print(f"    {w}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())