import json
import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from radek.pipeline import Pipeline
from .fixtures import ipa, macho, fat


class PipelineTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def run_fixture(self, exe=None, **kwargs):
        source = ipa(self.root / "input.ipa", exe)
        return Pipeline(self.root / "job").run(source, True, **kwargs)

    def test_synthetic_arm64_analysis_is_partial_not_ready(self):
        report = self.run_fixture(analyze_only=True)
        self.assertEqual(report["state"], "PARTIAL")
        self.assertEqual(report["conversion"]["backend"], "preserved-arm64")
        self.assertEqual(report["icon"]["status"], "SUPPORTED")
        self.assertFalse(list((self.root / "job").glob("job-*")))
        self.assertFalse((self.root / "job/RadekiOSConventor-debug.apk").exists())
        self.assertEqual(json.loads((self.root / "job/report.json").read_text())["state"], "PARTIAL")

    def test_framework_import_blocked(self):
        result = self.run_fixture(
            macho(
                imports=["_UIApplicationMain"],
                dependencies=["/System/Library/Frameworks/UIKit.framework/UIKit"],
            ),
            analyze_only=True,
        )
        self.assertEqual(result["state"], "BLOCKED")
        self.assertEqual(result["dependencies"]["edges"][0]["classification"], "unsupported")

    def test_encryption_is_never_bypassed(self):
        result = self.run_fixture(macho(encrypted=True), analyze_only=True)
        self.assertEqual(result["state"], "BLOCKED")
        self.assertIn("prohibited", result["blockers"][0])

    def test_arm64e_always_blocked(self):
        self.assertEqual(self.run_fixture(macho(subtype=2), analyze_only=True)["state"], "BLOCKED")

    def test_unsafe_instruction_blocked(self):
        result = self.run_fixture(macho(code=struct.pack("<II", 0xD4000001, 0xD65F03C0)), analyze_only=True)
        self.assertEqual(result["state"], "BLOCKED")
        self.assertIn("not in the proven subset", result["blockers"][0])

    def test_relocation_blocked(self):
        self.assertEqual(self.run_fixture(macho(reloc=True), analyze_only=True)["state"], "BLOCKED")

    def test_authorization_required(self):
        result = Pipeline(self.root / "job").run(ipa(self.root / "input.ipa"), False)
        self.assertEqual(result["state"], "FAILED")
        self.assertIn("authorization", result["error"]["message"])

    def test_missing_toolchain_is_failed_not_fake_ready(self):
        with patch("radek.pipeline.Toolchain.discover", side_effect=RuntimeError("no SDK")):
            result = self.run_fixture()
        self.assertEqual(result["state"], "FAILED")
        self.assertFalse((self.root / "job/RadekiOSConventor-debug.apk").exists())

    def test_embedded_framework_graph(self):
        main = macho(dependencies=["@executable_path/Frameworks/Embedded.framework/Embedded"])
        source = ipa(
            self.root / "input.ipa",
            main,
            extra={"Payload/Fixture.app/Frameworks/Embedded.framework/Embedded": macho()},
        )
        result = Pipeline(self.root / "job").run(source, True, True)
        self.assertEqual(result["state"], "BLOCKED")
        self.assertEqual(
            result["dependencies"]["edges"][0]["resolvedBundlePath"], "Frameworks/Embedded.framework/Embedded"
        )

    def test_unknown_loader_command_blocked(self):
        result = self.run_fixture(macho(extras=[struct.pack("<II", 0x777, 8)]), analyze_only=True)
        self.assertEqual(result["state"], "BLOCKED")

    def test_fat_selects_safe_arm64(self):
        result = self.run_fixture(fat([macho(cpu=12, subtype=9), macho()]), analyze_only=True)
        self.assertEqual(result["selectedArchitecture"], "arm64")

    def test_thumb_plan_is_offline_arm64(self):
        result = self.run_fixture(
            macho(struct.pack("<HH", 0x202A, 0x4770), cpu=12, subtype=9, thumb=True), analyze_only=True
        )
        self.assertEqual(result["conversion"]["backend"], "offline-arm32-to-arm64")
        self.assertEqual(result["conversion"]["outputBytes"], 8)

    def test_invalid_state_transition(self):
        p = Pipeline(self.root / "job")
        with self.assertRaises(RuntimeError):
            p.transition("READY", "not validated")

    def test_no_graphics_audio_swift_support_claims(self):
        p = Pipeline(self.root / "job")
        caps = {c["component"]: c["status"] for c in p.report["capabilities"]}
        for name in (
            "EAGL/OpenGL ES",
            "AudioToolbox/AVFoundation/OpenAL",
            "Swift",
            "Metal",
            "Foundation/CoreFoundation",
            "UIKit/CoreGraphics",
            "iOS lifecycle/input/sensors",
        ):
            self.assertEqual(caps[name], "BLOCKED")
