"""Targeted VFP emission tests: one keystone-assembled instruction per case.

Locks the exact C emitted for every VFP shape in the game (plus implemented
extras and fail-closed rejections). Complements the difftest, which covers
integer-only functions (this unicorn fork's VFP is broken, so VFP has no
emulation ground truth here; helpers are unit-tested against documented
ARM1176 semantics instead).
"""
import pytest

from keystone import Ks, KS_ARCH_ARM, KS_MODE_ARM

from radek.game import lift
from radek.game.disasm import Function
from radek.game.lift import LiftContext, LiftError

_KS = Ks(KS_ARCH_ARM, KS_MODE_ARM)


def emit_one(asm: str) -> str:
    enc, _ = _KS.asm(asm)
    raw = bytes(enc)
    assert len(raw) == 4, asm
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    insn = list(md.disasm(raw, 0x1000))[0]
    func = Function(address=0x1000, name="t",
                    instructions=[(0x1000, insn.mnemonic, insn.op_str, raw)],
                    code_words={0x1000})
    ctx = LiftContext(image=None, functions={0x1000: func})
    ctx.cname[0x1000] = "t_test"
    return lift.lift_function(ctx, func)


def norm(s: str) -> str:
    return "".join(s.split())


def test_pushpop():
    c = norm(emit_one("vpush {d8-d11}"))
    assert "cpu->r[13]-=32u;" in c
    assert "wr64(cpu->r[13]+0u,cpu->d[8]);" in c
    assert "wr64(cpu->r[13]+24u,cpu->d[11]);" in c
    c = norm(emit_one("vpop {d8}"))
    assert "cpu->d[8]=rd64(cpu->r[13]+0u);" in c
    assert "cpu->r[13]+=8u;" in c
    c = norm(emit_one("vpush {s0-s3}"))
    assert "cpu->r[13]-=16u;" in c
    assert "wr32(cpu->r[13]+12u,sget(cpu,3));" in c
    c = norm(emit_one("vpop {s5}"))
    assert "sset(cpu,5,rd32(cpu->r[13]+0u));" in c
    assert "cpu->r[13]+=4u;" in c


def test_multi():
    c = norm(emit_one("vstmia r4!, {s7}"))
    assert "_t2=cpu->r[4];" in c
    assert "wr32(_t2+0u,sget(cpu,7));" in c
    assert "cpu->r[4]=_t2+4u;" in c
    c = norm(emit_one("vldmia r9!, {s10}"))
    assert "sset(cpu,10,rd32(_t2+0u));" in c
    assert "cpu->r[9]=_t2+4u;" in c
    c = norm(emit_one("vstmia r0, {d8-d9}"))
    assert "wr64(_t2+0u,cpu->d[8]);" in c
    assert "wr64(_t2+8u,cpu->d[9]);" in c
    assert "cpu->r[0]=_t2" not in c  # no writeback
    c = norm(emit_one("vldmia r1, {d8}"))
    assert "cpu->d[8]=rd64(_t2+0u);" in c


def test_vmov():
    assert "sset(cpu,0,sget(cpu,1));" in norm(emit_one("vmov.f32 s0, s1"))
    assert "cpu->d[0]=cpu->d[1];" in norm(emit_one("vmov.f64 d0, d1"))
    assert "cpu->r[0]=sget(cpu,0);" in norm(emit_one("vmov r0, s0"))
    assert "sset(cpu,0,cpu->r[0]);" in norm(emit_one("vmov s0, r0"))
    c = norm(emit_one("vmov r0, r1, d7"))
    assert "cpu->r[0]=(uint32_t)cpu->d[7];" in c
    assert "cpu->r[1]=(uint32_t)(cpu->d[7]>>32);" in c
    c = norm(emit_one("vmov d1, r0, r1"))
    assert "cpu->d[1]=((uint64_t)(cpu->r[1])<<32)|(cpu->r[0]);" in c
    # pc source folds to addr+8
    assert "sset(cpu,0,((uint32_t)0x1008));" in norm(emit_one("vmov s0, pc"))
    # predicated form wraps in cc_pass (mi=4)
    c = norm(emit_one("vmovmi.f32 s3, s4"))
    assert "if(cc_pass(cpu->cpsr,4)){sset(cpu,3,sget(cpu,4));}" in c


def test_vcmp_fmstat():
    c = norm(emit_one("vcmp.f32 s13, s0"))
    assert "_f0=u2f(sget(cpu,13));" in c
    assert "_f1=u2f(sget(cpu,0));" in c
    assert "cpu->fpscr=(cpu->fpscr&~0xF0000000u)|vfp_nzcv_f(_f0,_f1);" in c
    c = norm(emit_one("vcmpe.f64 d2, d3"))
    assert "_d0=u2d(cpu->d[2]);" in c
    assert "cpu->fpscr=(cpu->fpscr&~0xF0000000u)|vfp_nzcv_d(_d0,_d1);" in c
    c = norm(emit_one("vcmp.f32 s15, #0"))
    assert "_f1=0.0f;" in c
    c = norm(emit_one("vcmp.f64 d0, #0"))
    assert "_d1=0.0;" in c
    c = norm(emit_one("vmrs apsr_nzcv, fpscr"))
    assert "cpu->cpsr=(cpu->cpsr&~0xF0000000u)|(cpu->fpscr&0xF0000000u);" in c


def test_vcvt():
    cases = [
        ("vcvt.f32.s32 s0, s1", "sset(cpu,0,f2u((float)(int32_t)sget(cpu,1)));"),
        ("vcvt.f32.u32 s0, s1", "sset(cpu,0,f2u((float)sget(cpu,1)));"),
        ("vcvt.f64.s32 d0, s1", "cpu->d[0]=d2u((double)(int32_t)sget(cpu,1));"),
        ("vcvt.f64.u32 d0, s1", "cpu->d[0]=d2u((double)sget(cpu,1));"),
        ("vcvt.f32.f64 s0, d1", "sset(cpu,0,f2u((float)u2d(cpu->d[1])));"),
        ("vcvt.f64.f32 d0, s1", "cpu->d[0]=d2u((double)u2f(sget(cpu,1)));"),
        ("vcvt.s32.f32 s0, s1", "sset(cpu,0,vcvt_s32_f(u2f(sget(cpu,1))));"),
        ("vcvt.s32.f64 s0, d1", "sset(cpu,0,vcvt_s32_d(u2d(cpu->d[1])));"),
        ("vcvt.u32.f32 s0, s1", "sset(cpu,0,vcvt_u32_f(u2f(sget(cpu,1))));"),
        ("vcvt.u32.f64 s0, d1", "sset(cpu,0,vcvt_u32_d(u2d(cpu->d[1])));"),
    ]
    for asm, expect in cases:
        assert expect in norm(emit_one(asm)), asm
    # predicated vcvt still parses the direction (le=13)
    c = norm(emit_one("vcvtle.s32.f32 s15, s15"))
    assert "if(cc_pass(cpu->cpsr,13)){" in c
    assert "vcvt_s32_f(u2f(sget(cpu,15)))" in c


def test_arith3():
    c = norm(emit_one("vmla.f32 s15, s14, s14"))
    assert "_f0=u2f(sget(cpu,15));" in c
    assert "_f1=(u2f(sget(cpu,14))*u2f(sget(cpu,14)));" in c
    assert "sset(cpu,15,f2u(_f0+_f1));" in c
    c = norm(emit_one("vmls.f32 s0, s1, s2"))
    assert "sset(cpu,0,f2u(_f0-_f1));" in c
    c = norm(emit_one("vnmla.f32 s0, s1, s2"))
    assert "sset(cpu,0,f2u(_f1-_f0));" in c
    c = norm(emit_one("vnmls.f32 s0, s1, s2"))
    assert "sset(cpu,0,f2u(-(_f0+_f1)));" in c
    c = norm(emit_one("vnmul.f32 s0, s1, s2"))
    assert "_f0=-(u2f(sget(cpu,1))*u2f(sget(cpu,2)));" in c
    assert "sset(cpu,0,f2u(_f0));" in c
    c = norm(emit_one("vadd.f32 s2, s6, s12"))
    assert "_f0=(u2f(sget(cpu,6))+u2f(sget(cpu,12)));" in c
    c = norm(emit_one("vsub.f64 d0, d1, d2"))
    assert "_d0=(u2d(cpu->d[1])-u2d(cpu->d[2]));" in c
    assert "cpu->d[0]=d2u(_d0);" in c
    c = norm(emit_one("vmul.f64 d7, d7, d6"))
    assert "_d0=(u2d(cpu->d[7])*u2d(cpu->d[6]));" in c
    c = norm(emit_one("vdiv.f32 s13, s15, s13"))
    assert "_f0=(u2f(sget(cpu,15))/u2f(sget(cpu,13)));" in c
    c = norm(emit_one("vmla.f64 d0, d1, d2"))
    assert "cpu->d[0]=d2u(_d0+_d1);" in c
    # predicated arithmetic wraps (ne=1)
    c = norm(emit_one("vdivne.f32 s15, s14, s15"))
    assert "if(cc_pass(cpu->cpsr,1)){" in c


def test_arith2():
    c = norm(emit_one("vsqrt.f32 s13, s15"))
    assert "sset(cpu,13,f2u(sqrtf(u2f(sget(cpu,15)))));" in c
    c = norm(emit_one("vneg.f32 s9, s10"))
    assert "sset(cpu,9,(sget(cpu,10)^0x80000000u));" in c
    c = norm(emit_one("vneg.f64 d6, d6"))
    assert "cpu->d[6]=(cpu->d[6]^0x8000000000000000ull);" in c
    c = norm(emit_one("vabs.f32 s1, s8"))
    assert "sset(cpu,1,(sget(cpu,8)&0x7FFFFFFFu));" in c


def test_fail_closed():
    with pytest.raises(LiftError):
        emit_one("vmov.f32 s0, #1.0")
    with pytest.raises(LiftError):
        emit_one("vmsr fpscr, r0")
    with pytest.raises(LiftError):
        emit_one("vmrs r0, fpscr")
    with pytest.raises(LiftError):
        emit_one("vmov r15, s0")
