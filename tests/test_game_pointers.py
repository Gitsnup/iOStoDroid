"""Pointer-model section scan for the playable-game path.

Pins the data-section triage: loader tables in __TEXT, exact-start SLIDE
rule, struct-holder mid-pointer rule, and the packed-table-int demotions.
"""

import unittest
import zipfile
from pathlib import Path

from iostodroid.game import disasm, macho, objc_meta, pointers

DATA = Path(__file__).resolve().parent / "data" / "AngryBirds_v1.0_os30.ipa"

try:
    import capstone  # noqa: F401

    HAVE_CAPSTONE = True
except ImportError:
    HAVE_CAPSTONE = False


GENERIC = {
    ("__DATA", "__data"),
    ("__DATA", "__const"),
    ("__DATA", "__dyld"),
    ("__DATA", "__cfstring"),
    ("__TEXT", "__const"),
    ("__TEXT", "__dyld"),
    ("__TEXT", "__mod_init_func"),
    ("__TEXT", "__nl_symbol_ptr"),
    ("__TEXT", "__la_symbol_ptr"),
}


@unittest.skipUnless(HAVE_CAPSTONE, "capstone is required for the game pipeline")
class GamePointersTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with zipfile.ZipFile(DATA) as archive:
            cls.image = macho.parse(archive.read("Payload/AngryBirds.app/AngryBirds"))
        cls.funcs = disasm.disassemble_all(cls.image)
        cls.targets = pointers.build_target_map(cls.image, cls.funcs)
        cls.model = pointers.build_model(cls.image, cls.funcs)

    def test_model_totals(self):
        self.assertEqual(
            self.model.stats["sites"],
            {"VALUE": 3685, "SLIDE": 7654, "IMPORT": 546},
        )
        self.assertEqual(self.model.stats["reviews"], 0)
        self.assertEqual(self.model.stats["demotions"], 163)

    def test_no_reviews_at_all(self):
        self.assertEqual(self.model.reviews, [])

    def test_jump_tables_followed(self):
        tables = [
            (func.name, base, count)
            for func in self.funcs.values()
            for base, count in func.jump_tables
        ]
        self.assertEqual(len(tables), 162)
        # 29 tables are shared by tail-merged aliases (same body, 2 names).
        unique = {(base, count) for _, base, count in tables}
        self.assertEqual(len(unique), 127)
        members = set()
        for _, base, count in tables:
            for i in range(count):
                members.add(base + 4 * i)
        self.assertEqual(len(members), 1465)
        for slot in members:
            site = self.model.sites.get(slot)
            self.assertIsNotNone(site, hex(slot))
            self.assertEqual(site.kind, "SLIDE", hex(slot))
        # Every PC-relative dispatch is followed (none aborts).
        for func in self.funcs.values():
            for addr, mnemonic, op_str in func.indirect_branches:
                if "ldr" in mnemonic and "[pc, " in op_str and "lsl #2" in op_str:
                    self.assertTrue(
                        any(base == addr + 8
                            for owner, base, _ in tables
                            if owner == func.name),
                        f"{func.name}@{addr:#x}",
                    )
        # Spot check: getDataSize switch has 16 arms (bound 0xf).
        by_base = {(base, count) for _, base, count in tables}
        self.assertIn((0x3CBAC, 16), by_base)

    def test_indirect_calls_continue(self):
        # `mov lr, pc; ldr pc, ...` returns: decoding continues past it.
        for addr, owner_part in (
            (0x12C1C8, "b2Joint7Destroy"),
            (0x68668, "setOrientation"),
            (0xB3E90, "EGL_Primitive6renderEv"),
            (0x1156F8, "read_frame"),
        ):
            owners = [f for f in self.funcs.values() if addr in f.code_words]
            self.assertTrue(owners, hex(addr))
            self.assertTrue(
                any(owner_part in f.name for f in owners),
                f"{addr:#x}: {[f.name for f in owners]}",
            )
        # Conditional indirects fall through to their bridge branch.
        for func in self.funcs.values():
            for addr, mnemonic, _ in func.indirect_branches:
                if mnemonic != "ldr" and "ldr" in mnemonic:
                    self.assertIn(addr + 4, func.code_words, hex(addr))

    def test_audit_precedence(self):
        # A strong dereference wins over a later dead read of one pool.
        site = self.model.sites[0x1113C8]
        self.assertEqual(site.kind, "SLIDE")
        self.assertIn("dereferenced", site.evidence)
        # Jump-table structure is the only excuse for sliding bad targets.
        for site in self.model.sites.values():
            if site.kind != "SLIDE":
                continue
            target = pointers.classify_target(
                self.image, self.targets, site.value)
            if target.startswith("bad:"):
                self.assertTrue(
                    site.evidence.startswith("jump-table-entry@"),
                    f"@{site.address:#x} {site.evidence}",
                )

    def test_exact_addresses_always_slide(self):
        # Compared registration/sentinel addresses must relocate too.
        for addr in (0x11498, 0x114F8, 0x1159C, 0xBA6C4, 0xBA7F4,
                     0xA0540, 0xA0E4C, 0xA22D0, 0xA2C94, 0xA2E54,
                     0x1168EC, 0x1168FC, 0x10EF14, 0x1113D4):
            self.assertEqual(self.model.sites[addr].kind, "SLIDE", hex(addr))
        # ... except proven small-integer pool-word hits (GL enums etc.).
        for addr in (0x69798, 0xA8C44, 0xB6ED8, 0xD37D0,
                     0x4104, 0xB5C3C, 0xB5FAC, 0xB5C64, 0xE15BC):
            site = self.model.sites[addr]
            self.assertEqual(site.kind, "VALUE", hex(addr))

    def test_full_text_coverage(self):
        code = set()
        for func in self.funcs.values():
            code |= func.code_words
        text = self.image.section_named("__TEXT", "__text")
        missing = []
        for off in range(0, text.size, 4):
            addr = text.address + off
            if addr in code or addr in self.model.sites:
                continue
            if disasm._points_into_image(self.image, self.image.read_u32(addr)):
                missing.append(addr)
        self.assertEqual(missing, [])

    def test_loader_tables_covered(self):
        sites = self.model.sites
        mod_init = [s for s in sites.values() if s.evidence.startswith("mod-init:")]
        self.assertEqual(len(mod_init), 10)
        self.assertTrue(all(s.kind == "SLIDE" for s in mod_init))
        nl = [s for s in sites.values() if s.evidence.startswith("__nl_symbol_ptr")]
        la = [s for s in sites.values() if s.evidence.startswith("__la_symbol_ptr")]
        self.assertEqual(len(nl), 0x134 // 4)
        self.assertEqual(len(la), 0x370 // 4)
        self.assertTrue(all(s.kind == "IMPORT" for s in nl + la))
        # __dyld: image base + argv/env pointers slide, dyld addresses skip.
        for addr in (0x13F008, 0x13F00C, 0x13F010, 0x13F014, 0x13F018):
            self.assertEqual(sites[addr].kind, "SLIDE")
        self.assertNotIn(0x13F000, sites)
        self.assertNotIn(0x13F004, sites)

    def test_struct_mid_pointers_slide(self):
        sites = self.model.sites
        # libmad synthesis table: exact bases plus &table[16] mids.
        for addr in (0x141038, 0x14103C, 0x141040, 0x141044, 0x141048,
                     0x14105C, 0x141060):
            self.assertEqual(sites[addr].kind, "SLIDE", hex(addr))
        for addr in (0x14104C, 0x141050, 0x141054, 0x141058):
            self.assertEqual(sites[addr].kind, "SLIDE", hex(addr))
            self.assertIn("struct-mid", sites[addr].evidence)
        # Misattributed statics after _tab_c1 are code-referenced pointers.
        self.assertEqual(sites[0x14554C].kind, "SLIDE")
        self.assertEqual(sites[0x145550].kind, "SLIDE")
        # luaL libs table opens with the empty (mid-string) name.
        self.assertEqual(sites[0x14023C].kind, "SLIDE")

    def test_objc_protocols_in_data(self):
        objc = objc_meta.parse(self.image)
        outside = {
            addr for addr in objc.sites
            if "objc" not in self.image.section_at(addr).name
        }
        self.assertEqual(len(outside), 30)
        self.assertEqual(min(outside), 0x143CF4)
        self.assertEqual(max(outside), 0x143D68)
        for addr, name in (
            (0x13527C, b"UIApplicationDelegate"),
            (0x135294, b"UIAccelerometerDelegate"),
            (0x135440, b"NSObject"),
        ):
            self.assertEqual(self.image.read(addr, 24).split(bytes([0]))[0], name)
        # Protocol slots are objc-claimed, so the generic scan skips them.
        for addr in (0x143CFC, 0x143D08, 0x143D24, 0x143D30, 0x143D50):
            self.assertEqual(self.model.sites[addr].kind, "SLIDE")
            self.assertTrue(self.model.sites[addr].evidence.startswith("objc:"))

    def test_table_ints_are_values(self):
        sites = self.model.sites
        # Branch-target coincidences inside unicode/ctype tables.
        self.assertEqual(sites[0x144BF4].kind, "VALUE")
        self.assertEqual(sites[0x13EF14].kind, "VALUE")
        # Packed locale-table words aiming at vtables/typeinfo/bss tables.
        for addr in (0x143FD0, 0x14405C, 0x144490, 0x145488):
            self.assertEqual(sites[addr].kind, "VALUE", hex(addr))
        # Samplerate table (44100/48000/...) is pure integers.
        for addr in range(0x13E9E4, 0x13EA08, 4):
            self.assertEqual(sites[addr].kind, "VALUE", hex(addr))
        # Typeinfo-name ASCII read as words.
        self.assertEqual(sites[0x13A1E4].kind, "VALUE")

    def test_struct_holder_set_pinned(self):
        sections = [
            self.image.section_named(*key)
            for key in (("__DATA", "__data"), ("__DATA", "__const"),
                        ("__TEXT", "__const"))
        ]
        holders = pointers._holder_stats(self.image, self.targets, sections)
        struct_with_mid = set()
        for name, (words, exact) in holders.items():
            if words >= 8 and exact >= 3:
                struct_with_mid.add(name)
        self.assertIn("__ZN2io15FileInputStream13sm_fileBundleE", struct_with_mid)
        self.assertIn("_C.68.5500", struct_with_mid)
        # Only these holders carry mid-object words; the ZTI ones are
        # bad:outside-image integers, everything else is a pure table.
        mid_holders = set()
        for section in sections:
            for off in range(0, section.size, 4):
                slot = section.address + off
                site = self.model.sites.get(slot)
                if site is None or site.kind != "SLIDE":
                    continue
                if site.evidence.startswith("objc:"):
                    continue  # structurally parsed protocols, not holder-ruled
                target = pointers.classify_target(
                    self.image, self.targets, site.value)
                if not pointers._is_exact_target(target):
                    contained = pointers.containing_symbol(
                        self.image, self.targets, slot)
                    mid_holders.add(contained[0])
        self.assertEqual(mid_holders, {"_C.68.5500"})

    def test_no_slide_in_table_holders_except_exact(self):
        sections = [
            self.image.section_named(*key)
            for key in (("__DATA", "__data"), ("__DATA", "__const"),
                        ("__TEXT", "__const"))
        ]
        holders = pointers._holder_stats(self.image, self.targets, sections)
        for section in sections:
            for off in range(0, section.size, 4):
                slot = section.address + off
                site = self.model.sites.get(slot)
                if site is None or site.kind != "SLIDE":
                    continue
                if slot in self.targets.reloc_slots:
                    continue
                target = pointers.classify_target(
                    self.image, self.targets, site.value)
                if pointers._is_exact_target(target):
                    continue
                contained = pointers.containing_symbol(
                    self.image, self.targets, slot)
                words, exact = holders[contained[0]]
                self.assertGreaterEqual(words, 8, hex(slot))
                self.assertGreaterEqual(exact, 3, hex(slot))

    def test_every_data_word_accounted(self):
        sites = self.model.sites
        for section in self.image.sections:
            key = (section.segment, section.name)
            if key not in GENERIC:
                continue
            for off in range(0, section.size, 4):
                slot = section.address + off
                word = self.image.read_u32(slot)
                if not disasm._points_into_image(self.image, word):
                    continue
                if section.name in ("__nl_symbol_ptr", "__la_symbol_ptr"):
                    continue  # IMPORT slots, content is dyld-bound
                self.assertIn(slot, sites, f"{key} @{slot:#x}={word:#x}")

    def test_no_movw_movt_anywhere(self):
        """ARMv6: literal pools are the only absolute-address source in code."""
        hits = [
            (func.name, insn[0], insn[1])
            for func in self.funcs.values()
            for insn in func.instructions
            if insn[1] in ("movw", "movt")
        ]
        self.assertEqual(hits, [])


if __name__ == "__main__":
    unittest.main()
