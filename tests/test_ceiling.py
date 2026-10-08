"""The conversion-ceiling ledger must agree with the fail-closed pipeline.

Every test here asserts two things at once: the gate the ledger names as the
ceiling, and that the ledger cannot claim more than the pipeline actually did.
"""

import json
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path

from iostodroid.ceiling import CONTRACT, GATES, assess, finalize_apk
from iostodroid.pipeline import Pipeline
from .fixtures import ipa, macho, fat

DATA = Path(__file__).resolve().parent / "data"


class ConversionCeilingTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def run_fixture(self, exe=None, name="job", **kwargs):
        source = ipa(self.root / "input.ipa", exe)
        return Pipeline(self.root / name).run(source, True, **kwargs)

    @staticmethod
    def gate(report, gate_id):
        return next(item for item in report["conversionCeiling"]["gates"] if item["gate"] == gate_id)

    def assert_ceiling(self, report, gate_id, *, state):
        ceiling = report["conversionCeiling"]
        self.assertEqual(ceiling["contract"], CONTRACT)
        self.assertEqual(ceiling["ceilingGate"], gate_id)
        self.assertEqual(report["state"], state)
        self.assertFalse(ceiling["countsAsConversionProgress"])
        self.assertFalse(ceiling["completeGameConversion"])
        # Everything after the ceiling is explicitly not reached, never estimated.
        order = [item["gate"] for item in ceiling["gates"]]
        blocked = order.index(gate_id)
        for item in ceiling["gates"][blocked + 1 :]:
            self.assertEqual(item["status"], "NOT_REACHED")
        self.assertEqual(ceiling["gatesPassed"], blocked)
        return ceiling

    def test_real_game_code_halts_at_the_entry_routine_proof(self):
        # A call instruction is ordinary game code and is outside the proven subset.
        report = self.run_fixture(
            macho(
                code=struct.pack("<II", 0x94000000, 0xD65F03C0),
                imports=["_UIApplicationMain", "_glClearColor"],
                dependencies=[
                    "/System/Library/Frameworks/UIKit.framework/UIKit",
                    "/System/Library/Frameworks/OpenGLES.framework/OpenGLES",
                ],
            ),
            analyze_only=True,
        )
        ceiling = self.assert_ceiling(report, "ENTRY_ROUTINE", state="BLOCKED")
        self.assertIn("not in the proven subset", ceiling["ceilingReason"])
        # The ledger and the prover stopped at the same statement.
        self.assertTrue(ceiling["prover"]["agreesWithCeiling"])
        self.assertIn(ceiling["ceilingReason"], ceiling["prover"]["message"])
        # The reason reaches the blocker list so it is visible without JSON.
        self.assertTrue(any("Conversion ceiling: ENTRY_ROUTINE" in b for b in report["blockers"]))
        self.assertFalse(ceiling["boundedEntryArtifact"]["available"])

    def test_encrypted_input_halts_at_protection(self):
        report = self.run_fixture(macho(encrypted=True), analyze_only=True)
        ceiling = self.assert_ceiling(report, "PROTECTION", state="BLOCKED")
        self.assertIn("prohibited", ceiling["ceilingReason"])
        self.assertTrue(ceiling["prover"]["agreesWithCeiling"])

    def test_arm64e_input_halts_at_slice_format(self):
        report = self.run_fixture(macho(subtype=2), analyze_only=True)
        self.assert_ceiling(report, "SLICE_FORMAT", state="BLOCKED")

    def test_damaged_bind_table_halts_at_loader_metadata(self):
        bind = b"\x40_symbol\x00\x70" + b"\xff" * 9 + b"\x01\x80\x01\x00"
        command = struct.pack("<12I", 0x80000022, 48, 0, 0, 0x2000, len(bind), 0, 0, 0, 0, 0, 0)
        report = self.run_fixture(macho(extras=[command], blobs={0x2000: bind}), analyze_only=True)
        ceiling = self.assert_ceiling(report, "LOADER_METADATA", state="BLOCKED")
        self.assertIn("bind table is incomplete", ceiling["ceilingReason"])

    def test_missing_arm64_slice_halts_at_target_abi(self):
        source = ipa(self.root / "arm32.ipa", macho(cpu=12, subtype=6))
        report = Pipeline(self.root / "job").run(source, True, analyze_only=True, target_abi="arm64-v8a")
        ceiling = self.assert_ceiling(report, "TARGET_ABI", state="BLOCKED")
        self.assertIn("requires an ARM64 IPA slice", ceiling["ceilingReason"])

    def test_declared_but_unreached_imports_halt_at_api_linking(self):
        # The pipeline still produces the isolated leaf artifact (PARTIAL), while the
        # complete-game ceiling stops at API linking: both statements are true and the
        # ledger records which one it means.
        report = self.run_fixture(macho(imports=["_UIApplicationMain"]), analyze_only=True)
        ceiling = self.assert_ceiling(report, "API_LINKING", state="PARTIAL")
        self.assertIn("import symbol(s) are declared", ceiling["ceilingReason"])
        self.assertTrue(ceiling["boundedEntryArtifact"]["available"])

    def test_stubs_and_triage_never_pass_the_api_gate(self):
        # Reachable imports classify into verified/stubbed targets in the registry, but
        # neither category is a linked implementation, so the gate stays blocked.
        report = self.run_fixture(
            macho(
                code=struct.pack("<II", 0x52800540, 0xD65F03C0),
                imports=["_glDrawArrays", "_alcOpenDevice", "_objc_msgSend"],
            ),
            analyze_only=True,
        )
        ceiling = self.assert_ceiling(report, "API_LINKING", state="PARTIAL")
        evidence = self.gate(report, "API_LINKING")["evidence"]
        self.assertEqual(evidence["declaredImports"], 3)
        # Resolution is total (every import has a stub target) but nothing is linked.
        self.assertEqual(report["compatRegistry"]["handlerResolutionCoveragePercent"], 100)
        self.assertEqual(report["compatRegistry"]["verifiedImplementations"], 3)
        self.assertEqual(report["compatRegistry"]["stubbedHandlers"], 0)
        # 100% resolution coverage still left the complete-game ceiling in place.
        self.assertIn("import table to be resolvable or absent", ceiling["ceilingReason"])

    def test_reachable_imports_are_split_into_available_and_blocked(self):
        # Reachable calls cannot occur inside the proven subset, so this evidence is
        # exercised directly against the assessment instead of through the pipeline.
        source = ipa(
            self.root / "input.ipa",
            macho(imports=["_CFAbsoluteTimeGetCurrent", "_MTLCreateSystemDefaultDevice"]),
        )
        with zipfile.ZipFile(source) as archive:
            executable_data = archive.read("Payload/Fixture.app/Fixture")
        report = Pipeline(self.root / "job").run(source, True, analyze_only=True)
        reconstruction = json.loads((self.root / "job" / "reconstruction.json").read_text())
        for image in reconstruction["images"]:
            for slice_data in image["slices"]:
                slice_data.setdefault("apis", {})["entryReachability"] = {
                    "status": "direct-call-graph-only",
                    "imports": [
                        {
                            "name": "_CFAbsoluteTimeGetCurrent",
                            "framework": "CoreFoundation",
                            "area": "foundation",
                            "feasibility": "compatibility",
                        },
                        {
                            "name": "_MTLCreateSystemDefaultDevice",
                            "framework": "Metal",
                            "area": "graphics",
                            "feasibility": "blocked",
                        },
                    ],
                }
        ceiling = assess(
            report["machO"], report["dependencies"], reconstruction, executable_data=executable_data
        )
        gate = next(item for item in ceiling["gates"] if item["gate"] == "API_LINKING")
        self.assertEqual(gate["status"], "BLOCKED")
        self.assertEqual(gate["evidence"]["declaredImports"], 2)
        self.assertEqual(gate["evidence"]["reachableImports"], 2)
        self.assertEqual(gate["evidence"]["hostTestedImplementationsAvailable"], 1)
        self.assertEqual(gate["evidence"]["noAndroidTarget"], 1)
        self.assertIn("none is linked into a game", gate["detail"])

    def test_bounded_subset_passes_every_code_gate_and_distinguishes_the_artifact(self):
        job = self.root / "job"
        report = Pipeline(job).run(DATA / "hello-test.ipa", True, analyze_only=True)
        ceiling = report["conversionCeiling"]
        self.assertEqual(report["state"], "PARTIAL")
        self.assertIsNone(ceiling["ceilingGate"])
        self.assertEqual(ceiling["status"], "PENDING_BUILD")
        self.assertEqual(ceiling["gatesPassed"], len(GATES) - 1)
        self.assertTrue(ceiling["boundedEntryArtifact"]["available"])
        # The entry-leaf artifact is one function, not a game.
        self.assertIn("not a game", ceiling["boundedEntryArtifact"]["detail"])
        self.assertFalse(ceiling["countsAsConversionProgress"])
        # Packaging is only attempted by a real build; nothing is claimed here.
        self.assertEqual(self.gate(report, "APK_PACKAGING")["status"], "NOT_REACHED")
        markdown = (job / "reconstruction.md").read_text()
        self.assertIn("## Conversion ceiling", markdown)
        self.assertIn("counts as conversion progress: False", markdown)

    def test_convert_path_reports_the_missing_toolchain_at_the_apk_gate(self):
        job = self.root / "job"
        report = Pipeline(job).run(DATA / "hello-test.ipa", True, analyze_only=False)
        ceiling = report["conversionCeiling"]
        apk = self.gate(report, "APK_PACKAGING")
        if apk["status"] == "PASS":
            self.assertEqual(ceiling["status"], "FULLY_CONVERTIBLE")
            self.assertTrue(ceiling["completeGameConversion"])
            self.assertEqual(report["state"], "READY")
        else:
            self.assertEqual(apk["status"], "BLOCKED")
            self.assertEqual(ceiling["ceilingGate"], "APK_PACKAGING")
            self.assertEqual(ceiling["status"], "ASSESSED")
            self.assertFalse(ceiling["completeGameConversion"])
            self.assertEqual(report["state"], "BLOCKED")

    def test_finalize_apk_is_the_only_way_the_last_gate_passes(self):
        report = self.run_fixture(None, analyze_only=True)
        ceiling = report["conversionCeiling"]
        built = finalize_apk(json.loads(json.dumps(ceiling)), "BUILT", "packaged and validated")
        self.assertEqual(built["ceilingGate"], None)
        self.assertEqual(built["status"], "FULLY_CONVERTIBLE")
        self.assertTrue(built["completeGameConversion"])
        blocked = finalize_apk(json.loads(json.dumps(ceiling)), "MISSING_TOOLCHAIN", "no build-tools")
        self.assertEqual(blocked["ceilingGate"], "APK_PACKAGING")
        self.assertEqual(blocked["status"], "ASSESSED")
        self.assertFalse(blocked["completeGameConversion"])

    def test_fat_input_reports_every_candidate_attempt(self):
        report = self.run_fixture(fat([macho(cpu=12, subtype=6), macho()]), analyze_only=True)
        ceiling = report["conversionCeiling"]
        self.assertEqual(ceiling["status"], "PENDING_BUILD")
        evidence = self.gate(report, "SLICE_FORMAT")["evidence"]
        architectures = [item["architecture"] for item in evidence["candidateFailures"]]
        self.assertIn("armv6", architectures)
        self.assertIn("arm64", architectures)


if __name__ == "__main__":
    unittest.main()
