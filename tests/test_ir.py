import struct
import unittest
from radek.ir import *


class IRTests(unittest.TestCase):
    def test_arm64_preserved(self):
        code = struct.pack('<III', 0x52800500, 0x11000800, 0xd65f03c0)
        p = lift(code, 'arm64'); self.assertEqual(p.machine_code, code)
        self.assertEqual([i.op for i in p.blocks[0].instructions], [Op.CONST, Op.ADD, Op.RETURN])
    def test_arm32_offline_lowering(self):
        p = lift(struct.pack('<III', 0xe3a00028, 0xe2800002, 0xe12fff1e), 'armv7')
        self.assertEqual(p.machine_code, struct.pack('<III', 0x52800500, 0x11000800, 0xd65f03c0))
    def test_thumb_offline_lowering(self):
        p = lift(struct.pack('<HHHH', 0x2029, 0x3002, 0x3801, 0x4770), 'armv7', True)
        self.assertEqual(len(p.machine_code), 16); self.assertEqual(p.blocks[0].instructions[2].op, Op.SUB)
    def test_thumb2_movw_movt(self):
        p = lift(struct.pack('<HHHHH', 0xf241, 0x2034, 0xf2c5, 0x6078, 0x4770), 'armv7s', True)
        self.assertEqual(p.blocks[0].instructions[0].immediate, 0x1234)
        self.assertEqual(p.blocks[0].instructions[1].immediate, 0x5678)
        self.assertEqual(p.machine_code, struct.pack('<III', 0x52824680, 0x72aacf00, 0xd65f03c0))
    def test_arm_rotated_constant(self):
        p = lift(struct.pack('<II', 0xe3a004ff, 0xe12fff1e), 'armv7')
        self.assertEqual(p.blocks[0].instructions[0].immediate, 0xff000000)
    def test_uninitialized_inputs(self):
        for code in (struct.pack('<I', 0xd65f03c0), struct.pack('<II', 0x11000420, 0xd65f03c0), struct.pack('<II', 0x72800020, 0xd65f03c0)):
            with self.assertRaises(Unsupported): lift(code, 'arm64')
    def test_memory_syscalls_pac_calls_branches_blocked(self):
        for word in (0xd4000001, 0xf9400000, 0xd503233f, 0x94000000, 0x14000000, 0x52800013, 0x5280001f):
            with self.subTest(word=word), self.assertRaises(Unsupported): lift(struct.pack('<II', word, 0xd65f03c0), 'arm64')
    def test_arm64e_blocked(self):
        with self.assertRaises(Unsupported): lift(b'', 'arm64e')
    def test_truncated_and_no_return(self):
        for code in (b'\x00', struct.pack('<I', 0x52800020)):
            with self.assertRaises(Unsupported): lift(code, 'arm64')
    def test_thumb_it_and_branch_blocked(self):
        for word in (0xbf08, 0xe000, 0x4800):
            with self.assertRaises(Unsupported): lift(struct.pack('<HH', word, 0x4770), 'armv7', True)
    def test_invalid_thumb2(self):
        with self.assertRaises(Unsupported): lift(struct.pack('<HH', 0xf240, 0x8000), 'armv7', True)
    def test_arithmetic_modulo_width(self):
        code = struct.pack('<IIII', 0x529fffe0, 0x72bfffe0, 0x11000400, 0xd65f03c0)
        p = lift(code, 'arm64'); self.assertEqual(p.machine_code, code)
