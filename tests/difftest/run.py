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
MEM_SIZE = 0xE0800000
INSN_LIMIT = 200000

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
        for a in addrs:
            f.write(out[a])
            f.write("\n")
    shutil.copy("radek/game/rt/cpu.h", os.path.join(BUILD, "cpu.h"))
    shutil.copy(os.path.join(os.path.dirname(__file__), "dt_main.c"),
                os.path.join(BUILD, "dt_main.c"))
    r = subprocess.run(["gcc", "-O1", "-o", os.path.join(BUILD, "dt_run"),
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
    from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1,
                                   UC_ARM_REG_R13, UC_ARM_REG_R14,
                                   UC_ARM_REG_CPSR)
    assert UC_ARM_REG_R1 == UC_ARM_REG_R0 + 1
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
        if state["n"] > INSN_LIMIT:
            state["mode"] = "LIMIT"
            u.emu_stop()
            return
        if pc in stub_of:
            state["mode"] = "SHIM"
            state["detail"] = pc
            u.emu_stop()
            return
        if pc in vfp_addrs:
            state["mode"] = "VFP"
            u.emu_stop()
            return
        if pc not in code_set:
            state["mode"] = "RET" if pc == RET_MARKER else "DISPATCH"
            state["detail"] = pc
            u.emu_stop()

    def h_write(u, access, addr, size, value, data):
        # NOTE: unicorn also fires this hook for unmapped writes (the
        # unmapped hook fires too); only mapped pages need restoring.
        pg = addr & ~0xFFF
        if pg in pages:
            dirty.add(pg)

    def h_fault(u, access, addr, size, value, data):
        if access == UC_MEM_FETCH_UNMAPPED:
            state["mode"] = "RET" if addr == RET_MARKER else "DISPATCH"
            state["detail"] = addr & ~1
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
        for k in range(15):
            mu.reg_write(UC_ARM_REG_R0 + k, regs[k])
        mu.reg_write(UC_ARM_REG_R13, STACK_BASE + STACK_SIZE - 64)
        mu.reg_write(UC_ARM_REG_R14, RET_MARKER)
        mu.reg_write(UC_ARM_REG_CPSR, cpsr)
        state.clear()
        state["n"] = 0
        try:
            mu.emu_start(addr, 0)
        except Exception:
            if "mode" not in state:
                state["mode"] = "UCERR"
                state["detail"] = 0
        if "mode" not in state:
            state["mode"] = "UCERR"
            state["detail"] = 0
        got = [mu.reg_read(UC_ARM_REG_R0 + k) & 0xFFFFFFFF for k in range(15)]
        gotc = mu.reg_read(UC_ARM_REG_CPSR) & 0xFFFFFFFF
        hashes = [fnv(bytes(mu.mem_read(a, ln))) for a, ln in regions]
        hashes.append(fnv(bytes(mu.mem_read(STACK_BASE, STACK_SIZE))))
        hashes.append(fnv(bytes(mu.mem_read(SCR0_BASE, SCR_SIZE))))
        hashes.append(fnv(bytes(mu.mem_read(SCR1_BASE, SCR_SIZE))))
        results.append((state["mode"], state.get("detail", 0), got, gotc, hashes))
    return results


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
    for ci, ((cm, cd, cr, cc, ch), (um, ud, ur, uc, uh)) in enumerate(zip(cres, ures)):
        fi, addr, seed, _r, _c = cases[ci]
        if cm == "FAULT" and cd >= MEM_SIZE:
            cm = "TIMEOUT"  # host-stack overflow: runaway recursion, not a mem fault
        if um == "VFP":
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
            if cr != ur:
                ok = False
                for k in range(15):
                    if cr[k] != ur[k]:
                        why.append(f"r{k} {cr[k]:#x} vs {ur[k]:#x}")
                        if len(why) > 6:
                            break
            if cc != uc:
                ok = False
                why.append(f"cpsr {cc:#x} vs {uc:#x}")
            if ch != uh:
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
    print(f"PASS {npass} SKIP {nskip} FAIL {len(fails)} / {len(cases)}", flush=True)
    for addr, name, seed, cm, cd, um, ud, why in fails[:25]:
        print(f"FAIL {addr:#x} {name} seed={seed} C={cm}/{cd:#x} U={um}/{ud:#x}")
        for w in why[:8]:
            print(f"    {w}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())