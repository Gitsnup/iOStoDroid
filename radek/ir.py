"""Offline leaf-function lifting. This module never executes guest instructions.

The IR can describe a broader machine than the current proven backend accepts.
ARM64 slices keep their original instruction bytes; every 32-bit ARM slice
(armv4t/armv5tej/**armv6**/armv7/armv7s and their Thumb modes) is decoded into
explicit records and re-emitted as real ARM64 instructions.

Nothing is stubbed: an operation that has no verified ARM64 lowering raises
``Unsupported`` and the slice is rejected instead of being mistranslated.
"""

from dataclasses import dataclass, field, asdict
from enum import Enum
import struct
import hashlib

from .arch import ARM32, ARM64, is_arm32


class Unsupported(ValueError):
    pass


class Op(str, Enum):
    CONST = "const"
    INSERT = "insert"
    MOVE = "move"
    MVN = "mvn"
    ADD = "add"
    SUB = "sub"
    AND = "and"
    ORR = "orr"
    EOR = "eor"
    BIC = "bic"
    MUL = "mul"
    SHIFT = "shift"
    RETURN = "return"
    LOAD = "load"
    STORE = "store"
    COMPARE = "compare"
    BRANCH = "branch"
    CALL = "call"
    ADDRESS = "address"
    PUSH = "push"
    POP = "pop"
    ATOMIC = "atomic"


#: Scratch register used while lowering ARM32 immediate logical operands. ARM32
#: leaf code may only touch r0-r12 (see ``_limit``), so w13/w14/w15 are provably
#: free and the scratch value can never clobber live guest state.
SCRATCH = 15


@dataclass
class Instruction:
    op: Op
    address: int
    dst: int | None = None
    src: int | None = None
    immediate: int = 0
    shift: int = 0
    width: int = 32
    writes_flags: bool = False
    #: Third source register (Rm of a register-register operation).
    extra: int | None = None


@dataclass
class Block:
    address: int
    instructions: list[Instruction] = field(default_factory=list)
    successors: list[int] = field(default_factory=list)


@dataclass
class Program:
    architecture: str
    blocks: list[Block]
    machine_code: bytes
    source_size: int

    def report(self):
        return {
            "architecture": self.architecture,
            "blocks": [asdict(b) for b in self.blocks],
            "sourceBytes": self.source_size,
            "outputBytes": len(self.machine_code),
            "machineCodeSha256": hashlib.sha256(self.machine_code).hexdigest(),
            "backend": "preserved-arm64" if self.architecture in ARM64 else "offline-arm32-to-arm64",
        }


def _const(dst: int, value: int) -> list[int]:
    """MOVZ/MOVK pair materialising a 32-bit constant in ``dst``."""
    value &= 0xFFFFFFFF
    words = [0x52800000 | ((value & 0xFFFF) << 5) | dst]
    if value >> 16:
        words.append(0x72A00000 | ((value >> 16) << 5) | dst)
    return words


def _logical(base: int, dst: int, src: int, operand: int) -> int:
    """Shifted-register logical instruction with an unshifted second operand."""
    return base | (operand << 16) | (src << 5) | dst


LOGIC_BASE = {Op.AND: 0x0A000000, Op.ORR: 0x2A000000, Op.EOR: 0x4A000000, Op.BIC: 0x0A200000}


def _emit(i: Instruction) -> bytes:
    """Lower one IR record to real ARM64 instruction words."""
    dst = i.dst if i.dst is not None else 0
    src = i.src if i.src is not None else 0
    if i.op == Op.CONST:
        words = _const(dst, i.immediate << i.shift)
    elif i.op == Op.INSERT:
        words = [0x72800000 | ((i.shift // 16) << 21) | (i.immediate << 5) | dst]
    elif i.op == Op.MOVE:
        words = [0x2A0003E0 | (src << 16) | dst]            # MOV Wd, Wm == ORR Wd, WZR, Wm
    elif i.op == Op.MVN:
        words = [0x2A2003E0 | (src << 16) | dst]            # MVN Wd, Wm == ORN Wd, WZR, Wm
    elif i.op in (Op.ADD, Op.SUB):
        if i.extra is not None:                              # ADD/SUB Wd, Wn, Wm
            base = 0x0B000000 if i.op == Op.ADD else 0x4B000000
            words = [base | (i.extra << 16) | (src << 5) | dst]
        else:
            if not 0 <= i.immediate <= 4095:
                raise Unsupported("ARM immediate cannot be lowered to a single safe ADD/SUB")
            base = 0x11000000 if i.op == Op.ADD else 0x51000000
            words = [base | (i.immediate << 10) | (src << 5) | dst]
    elif i.op in LOGIC_BASE:
        if i.extra is not None:
            words = [_logical(LOGIC_BASE[i.op], dst, src, i.extra)]
        else:
            # The ARM bitmask-immediate encoding cannot express every constant, so
            # the value is materialised in a scratch register that ARM32 leaf code
            # provably never uses and then applied with the register form.
            words = _const(SCRATCH, i.immediate) + [_logical(LOGIC_BASE[i.op], dst, src, SCRATCH)]
    elif i.op == Op.MUL:
        # MUL Wd, Wn, Wm == MADD Wd, Wn, Wm, WZR
        words = [0x1B007C00 | ((i.extra or 0) << 16) | (src << 5) | dst]
    elif i.op == Op.SHIFT:
        amount = i.immediate & 31
        if not 1 <= amount <= 31:
            raise Unsupported("zero-amount shift is not an instruction")
        if i.shift == 0:                                     # LSL -> UBFM #32-s, #31-s
            words = [0x53000000 | ((32 - amount) << 16) | ((31 - amount) << 10) | (src << 5) | dst]
        elif i.shift == 1:                                   # LSR -> UBFM #s, #31
            words = [0x53000000 | (amount << 16) | (31 << 10) | (src << 5) | dst]
        elif i.shift == 2:                                   # ASR -> SBFM #s, #31
            words = [0x13000000 | (amount << 16) | (31 << 10) | (src << 5) | dst]
        else:
            raise Unsupported("ROR has no verified single-instruction ARM64 lowering")
    elif i.op == Op.RETURN:
        words = [0xD65F03C0]
    else:
        raise Unsupported("IR operation has no verified backend: " + i.op.value)
    return b"".join(struct.pack("<I", w) for w in words)


def _rotated(imm8: int, rotate: int) -> int:
    """ARM32 immediate operand: 8-bit value rotated right by 2*rotate."""
    rotate *= 2
    return imm8 if not rotate else ((imm8 >> rotate) | (imm8 << (32 - rotate))) & 0xFFFFFFFF


# ARM32 data-processing opcodes with a verified logical ARM64 lowering.
_A32_LOGICAL = {0: Op.AND, 1: Op.EOR, 12: Op.ORR, 14: Op.BIC}


def _decode_arm32(word: int, address: int) -> Instruction:
    """A32 (ARM) decode for the proven closed-leaf subset."""
    if word == 0xE12FFF1E:                                   # BX LR
        return Instruction(Op.RETURN, address)
    if word >> 28 != 14:
        raise Unsupported(f"conditional ARM instruction 0x{word:08x} at +0x{address:x}")
    body = word & 0x0FFFFFFF
    flags = bool(word & 1 << 20)
    if body & 0x0FE0F0F0 == 0x00000090:                      # MUL/MULS Rd, Rm, Rs
        return Instruction(Op.MUL, address, word >> 16 & 15, word >> 8 & 15,
                           writes_flags=flags, extra=word & 15)
    if body >> 25 in (0, 1):                                 # data processing
        opcode = body >> 21 & 15
        dst, src = word >> 12 & 15, word >> 16 & 15
        immediate = bool(body >> 25)
        operand = word & 0xFFF
        if not immediate and operand & 0x10:
            # bit4 set with a register operand is the miscellaneous/halfword
            # transfer class, never data processing; reject instead of guessing.
            raise Unsupported(f"ARM instruction 0x{word:08x} at +0x{address:x} is unsupported")
        if opcode == 13 and src == 0:                        # MOV / MOVS
            if immediate:
                return Instruction(Op.CONST, address, dst,
                                   immediate=_rotated(word & 255, word >> 8 & 15), writes_flags=flags)
            if operand & 0xFF0 == 0:
                return Instruction(Op.MOVE, address, dst, operand & 15, writes_flags=flags)
            kind, amount = operand >> 5 & 3, operand >> 7 & 31
            if operand & 0x10 or kind == 3 or amount == 0 or (kind and amount == 0):
                raise Unsupported(
                    f"ARM operand 0x{operand:03x} at +0x{address:x} is outside the proven subset"
                )
            return Instruction(Op.SHIFT, address, dst, operand & 15, immediate=amount, shift=kind,
                               writes_flags=flags)
        if opcode == 15 and src == 0:                        # MVN / MVNS
            if immediate:
                return Instruction(Op.CONST, address, dst,
                                   immediate=(~_rotated(word & 255, word >> 8 & 15)) & 0xFFFFFFFF,
                                   writes_flags=flags)
            if operand & 0xFF0:
                raise Unsupported("shifted MVN operand is outside the proven subset")
            return Instruction(Op.MVN, address, dst, word & 15, writes_flags=flags)
        if opcode in (2, 4):                                 # SUB / ADD
            op = Op.SUB if opcode == 2 else Op.ADD
            if immediate:
                return Instruction(op, address, dst, src, _rotated(word & 255, word >> 8 & 15),
                                   writes_flags=flags)
            if operand & 0xFF0 == 0:
                return Instruction(op, address, dst, src, writes_flags=flags, extra=operand & 15)
            raise Unsupported("shifted-register ADD/SUB operand is outside the proven subset")
        if opcode in _A32_LOGICAL:
            op = _A32_LOGICAL[opcode]
            if immediate:
                return Instruction(op, address, dst, src, _rotated(word & 255, word >> 8 & 15),
                                   writes_flags=flags)
            if operand & 0xFF0 == 0:
                return Instruction(op, address, dst, src, writes_flags=flags, extra=operand & 15)
            raise Unsupported("shifted logical operand is outside the proven subset")
        raise Unsupported("ARM data processing operation outside proven subset")
    raise Unsupported(f"ARM instruction 0x{word:08x} at +0x{address:x} is unsupported")


def _decode_thumb(word: int, address: int) -> Instruction:
    """T1 (Thumb) decode for the proven closed-leaf subset."""
    if word == 0x4770:                                       # BX LR
        return Instruction(Op.RETURN, address)
    if word & 0xF800 in (0x2000, 0x3000, 0x3800):            # MOVS/ADDS/SUBS #imm8
        dst, imm = word >> 8 & 7, word & 255
        op = {0x2000: Op.CONST, 0x3000: Op.ADD, 0x3800: Op.SUB}[word & 0xF800]
        return Instruction(op, address, dst, dst if op != Op.CONST else None, imm, writes_flags=True)
    if word & 0xF800 == 0x0000:                              # LSL/LSR/ASR Rd, Rm, #imm5
        kind, amount = word >> 11 & 3, word >> 6 & 31
        if kind == 3 or amount == 0:
            raise Unsupported(f"Thumb shift 0x{word:04x} at +0x{address:x} is outside the proven subset")
        return Instruction(Op.SHIFT, address, word & 7, word >> 3 & 7, immediate=amount, shift=kind,
                           writes_flags=True)
    if word & 0xFC00 in (0x1800, 0x1A00):                    # ADD/SUB Rd, Rn, Rm
        op = Op.ADD if word & 0xFC00 == 0x1800 else Op.SUB
        return Instruction(op, address, word & 7, word >> 3 & 7, writes_flags=True, extra=word >> 6 & 7)
    if word & 0xFC00 in (0x1C00, 0x1E00):                    # ADD/SUB Rd, Rn, #imm3
        op = Op.ADD if word & 0xFC00 == 0x1C00 else Op.SUB
        return Instruction(op, address, word & 7, word >> 3 & 7, word >> 6 & 7, writes_flags=True)
    if word & 0xFF00 == 0x4600:                              # MOV Rd, Rm (any registers)
        return Instruction(Op.MOVE, address, ((word >> 4) & 8) | (word & 7), (word >> 3) & 15)
    # T1 data-processing register group: 0100 00 op(9-6) Rm Rd. The six op bits
    # must all be matched or MUL/BIC/MVN alias onto ORR/AND.
    table = {
        0x4000: Op.AND, 0x4040: Op.EOR, 0x4300: Op.ORR, 0x4340: Op.MUL, 0x4380: Op.BIC, 0x43C0: Op.MVN,
    }
    if word & 0xFFC0 in table:
        op = table[word & 0xFFC0]
        dst, src = word & 7, word >> 3 & 7
        if op == Op.MVN:
            return Instruction(Op.MVN, address, dst, src, writes_flags=True)
        if op == Op.MUL:
            return Instruction(Op.MUL, address, dst, src, writes_flags=True, extra=src)
        return Instruction(op, address, dst, src, writes_flags=True, extra=src)
    if word & 0xFBF0 in (0xF240, 0xF2C0):
        raise Unsupported("Thumb-2 32-bit encoding requires the second halfword")
    raise Unsupported(
        f"Thumb instruction 0x{word:04x} at +0x{address:x} is unsupported "
        "(branches/IT/register shifts require a future CFG backend)"
    )


#: Architectures whose Thumb mode is Thumb-1 only. A 32-bit Thumb encoding in a
#: pre-ARMv7 slice is not a valid instruction, so it is rejected rather than read
#: as two unrelated 16-bit instructions.
THUMB1_ONLY = ("armv4t", "armv5tej", "armv6", "armv6m")


def _decode_thumb2(first: int, second: int, address: int, architecture: str = "armv7") -> Instruction:
    """32-bit Thumb encodings (T2). MOVW/MOVT are proven; everything else is not."""
    if architecture in THUMB1_ONLY:
        label = architecture.replace("arm", "ARM", 1)
        raise Unsupported(f"Thumb-2 is unavailable on {label} (Thumb-1 only)")
    if second & 0x8000:
        raise Unsupported("invalid Thumb-2 MOVW/MOVT")
    imm = ((first & 15) << 12) | ((first >> 10 & 1) << 11) | ((second >> 12 & 7) << 8) | (second & 255)
    if first & 0xFBF0 == 0xF2C0:
        return Instruction(Op.INSERT, address, second >> 8 & 15, immediate=imm, shift=16)
    return Instruction(Op.CONST, address, second >> 8 & 15, immediate=imm)


def _limit(architecture: str) -> int:
    """Highest usable register index. ARM32 SP/LR/PC and ARM64 platform and
    callee-saved registers never cross the ABI, so touching them is rejected."""
    return 16 if architecture in ARM64 else 13


#: First halfwords that start a 32-bit Thumb instruction (11101/11110/11111).
THUMB_32BIT = (0xE800, 0xF000, 0xF800)


def lift(code: bytes, architecture: str, thumb: bool = False) -> Program:
    """Prove a straight-line closed leaf and produce Android ARM64 bytes."""
    if architecture not in ARM64 and not is_arm32(architecture):
        raise Unsupported("no safe backend for " + architecture)
    if architecture not in ARM32:
        thumb = False
    instructions, initialized, output = [], set(), bytearray()
    limit = _limit(architecture)
    p = 0
    while p < len(code) and len(instructions) < 4096:
        start = p
        if architecture in ARM64:
            if p + 4 > len(code):
                raise Unsupported("truncated ARM64 instruction")
            w = struct.unpack_from("<I", code, p)[0]
            p += 4
            if w == 0xD65F03C0:
                i = Instruction(Op.RETURN, start)
            elif w & 0xFF800000 in (0x52800000, 0x72800000):
                shift = (w >> 21 & 3) * 16
                if shift > 16:
                    raise Unsupported("invalid 32-bit MOV encoding")
                op = Op.CONST if w & 0xFF800000 == 0x52800000 else Op.INSERT
                imm = w >> 5 & 0xFFFF
                i = Instruction(
                    op, start, w & 31, immediate=imm << shift if op == Op.CONST else imm, shift=shift
                )
            elif w & 0xFFC00000 in (0x11000000, 0x51000000):
                i = Instruction(
                    Op.ADD if w >> 30 & 1 == 0 else Op.SUB, start, w & 31, w >> 5 & 31, w >> 10 & 4095
                )
            else:
                raise Unsupported(f"ARM64 instruction 0x{w:08x} at +0x{start:x} is not in the proven subset")
        elif not thumb:
            if p + 4 > len(code):
                raise Unsupported("truncated ARM instruction")
            i = _decode_arm32(struct.unpack_from("<I", code, p)[0], start)
            p += 4
        else:
            if p + 2 > len(code):
                raise Unsupported("truncated Thumb instruction")
            w = struct.unpack_from("<H", code, p)[0]
            p += 2
            if w & 0xF800 in THUMB_32BIT:
                if p + 2 > len(code):
                    raise Unsupported("truncated Thumb-2 instruction")
                second = struct.unpack_from("<H", code, p)[0]
                if w & 0xFBF0 in (0xF240, 0xF2C0) and not second & 0x8000:
                    p += 2
                    i = _decode_thumb2(w, second, start, architecture)
                else:
                    raise Unsupported(
                        f"Thumb-2 instruction 0x{w:04x} 0x{second:04x} at +0x{start:x} is unsupported"
                    )
            else:
                i = _decode_thumb(w, start)
        # ARM32 SP/LR/PC and ARM64 platform/callee-saved registers never cross the ABI.
        for register in (i.dst, i.src, i.extra):
            if register is not None and not 0 <= register < limit:
                raise Unsupported("special/platform/callee-saved register write")
        if i.op == Op.INSERT and i.dst not in initialized:
            raise Unsupported("read of uninitialized register")
        for register in (i.src, i.extra):
            if register is not None and register not in initialized:
                raise Unsupported("input/stack register dependency is not a closed leaf function")
        if i.op == Op.RETURN and 0 not in initialized:
            raise Unsupported("return value is not initialized")
        if i.dst is not None:
            initialized.add(i.dst)
        instructions.append(i)
        output.extend(code[start:p] if architecture in ARM64 else _emit(i))
        if i.op == Op.RETURN:
            return Program(architecture, [Block(0, instructions)], bytes(output), p)
    raise Unsupported("entry point does not terminate within 4096 verified instructions")
