package dev.radek.conventor

/**
 * Offline leaf lifting on the device. Mirrors `radek/ir.py`: a straight-line
 * closed integer function is *proved* instruction by instruction and re-emitted
 * as real Android ARM64 machine code. Guest instructions are decoded, never run.
 *
 * Anything outside the proven subset raises [Unsupported] so the caller can
 * report it honestly instead of shipping a mistranslation.
 */
object Ir {
    class Unsupported(message: String) : RuntimeException(message)

    val ARM32 = setOf(
        "armv4t", "armv5tej", "armv6", "armv6m", "armv7", "armv7f", "armv7s",
        "armv7k", "armv7m", "armv7em", "armv8-32", "arm32-unknown"
    )

    /** Scratch register for ARM32 immediate logical operands. ARM32 leaf code may
     *  only touch r0-r12, so w13/w14/w15 are provably free. */
    private const val SCRATCH = 15
    private const val MAX_INSTRUCTIONS = 4096

    enum class Op { CONST, INSERT, MOVE, MVN, ADD, SUB, AND, ORR, EOR, BIC, MUL, SHIFT, RETURN }

    class Instruction(
        val op: Op,
        val dst: Int? = null,
        val src: Int? = null,
        val immediate: Long = 0,
        val shift: Int = 0,
        val extra: Int? = null
    )

    class Program(val architecture: String, val instructions: List<Instruction>, val machineCode: ByteArray, val sourceBytes: Int)

    private fun word(value: Int) = byteArrayOf(
        (value and 0xFF).toByte(), (value shr 8 and 0xFF).toByte(),
        (value shr 16 and 0xFF).toByte(), (value shr 24 and 0xFF).toByte()
    )

    private fun u32(code: ByteArray, at: Int): Int =
        (code[at].toInt() and 255) or ((code[at + 1].toInt() and 255) shl 8) or
            ((code[at + 2].toInt() and 255) shl 16) or ((code[at + 3].toInt() and 255) shl 24)

    private fun u16(code: ByteArray, at: Int): Int =
        (code[at].toInt() and 255) or ((code[at + 1].toInt() and 255) shl 8)

    /** MOVZ/MOVK pair materialising a 32-bit constant. */
    private fun constant(dst: Int, value: Long): List<Int> {
        val v = value.toInt()
        val words = mutableListOf(0x52800000 or ((v and 0xFFFF) shl 5) or dst)
        if ((v ushr 16) != 0) words.add(0x72A00000 or ((v ushr 16) shl 5) or dst)
        return words
    }

    private fun logical(base: Int, dst: Int, src: Int, operand: Int) =
        base or (operand shl 16) or (src shl 5) or dst

    /** Lower one proved instruction to real ARM64 words. */
    private fun emit(i: Instruction): ByteArray {
        val dst = i.dst ?: 0
        val src = i.src ?: 0
        val words = mutableListOf<Int>()
        when (i.op) {
            Op.CONST -> words.addAll(constant(dst, i.immediate shl i.shift))
            Op.INSERT -> words.add(0x72800000 or ((i.shift / 16) shl 21) or (i.immediate.toInt() shl 5) or dst)
            Op.MOVE -> words.add(0x2A0003E0 or (src shl 16) or dst)          // ORR Wd, WZR, Wm
            Op.MVN -> words.add(0x2A2003E0 or (src shl 16) or dst)           // ORN Wd, WZR, Wm
            Op.ADD, Op.SUB -> if (i.extra != null) {
                val base = if (i.op == Op.ADD) 0x0B000000 else 0x4B000000
                words.add(base or (i.extra shl 16) or (src shl 5) or dst)
            } else {
                if (i.immediate < 0 || i.immediate > 4095) throw Unsupported("immediate outside ADD/SUB range")
                val base = if (i.op == Op.ADD) 0x11000000 else 0x51000000
                words.add(base or (i.immediate.toInt() shl 10) or (src shl 5) or dst)
            }
            Op.AND, Op.ORR, Op.EOR, Op.BIC -> {
                val base = when (i.op) {
                    Op.AND -> 0x0A000000
                    Op.ORR -> 0x2A000000
                    Op.EOR -> 0x4A000000
                    else -> 0x0A200000
                }
                if (i.extra != null) words.add(logical(base, dst, src, i.extra))
                else {
                    words.addAll(constant(SCRATCH, i.immediate))
                    words.add(logical(base, dst, src, SCRATCH))
                }
            }
            Op.MUL -> words.add(0x1B007C00 or ((i.extra ?: 0) shl 16) or (src shl 5) or dst)
            Op.SHIFT -> {
                val amount = (i.immediate.toInt() and 31)
                if (amount < 1 || amount > 31) throw Unsupported("zero-amount shift is not an instruction")
                words.add(
                    when (i.shift) {
                        0 -> 0x53000000 or ((32 - amount) shl 16) or ((31 - amount) shl 10) or (src shl 5) or dst
                        1 -> 0x53000000 or (amount shl 16) or (31 shl 10) or (src shl 5) or dst
                        2 -> 0x13000000 or (amount shl 16) or (31 shl 10) or (src shl 5) or dst
                        else -> throw Unsupported("ROR has no verified single-instruction ARM64 lowering")
                    }
                )
            }
            Op.RETURN -> words.add(0xD65F03C0)
        }
        val out = ByteArray(words.size * 4)
        words.forEachIndexed { index, value -> word(value).copyInto(out, index * 4) }
        return out
    }

    /** ARM32 immediate operand: 8-bit value rotated right by 2*rotate. */
    private fun rotated(imm8: Int, rotate: Int): Long {
        val amount = rotate * 2
        if (amount == 0) return imm8.toLong() and 0xFFL
        val v = imm8.toLong() and 0xFFL
        return ((v ushr amount) or (v shl (32 - amount))) and 0xFFFFFFFFL
    }

    private fun arm32(w: Int, at: Int): Instruction {
        if (w == 0xE12FFF1E.toInt()) return Instruction(Op.RETURN)          // BX LR
        if ((w ushr 28) != 14) throw Unsupported("conditional ARM instruction 0x${Integer.toHexString(w)} at +0x${Integer.toHexString(at)}")
        val body = w and 0x0FFFFFFF
        if (body and 0x0FE0F0F0 == 0x00000090) {                            // MUL Rd, Rm, Rs
            return Instruction(Op.MUL, dst = w shr 16 and 15, src = w shr 8 and 15, extra = w and 15)
        }
        if ((body ushr 25) and 7 <= 1) {                                     // data processing
            val opcode = body shr 21 and 15
            val dst = w shr 12 and 15
            val src = w shr 16 and 15
            val immediate = (body ushr 25) and 7 == 1
            val operand = w and 0xFFF
            if (!immediate && operand and 0x10 != 0) {
                throw Unsupported("ARM instruction 0x${Integer.toHexString(w)} at +0x${Integer.toHexString(at)} is unsupported")
            }
            if (opcode == 13 && src == 0) {                                  // MOV
                if (immediate) return Instruction(Op.CONST, dst = dst, immediate = rotated(w and 255, w shr 8 and 15))
                if (operand and 0xFF0 == 0) return Instruction(Op.MOVE, dst = dst, src = operand and 15)
                val kind = operand shr 5 and 3
                val amount = operand shr 7 and 31
                if (operand and 0x10 != 0 || kind == 3 || amount == 0) {
                    throw Unsupported("ARM operand 0x${Integer.toHexString(operand)} is outside the proven subset")
                }
                return Instruction(Op.SHIFT, dst = dst, src = operand and 15, immediate = amount.toLong(), shift = kind)
            }
            if (opcode == 15 && src == 0) {                                  // MVN
                if (immediate) {
                    return Instruction(Op.CONST, dst = dst, immediate = rotated(w and 255, w shr 8 and 15).inv() and 0xFFFFFFFFL)
                }
                if (operand and 0xFF0 != 0) throw Unsupported("shifted MVN operand is outside the proven subset")
                return Instruction(Op.MVN, dst = dst, src = w and 15)
            }
            if (opcode == 2 || opcode == 4) {                                // SUB / ADD
                val op = if (opcode == 2) Op.SUB else Op.ADD
                if (immediate) return Instruction(op, dst = dst, src = src, immediate = rotated(w and 255, w shr 8 and 15))
                if (operand and 0xFF0 == 0) return Instruction(op, dst = dst, src = src, extra = operand and 15)
                throw Unsupported("shifted-register ADD/SUB operand is outside the proven subset")
            }
            val logicalOp = when (opcode) { 0 -> Op.AND; 1 -> Op.EOR; 12 -> Op.ORR; 14 -> Op.BIC; else -> null }
            if (logicalOp != null) {
                if (immediate) return Instruction(logicalOp, dst = dst, src = src, immediate = rotated(w and 255, w shr 8 and 15))
                if (operand and 0xFF0 == 0) return Instruction(logicalOp, dst = dst, src = src, extra = operand and 15)
                throw Unsupported("shifted logical operand is outside the proven subset")
            }
            throw Unsupported("ARM data processing operation outside proven subset")
        }
        throw Unsupported("ARM instruction 0x${Integer.toHexString(w)} at +0x${Integer.toHexString(at)} is unsupported")
    }

    private fun thumb(w: Int, at: Int): Instruction {
        if (w == 0x4770) return Instruction(Op.RETURN)                       // BX LR
        if (w and 0xF800 == 0x2000) return Instruction(Op.CONST, dst = w shr 8 and 7, immediate = (w and 255).toLong())
        if (w and 0xF800 == 0x3000) return Instruction(Op.ADD, dst = w shr 8 and 7, src = w shr 8 and 7, immediate = (w and 255).toLong())
        if (w and 0xF800 == 0x3800) return Instruction(Op.SUB, dst = w shr 8 and 7, src = w shr 8 and 7, immediate = (w and 255).toLong())
        if (w and 0xF800 == 0x0000) {                                        // LSL/LSR/ASR #imm5
            val kind = w shr 11 and 3
            val amount = w shr 6 and 31
            if (kind == 3 || amount == 0) throw Unsupported("Thumb shift 0x${Integer.toHexString(w)} is outside the proven subset")
            return Instruction(Op.SHIFT, dst = w and 7, src = w shr 3 and 7, immediate = amount.toLong(), shift = kind)
        }
        if (w and 0xFC00 == 0x1800 || w and 0xFC00 == 0x1A00) {              // ADD/SUB Rd, Rn, Rm
            val op = if (w and 0xFC00 == 0x1800) Op.ADD else Op.SUB
            return Instruction(op, dst = w and 7, src = w shr 3 and 7, extra = w shr 6 and 7)
        }
        if (w and 0xFC00 == 0x1C00 || w and 0xFC00 == 0x1E00) {              // ADD/SUB Rd, Rn, #imm3
            val op = if (w and 0xFC00 == 0x1C00) Op.ADD else Op.SUB
            return Instruction(op, dst = w and 7, src = w shr 3 and 7, immediate = (w shr 6 and 7).toLong())
        }
        if (w and 0xFF00 == 0x4600) {                                        // MOV Rd, Rm
            return Instruction(Op.MOVE, dst = ((w shr 4) and 8) or (w and 7), src = (w shr 3) and 15)
        }
        // T1 data-processing register group: 0100 00 op(9-6) Rm Rd
        val group = w and 0xFFC0
        val dst = w and 7
        val src = w shr 3 and 7
        when (group) {
            0x4000 -> return Instruction(Op.AND, dst = dst, src = src, extra = src)
            0x4040 -> return Instruction(Op.EOR, dst = dst, src = src, extra = src)
            0x4300 -> return Instruction(Op.ORR, dst = dst, src = src, extra = src)
            0x4340 -> return Instruction(Op.MUL, dst = dst, src = src, extra = src)
            0x4380 -> return Instruction(Op.BIC, dst = dst, src = src, extra = src)
            0x43C0 -> return Instruction(Op.MVN, dst = dst, src = src)
        }
        throw Unsupported(
            "Thumb instruction 0x${Integer.toHexString(w)} at +0x${Integer.toHexString(at)} is unsupported " +
                "(branches/IT/register shifts require a CFG backend)"
        )
    }

    private fun thumb2(first: Int, second: Int): Instruction {
        if (second and 0x8000 != 0) throw Unsupported("invalid Thumb-2 MOVW/MOVT")
        val imm = (((first and 15) shl 12) or ((first shr 10 and 1) shl 11) or
            ((second shr 12 and 7) shl 8) or (second and 255)).toLong()
        val dst = second shr 8 and 15
        return if (first and 0xFBF0 == 0xF2C0) Instruction(Op.INSERT, dst = dst, immediate = imm, shift = 16)
        else Instruction(Op.CONST, dst = dst, immediate = imm)
    }

    /**
     * Prove the entry leaf and return real Android ARM64 bytes.
     * @param thumbMode true when the entry symbol carries N_ARM_THUMB_DEF.
     */
    fun lift(code: ByteArray, architecture: String, thumbMode: Boolean): Program {
        val isArm32 = architecture in ARM32
        if (architecture != "arm64" && !isArm32) throw Unsupported("no safe backend for $architecture")
        val thumb = isArm32 && thumbMode
        val limit = if (architecture == "arm64") 16 else 13
        val instructions = mutableListOf<Instruction>()
        val initialized = mutableSetOf<Int>()
        val output = java.io.ByteArrayOutputStream()
        var p = 0
        while (p < code.size && instructions.size < MAX_INSTRUCTIONS) {
            val i: Instruction
            var consumed: Int
            if (architecture == "arm64") {
                if (p + 4 > code.size) throw Unsupported("truncated ARM64 instruction")
                val w = u32(code, p)
                consumed = 4
                i = when {
                    w == 0xD65F03C0.toInt() -> Instruction(Op.RETURN)
                    w and 0xFF800000.toInt() == 0x52800000 || w and 0xFF800000.toInt() == 0x72800000 -> {
                        val shift = (w shr 21 and 3) * 16
                        if (shift > 16) throw Unsupported("invalid 32-bit MOV encoding")
                        val imm = ((w shr 5) and 0xFFFF).toLong()
                        if (w and 0xFF800000.toInt() == 0x52800000) {
                            Instruction(Op.CONST, dst = w and 31, immediate = imm shl shift)
                        } else Instruction(Op.INSERT, dst = w and 31, immediate = imm, shift = shift)
                    }
                    w and 0xFFC00000.toInt() == 0x11000000 || w and 0xFFC00000.toInt() == 0x51000000 -> {
                        val op = if ((w shr 30) and 1 == 0) Op.ADD else Op.SUB
                        Instruction(op, dst = w and 31, src = w shr 5 and 31, immediate = ((w shr 10) and 4095).toLong())
                    }
                    else -> throw Unsupported("ARM64 instruction 0x${Integer.toHexString(w)} at +0x${Integer.toHexString(p)} is not in the proven subset")
                }
            } else if (!thumb) {
                if (p + 4 > code.size) throw Unsupported("truncated ARM instruction")
                i = arm32(u32(code, p), p)
                consumed = 4
            } else {
                if (p + 2 > code.size) throw Unsupported("truncated Thumb instruction")
                val w = u16(code, p)
                if (w and 0xF800 == 0xE800 || w and 0xF800 == 0xF000 || w and 0xF800 == 0xF800) {
                    if (p + 4 > code.size) throw Unsupported("truncated Thumb-2 instruction")
                    val second = u16(code, p + 2)
                    if ((w and 0xFBF0 == 0xF240 || w and 0xFBF0 == 0xF2C0) && second and 0x8000 == 0) {
                        i = thumb2(w, second)
                        consumed = 4
                    } else throw Unsupported(
                        "Thumb-2 instruction 0x${Integer.toHexString(w)} 0x${Integer.toHexString(second)} is unsupported"
                    )
                } else {
                    i = thumb(w, p)
                    consumed = 2
                }
            }
            // ARM32 SP/LR/PC and ARM64 platform/callee-saved registers never cross the ABI.
            for (register in listOf(i.dst, i.src, i.extra)) {
                if (register != null && (register < 0 || register >= limit)) {
                    throw Unsupported("special/platform/callee-saved register write")
                }
            }
            if (i.op == Op.INSERT && (i.dst == null || !initialized.contains(i.dst))) {
                throw Unsupported("read of uninitialized register")
            }
            for (register in listOf(i.src, i.extra)) {
                if (register != null && register !in initialized) {
                    throw Unsupported("input/stack register dependency is not a closed leaf function")
                }
            }
            if (i.op == Op.RETURN && 0 !in initialized) throw Unsupported("return value is not initialized")
            i.dst?.let { initialized.add(it) }
            instructions.add(i)
            output.write(if (architecture == "arm64") code.copyOfRange(p, p + consumed) else emit(i))
            p += consumed
            if (i.op == Op.RETURN) {
                return Program(architecture, instructions, output.toByteArray(), p)
            }
        }
        throw Unsupported("entry point does not terminate within $MAX_INSTRUCTIONS verified instructions")
    }
}
