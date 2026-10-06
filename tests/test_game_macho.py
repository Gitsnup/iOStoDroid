"""Exact Mach-O parsing for the playable-game path, proven on Angry Birds 1.0."""

import unittest
import zipfile
from pathlib import Path

from radek.game import macho

DATA = Path(__file__).resolve().parent / "data" / "AngryBirds_v1.0_os30.ipa"


def load_image():
    with zipfile.ZipFile(DATA) as archive:
        raw = archive.read("Payload/AngryBirds.app/AngryBirds")
    return macho.parse(raw)


class GameMachOTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image = load_image()

    def test_header_and_segments(self):
        image = self.image
        self.assertEqual(image.cputype, macho.CPU_TYPE_ARM)
        self.assertEqual(image.filetype, macho.MH_EXECUTE)
        self.assertFalse(image.encrypted)
        names = [segment.name for segment in image.segments]
        self.assertEqual(names, ["__PAGEZERO", "__TEXT", "__DATA", "__LINKEDIT"])
        text = image.segments[1]
        self.assertEqual((text.vmaddr, text.file_size), (0x1000, 1302528))

    def test_thread_entry(self):
        self.assertEqual(self.image.thread_entry.get("pc"), 0x4320)

    def test_dependencies(self):
        self.assertEqual(len(self.image.dependencies), 12)
        self.assertIn("/usr/lib/libobjc.A.dylib", self.image.dependencies)
        self.assertIn(
            "/System/Library/Frameworks/OpenGLES.framework/OpenGLES", self.image.dependencies
        )

    def test_symbols_and_imports(self):
        image = self.image
        self.assertEqual(len(image.symbols), 14853)
        self.assertEqual(len(image.undefined_symbols), 254)
        names = {symbol.name for symbol in image.undefined_symbols}
        self.assertIn("_UIApplicationMain", names)
        self.assertIn("_objc_msgSend", names)
        self.assertIn("__ZdaPv", names)
        main = image.symbol_by_name("_main")
        self.assertIsNotNone(main)
        self.assertEqual(main.value, 0x74358)

    def test_stub_map(self):
        entries = macho.stub_map(self.image)
        self.assertEqual(len(entries), 220)
        by_name = {name: (stub, slot) for stub, slot, name in entries}
        self.assertEqual(by_name["_objc_msgSend"], (0x132CF0, 0x13F3E8))
        self.assertEqual(by_name["_UIApplicationMain"], (0x1325C4, 0x13F184))
        self.assertEqual(by_name["_main"] if "_main" in by_name else None, None)

    def test_non_lazy_map(self):
        entries = macho.non_lazy_map(self.image)
        self.assertEqual(len(entries), 77)
        imports = {name for _, name in entries if name}
        self.assertEqual(
            imports,
            {
                "_exit",
                "___gxx_personality_sj0",
                "__DefaultRuneLocale",
                "_kEAGLDrawablePropertyRetainedBacking",
                "_kEAGLColorFormatRGB565",
                "_kEAGLColorFormatRGBA8",
                "_kEAGLDrawablePropertyColorFormat",
                "___stderrp",
                "___stdinp",
                "___stdoutp",
            },
        )

    def test_external_relocations(self):
        image = self.image
        self.assertEqual(len(image.external_relocations), 348)
        self.assertEqual(len(image.local_relocations), 0)
        for reloc in image.external_relocations:
            self.assertFalse(reloc.pcrel)
            self.assertEqual(reloc.length, 2)
            self.assertEqual(reloc.type, macho.RELOC_VANILLA)
        # Every relocation must reference a real undefined symbol.
        undefined = {symbol.index for symbol in image.undefined_symbols}
        for reloc in image.external_relocations:
            self.assertIn(reloc.symbol_index, undefined)

    def test_section_reads(self):
        image = self.image
        self.assertEqual(image.read_u32(0x14365C), 0)  # bound-at-load classref
        self.assertEqual(image.read(0x1456D0, 16), bytes(16))  # zerofill
        text = image.read_cstring(0x132FFC)
        self.assertTrue(text)  # selector name storage

    def test_rejects_non_arm(self):
        with self.assertRaises(macho.MachOError):
            macho.parse(b"short")
        with self.assertRaises(macho.MachOError):
            macho.parse(b"\xcf\xfa\xed\xfe" + bytes(64))


if __name__ == "__main__":
    unittest.main()
