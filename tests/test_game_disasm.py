"""Literal-aware disassembly for the playable-game path."""

import unittest
import zipfile
from pathlib import Path

from radek.game import disasm, macho

DATA = Path(__file__).resolve().parent / "data" / "AngryBirds_v1.0_os30.ipa"

try:
    import capstone  # noqa: F401

    HAVE_CAPSTONE = True
except ImportError:
    HAVE_CAPSTONE = False


@unittest.skipUnless(HAVE_CAPSTONE, "capstone is required for the game disassembler")
class GameDisasmTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with zipfile.ZipFile(DATA) as archive:
            cls.image = macho.parse(archive.read("Payload/AngryBirds.app/AngryBirds"))

    def test_main_literals_all_resolve_to_pointers(self):
        func = disasm.disassemble_function(self.image, 0x74358, "_main")
        self.assertEqual(len(func.instructions), 21)
        self.assertEqual({lit.verdict for lit in func.literals}, {"POINTER"})
        values = {lit.value for lit in func.literals}
        # NSAutoreleasePool classref/selref, delegate CFString, release selref.
        self.assertEqual(values, {0x143620, 0x14365C, 0x141410, 0x143564})

    def test_mainloop_discovers_landing_pad(self):
        functions = disasm.disassemble_all(self.image)
        pads = [addr for addr in functions if "$pad" in functions[addr].name]
        self.assertTrue(pads, "SjLj landing pads must be discovered via context fills")
        # The mainloop dispatcher reads call_site/selector from the context.
        found = any(
            any("ldr" in insn[1] and "[sp" in insn[2] for insn in functions[addr].instructions)
            for addr in pads
        )
        self.assertTrue(found)

    def test_entries_cover_text(self):
        entries = disasm.function_entries(self.image)
        self.assertGreater(len(entries), 2000)
        self.assertIn(0x4320, entries)  # start
        self.assertIn(0x74358, entries)  # _main


if __name__ == "__main__":
    unittest.main()
