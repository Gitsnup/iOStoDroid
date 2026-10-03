import struct
import unittest
from radek.ir import *


class IRTests(unittest.TestCase):
    def test_arm64_preserved(self):
        code = struct.pack("<III", 0x52800500, 0x11000800, 0xD65F03C0)
        p = lift(code, "arm64")
        self.assertEqual(p.machine_code, code)
        self.assertEqual([i.op for i in p.blocks[0].instructions], [Op.CONST, Op.ADD, Op.RETURN])

    def test_arm32_offline_lowering(self):
        p = lift(struct.pack("<III", 0xE3A00028, 0xE2800002, 0xE12FFF1E), "armv7")
        self.assertEqual(p.machine_code, struct.pack("<III", 0x52800500, 0x11000800, 0xD65F03C0))

    def test_thumb_offline_lowering(self):
        p = lift(struct.pack("<HHHH", 0x2029, 0x3002, 0x3801, 0x4770), "armv7", True)
        self.assertEqual(len(p.machine_code), 16)
        self.assertEqual(p.blocks[0].instructions[2].op, Op.SUB)

    def test_thumb2_movw_movt(self):
        p = lift(struct.pack("<HHHHH", 0xF241, 0x2034, 0xF2C5, 0x6078, 0x4770), "armv7s", True)
        self.assertEqual(p.blocks[0].instructions[0].immediate, 0x1234)
        self.assertEqual(p.blocks[0].instructions[1].immediate, 0x5678)
        self.assertEqual(p.machine_code, struct.pack("<III", 0x52824680, 0x72AACF00, 0xD65F03C0))

    def test_arm_rotated_constant(self):
        p = lift(struct.pack("<II", 0xE3A004FF, 0xE12FFF1E), "armv7")
        self.assertEqual(p.blocks[0].instructions[0].immediate, 0xFF000000)

    def test_uninitialized_inputs(self):
        for code in (
            struct.pack("<I", 0xD65F03C0),
            struct.pack("<II", 0x11000420, 0xD65F03C0),
            struct.pack("<II", 0x72800020, 0xD65F03C0),
        ):
            with self.assertRaises(Unsupported):
                lift(code, "arm64")

    def test_memory_syscalls_pac_calls_branches_blocked(self):
        for word in (0xD4000001, 0xF9400000, 0xD503233F, 0x94000000, 0x14000000, 0x52800013, 0x5280001F):
            with self.subTest(word=word), self.assertRaises(Unsupported):
                lift(struct.pack("<II", word, 0xD65F03C0), "arm64")

    def test_arm64e_blocked(self):
        with self.assertRaises(Unsupported):
            lift(b"", "arm64e")

    def test_truncated_and_no_return(self):
        for code in (b"\x00", struct.pack("<I", 0x52800020)):
            with self.assertRaises(Unsupported):
                lift(code, "arm64")

    def test_thumb_it_and_branch_blocked(self):
        for word in (0xBF08, 0xE000, 0x4800):
            with self.assertRaises(Unsupported):
                lift(struct.pack("<HH", word, 0x4770), "armv7", True)

    def test_invalid_thumb2(self):
        with self.assertRaises(Unsupported):
            lift(struct.pack("<HH", 0xF240, 0x8000), "armv7", True)

    def test_arithmetic_modulo_width(self):
        code = struct.pack("<IIII", 0x529FFFE0, 0x72BFFFE0, 0x11000400, 0xD65F03C0)
        p = lift(code, "arm64")
        self.assertEqual(p.machine_code, code)


class Arm32BroadeningTests(unittest.TestCase):
    """armv4t/armv5tej/armv6/armv7 slices lower to real ARM64 instructions."""

    def lower(self, words, architecture, thumb=False):
        packed = struct.pack("<%d%s" % (len(words), "H" if thumb else "I"), *words)
        return lift(packed, architecture, thumb)

    def test_armv6_a32_leaf(self):
        program = self.lower([0xE3A0002A, 0xE12FFF1E], "armv6")
        self.assertEqual(program.machine_code, struct.pack("<II", 0x52800540, 0xD65F03C0))
        self.assertEqual(program.report()["backend"], "offline-arm32-to-arm64")

    def test_armv6_thumb_leaf(self):
        program = self.lower([0x202A, 0x4770], "armv6", thumb=True)
        self.assertEqual(program.machine_code, struct.pack("<II", 0x52800540, 0xD65F03C0))

    def test_every_arm32_flavour_is_accepted(self):
        for architecture in ("armv4t", "armv5tej", "armv6", "armv7", "armv7s", "armv8-32"):
            with self.subTest(architecture=architecture):
                self.lower([0xE3A00007, 0xE12FFF1E], architecture)

    def test_a32_register_and_logic_lowering(self):
        cases = {
            "MOV R0, R1": ([0xE3A01007, 0xE1A00001, 0xE12FFF1E], [0x528000E1, 0x2A0103E0, 0xD65F03C0]),
            "MVN R0, R1": ([0xE3A01007, 0xE1E00001, 0xE12FFF1E], [0x528000E1, 0x2A2103E0, 0xD65F03C0]),
            "ADD R0, R1, R2": (
                [0xE3A01003, 0xE3A02004, 0xE0810002, 0xE12FFF1E],
                [0x52800061, 0x52800082, 0x0B020020, 0xD65F03C0],
            ),
            "SUB R0, R1, R2": (
                [0xE3A01003, 0xE3A02001, 0xE0410002, 0xE12FFF1E],
                [0x52800061, 0x52800022, 0x4B020020, 0xD65F03C0],
            ),
            "ORR R0, R1, R2": (
                [0xE3A01003, 0xE3A02001, 0xE1810002, 0xE12FFF1E],
                [0x52800061, 0x52800022, 0x2A020020, 0xD65F03C0],
            ),
            "MUL R0, R1, R2": (
                [0xE3A01003, 0xE3A02001, 0xE0000291, 0xE12FFF1E],
                [0x52800061, 0x52800022, 0x1B017C40, 0xD65F03C0],
            ),
            "MOV R0, R1, LSL #3": (
                [0xE3A01003, 0xE1A00181, 0xE12FFF1E],
                [0x52800061, 0x531D7020, 0xD65F03C0],
            ),
        }
        for name, (words, expected) in cases.items():
            with self.subTest(name=name):
                program = self.lower(words, "armv6")
                self.assertEqual(program.machine_code, struct.pack("<%dI" % len(expected), *expected))

    def test_a32_logical_immediate_uses_free_scratch_register(self):
        # AND R0, R1, #15 has no ARM bitmask-immediate encoding for every value,
        # so it is materialised in w15 (untouchable by ARM32 leaf code) and applied.
        program = self.lower([0xE3A010FF, 0xE201000F, 0xE12FFF1E], "armv6")
        self.assertEqual(
            program.machine_code, struct.pack("<IIII", 0x52801FE1, 0x528001EF, 0x0A0F0020, 0xD65F03C0)
        )

    def test_thumb_register_group_is_decoded_by_six_op_bits(self):
        cases = {
            0x4000: Op.AND, 0x4040: Op.EOR, 0x4300: Op.ORR,
            0x4340: Op.MUL, 0x4380: Op.BIC, 0x43C0: Op.MVN,
        }
        for word, op in cases.items():
            with self.subTest(word=hex(word)):
                program = self.lower([0x2007, word, 0x4770], "armv6", thumb=True)
                self.assertEqual(program.blocks[0].instructions[1].op, op)

    def test_thumb_shift_and_move(self):
        program = self.lower([0x2105, 0x0088, 0x4608, 0x4770], "armv6", thumb=True)
        ops = [i.op for i in program.blocks[0].instructions]
        self.assertEqual(ops, [Op.CONST, Op.SHIFT, Op.MOVE, Op.RETURN])
        self.assertEqual(program.blocks[0].instructions[1].immediate, 2)

    def test_thumb_three_bit_immediate_add(self):
        program = self.lower([0x2001, 0x1C41, 0x4770], "armv6", thumb=True)
        self.assertEqual(program.blocks[0].instructions[1].op, Op.ADD)
        self.assertEqual(program.blocks[0].instructions[1].immediate, 1)

    def test_unproven_arm32_operations_are_rejected(self):
        rejected = [
            ([0xE3A01000, 0xE1510001, 0xE12FFF1E], False),        # CMP + flags
            ([0xE3A01000, 0xE02DD191, 0xE12FFF1E], False),        # multiply-accumulate
            ([0xE3A01000, 0xE1A00111, 0xE12FFF1E], False),        # MOV ROR
            ([0xE3A01000, 0x1A000000, 0xE12FFF1E], False),        # conditional branch
            ([0xE3A01000, 0xE24DD004, 0xE12FFF1E], False),        # SUB SP (stack)
            ([0x2007, 0x4140, 0x4770], True),                     # ADCS
            ([0x2007, 0x40C0, 0x4770], True),                     # LSRS register
            ([0x2007, 0x4280, 0x4770], True),                     # CMP register
        ]
        for words, thumb in rejected:
            with self.subTest(words=[hex(w) for w in words]), self.assertRaises(Unsupported):
                self.lower(words, "armv6", thumb)
