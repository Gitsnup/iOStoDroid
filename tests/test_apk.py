import hashlib
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path
from radek.apk import (
    artifact_filename,
    build_apk,
    elf_info,
    validate_apk,
    _validate_complete_game_metadata,
    _validate_packaged_payloads,
)
from radek.archive import InputError


class ELFTests(unittest.TestCase):
    def test_not_elf_rejected(self):
        with self.assertRaises(InputError):
            elf_info(b"bad")

    def test_wrong_machine_rejected(self):
        b = bytearray(64)
        b[:6] = b"\x7fELF\x02\x01"
        struct.pack_into("<HH", b, 16, 3, 62)
        with self.assertRaises(InputError):
            elf_info(b)

    def test_arm64_elf_is_parsed_and_native_symbol_is_hashed(self):
        name = b"Java_dev_radek_generated_MainActivity_runNative"
        strings = b"\0" + name + b"\0"
        text = struct.pack("<I", 0xD65F03C0)
        data = bytearray(0x400 + 4 * 64)
        text_off = 0x100
        strings_off = text_off + len(text)
        symbols_off = (strings_off + len(strings) + 7) & ~7
        dynamic_off = 0x200
        section_off = 0x400
        data[text_off : text_off + len(text)] = text
        data[strings_off : strings_off + len(strings)] = strings
        data[symbols_off : symbols_off + 24] = bytes(24)
        data[symbols_off + 24 : symbols_off + 48] = struct.pack(
            "<IBBHQQ", 1, 0x12, 0, 1, 0x1000 + text_off, len(text)
        )
        dynamic = struct.pack(
            "<qQqQqQ", 5, 0x1000 + strings_off, 10, len(strings), 0, 0
        )
        data[dynamic_off : dynamic_off + len(dynamic)] = dynamic
        ident = b"\x7fELF" + bytes((2, 1, 1, 0)) + bytes(8)
        data[:16] = ident
        struct.pack_into(
            "<HHIQQQIHHHHHH", data, 16, 3, 183, 1, 0, 64, section_off, 0,
            64, 56, 2, 64, 4, 0,
        )
        struct.pack_into(
            "<IIQQQQQQ", data, 64, 1, 5, 0, 0x1000, 0x1000, len(data), len(data), 0x1000
        )
        struct.pack_into(
            "<IIQQQQQQ", data, 120, 2, 4, dynamic_off, 0x1000 + dynamic_off,
            0x1000 + dynamic_off, len(dynamic), len(dynamic), 8,
        )
        headers = [
            (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
            (0, 1, 6, 0x1000 + text_off, text_off, len(text), 0, 0, 4, 0),
            (0, 3, 2, 0x1000 + strings_off, strings_off, len(strings), 0, 0, 1, 0),
            (0, 11, 2, 0x1000 + symbols_off, symbols_off, 48, 2, 1, 8, 24),
        ]
        for index, header in enumerate(headers):
            struct.pack_into("<IIQQQQIIQQ", data, section_off + index * 64, *header)

        info = elf_info(bytes(data))
        self.assertEqual(info["architecture"], "arm64-v8a")
        self.assertEqual(info["elfClass"], 64)
        entry = info["exports"][name.decode()]
        self.assertEqual(entry["size"], len(text))
        self.assertEqual(entry["sha256"], hashlib.sha256(text).hexdigest())

    def test_arm32_elf_is_parsed_and_native_symbol_is_hashed(self):
        name = b"Java_dev_radek_generated_MainActivity_runNative"
        strings = b"\0" + name + b"\0"
        text = struct.pack("<I", 0xE12FFF1E)
        data = bytearray(0x300 + 4 * 40)
        text_off = 0x100
        strings_off = text_off + len(text)
        symbols_off = (strings_off + len(strings) + 3) & ~3
        dynamic_off = 0x200
        section_off = 0x300
        data[text_off : text_off + len(text)] = text
        data[strings_off : strings_off + len(strings)] = strings
        data[symbols_off : symbols_off + 16] = bytes(16)
        data[symbols_off + 16 : symbols_off + 32] = struct.pack(
            "<IIIBBH", 1, 0x1000 + text_off, len(text), 0x12, 0, 1
        )
        dynamic = struct.pack(
            "<iIiIiI", 5, 0x1000 + strings_off, 10, len(strings), 0, 0
        )
        data[dynamic_off : dynamic_off + len(dynamic)] = dynamic
        ident = b"\x7fELF" + bytes((1, 1, 1, 0)) + bytes(8)
        data[:16] = ident
        struct.pack_into(
            "<HHIIIIIHHHHHH", data, 16, 3, 40, 1, 0, 52, section_off, 0,
            52, 32, 2, 40, 4, 0,
        )
        struct.pack_into(
            "<IIIIIIII", data, 52, 1, 0, 0x1000, 0x1000, len(data), len(data), 5, 0x1000
        )
        struct.pack_into(
            "<IIIIIIII", data, 84, 2, dynamic_off, 0x1000 + dynamic_off, 0x1000 + dynamic_off,
            len(dynamic), len(dynamic), 4, 4,
        )
        headers = [
            (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
            (0, 1, 6, 0x1000 + text_off, text_off, len(text), 0, 0, 4, 0),
            (0, 3, 2, 0x1000 + strings_off, strings_off, len(strings), 0, 0, 1, 0),
            (0, 11, 2, 0x1000 + symbols_off, symbols_off, 32, 2, 1, 4, 16),
        ]
        for index, header in enumerate(headers):
            struct.pack_into("<IIIIIIIIII", data, section_off + index * 40, *header)

        info = elf_info(bytes(data))
        self.assertEqual(info["architecture"], "armeabi-v7a")
        self.assertEqual(info["elfClass"], 32)
        entry = info["exports"][name.decode()]
        self.assertEqual(entry["size"], len(text))
        self.assertEqual(entry["sha256"], hashlib.sha256(text).hexdigest())

    def test_missing_apk_rejected_before_tools(self):
        with self.assertRaises(InputError):
            validate_apk(Path("/not-an-apk"), None, "p", "entry")

    def test_output_apk_name_uses_sanitized_ipa_basename(self):
        self.assertEqual(artifact_filename("/imports/My Game.ipa"), "My Game.apk")
        self.assertEqual(artifact_filename(r"C:\\imports\\bad:name.ipa"), "bad_name.apk")
        self.assertEqual(artifact_filename("...ipa"), "ConvertedIPA.apk")

    def test_incomplete_apk_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "bad.apk"
            with zipfile.ZipFile(p, "w") as z:
                z.writestr("AndroidManifest.xml", "not real")
            with self.assertRaises(InputError):
                validate_apk(p, None, "p", "entry")


class PackagedPayloadTests(unittest.TestCase):
    def test_source_ipa_bytes_are_rejected_even_when_renamed(self):
        source = b"PK\x03\x04synthetic source IPA bytes"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sample.apk"
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("assets/opaque.payload", source)
            with zipfile.ZipFile(path) as archive:
                with self.assertRaisesRegex(InputError, "original IPA content"):
                    _validate_packaged_payloads(
                        archive,
                        archive.namelist(),
                        hashlib.sha256(source).hexdigest(),
                    )

    def test_apple_executable_is_rejected_outside_assets(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sample.apk"
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("lib/arm64-v8a/foreign.so", b"\xcf\xfa\xed\xfe payload")
            with zipfile.ZipFile(path) as archive:
                with self.assertRaisesRegex(InputError, "Apple executable"):
                    _validate_packaged_payloads(archive, archive.namelist(), "0" * 64)


class NoPlaceholderPackagingTests(unittest.TestCase):
    @staticmethod
    def _complete_game_metadata():
        source_hash = "0" * 64
        return {
            "contract": "complete-game-v1",
            "package": "dev.radek.converted.p" + source_hash[:20],
            "source": {"sha256": source_hash},
            "targetAbi": "arm64-v8a",
            "conversion": {"outputBytes": 4, "targetAbi": "arm64-v8a", "backend": "test"},
            "gameConversion": {
                "status": "COMPLETE",
                "completeGameConversion": True,
                "reachableSourceFunctions": 1,
                "translatedReachableFunctions": 1,
                "untranslatedReachableFunctions": 0,
                "reachableApiCount": 0,
                "generatedApiReplacements": 0,
                "nativeApiPassthroughs": 0,
                "untranslatedReachableApiCount": 0,
                "apiCoverageComplete": True,
                "apiReplacements": [],
                "resourcesComplete": True,
                "lifecycleImplemented": True,
            },
        }

    def test_restricted_leaf_wrapper_is_never_packaged_as_an_apk(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            work = root / "package-work"
            output = root / "game.apk"
            with self.assertRaisesRegex(InputError, "no complete iOS-to-Android game translator"):
                build_apk(
                    work,
                    output,
                    b"\x00\x00\x00\x00",
                    {"sha256": "0" * 64},
                    None,
                    root / "assets",
                    {"conversion": {"outputBytes": 4}},
                    None,
                    root / "debug.keystore",
                )
            self.assertFalse(work.exists())
            self.assertFalse(output.exists())

    def test_closed_integer_entry_is_not_a_complete_game_conversion(self):
        with self.assertRaisesRegex(InputError, "not a complete-game conversion"):
            _validate_complete_game_metadata(
                {
                    "contract": "closed-integer-entry-v1",
                    "package": "dev.radek.converted.p" + "0" * 20,
                },
                "dev.radek.converted.p" + "0" * 20,
                "arm64-v8a",
            )

    def test_candidate_api_mapping_cannot_satisfy_complete_game_contract(self):
        metadata = {
            "contract": "complete-game-v1",
            "package": "dev.radek.converted.p" + "0" * 20,
            "source": {"sha256": "0" * 64},
            "targetAbi": "arm64-v8a",
            "conversion": {"outputBytes": 4, "targetAbi": "arm64-v8a", "backend": "test"},
            "gameConversion": {
                "status": "COMPLETE",
                "completeGameConversion": True,
                "reachableSourceFunctions": 1,
                "translatedReachableFunctions": 1,
                "untranslatedReachableFunctions": 0,
                "reachableApiCount": 1,
                "generatedApiReplacements": 0,
                "nativeApiPassthroughs": 0,
                "untranslatedReachableApiCount": 0,
                "apiCoverageComplete": True,
                "apiReplacements": [],
                "resourcesComplete": True,
                "lifecycleImplemented": True,
            },
        }
        with self.assertRaisesRegex(InputError, "API replacement accounting"):
            _validate_complete_game_metadata(
                metadata,
                "dev.radek.converted.p" + "0" * 20,
                "arm64-v8a",
            )

    def test_metadata_rejects_boolean_native_output_size(self):
        metadata = self._complete_game_metadata()
        metadata["conversion"]["outputBytes"] = True
        with self.assertRaisesRegex(InputError, "generated native-code size"):
            _validate_complete_game_metadata(metadata, metadata["package"], "arm64-v8a")

    def test_metadata_rejects_non_string_source_hash(self):
        metadata = self._complete_game_metadata()
        metadata["source"]["sha256"] = ["0" * 64]
        with self.assertRaisesRegex(InputError, "source hash"):
            _validate_complete_game_metadata(metadata, metadata["package"], "arm64-v8a")

    def test_metadata_rejects_boolean_untranslated_api_count(self):
        metadata = self._complete_game_metadata()
        metadata["gameConversion"]["untranslatedReachableApiCount"] = False
        with self.assertRaisesRegex(InputError, "API replacement accounting"):
            _validate_complete_game_metadata(metadata, metadata["package"], "arm64-v8a")

    def test_metadata_rejects_non_string_generated_api_artifact(self):
        metadata = self._complete_game_metadata()
        game = metadata["gameConversion"]
        game.update(reachableApiCount=1, generatedApiReplacements=1)
        game["apiReplacements"] = [{
            "codeGenerated": True,
            "linkedIntoApk": True,
            "reachableFromEntry": True,
            "sourceSymbol": "_UIApplicationMain",
            "targetAndroidApi": "android.app.Activity",
            "implementationSha256": "0" * 64,
            "implementationArtifact": ["assets/generated/api.bin"],
        }]
        with self.assertRaisesRegex(InputError, "generated linked implementation evidence"):
            _validate_complete_game_metadata(metadata, metadata["package"], "arm64-v8a")

    def test_metadata_rejects_unsafe_generated_api_artifact_path(self):
        metadata = self._complete_game_metadata()
        game = metadata["gameConversion"]
        game.update(reachableApiCount=1, generatedApiReplacements=1)
        game["apiReplacements"] = [{
            "codeGenerated": True,
            "linkedIntoApk": True,
            "reachableFromEntry": True,
            "sourceSymbol": "_UIApplicationMain",
            "targetAndroidApi": "android.app.Activity",
            "implementationSha256": "0" * 64,
            "implementationArtifact": "../generated/api.bin",
        }]
        with self.assertRaisesRegex(InputError, "path is unsafe"):
            _validate_complete_game_metadata(metadata, metadata["package"], "arm64-v8a")
