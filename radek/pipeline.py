from __future__ import annotations
import datetime
import json
import shutil
import tempfile
from pathlib import Path
from .archive import extract_ipa, discover_app, read_plist, metadata, icon_candidates
from .analysis import analyze, dependency_graph, prove_leaf, capabilities
from .ir import Unsupported
from .resources import normalize_png, copy_resources
from .apk import ARTIFACT, Toolchain, build_apk, validate_apk

STATES = {
    "IMPORTED",
    "ANALYZING",
    "CONVERTING",
    "PACKAGING",
    "VALIDATING",
    "READY",
    "PARTIAL",
    "BLOCKED",
    "FAILED",
}
TRANSITIONS = {
    None: {"IMPORTED", "FAILED"},
    "IMPORTED": {"ANALYZING", "FAILED"},
    "ANALYZING": {"CONVERTING", "PARTIAL", "BLOCKED", "FAILED"},
    "CONVERTING": {"PACKAGING", "BLOCKED", "FAILED"},
    "PACKAGING": {"VALIDATING", "FAILED"},
    "VALIDATING": {"READY", "FAILED"},
}


class Pipeline:
    def __init__(self, output: Path):
        self.output = output.resolve()
        self.output.mkdir(parents=True, exist_ok=False, mode=0o700)
        self.report = {"schemaVersion": 1, "state": None, "events": [], "capabilities": capabilities()}

    def save(self):
        tmp = self.output / "report.json.tmp"
        tmp.write_text(json.dumps(self.report, indent=2, ensure_ascii=True))
        tmp.replace(self.output / "report.json")

    def log(self, stage: str, message: str):
        event = {
            "time": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "stage": stage,
            "message": message,
        }
        self.report["events"].append(event)
        with (self.output / "conversion.jsonl").open("a") as f:
            f.write(json.dumps(event) + "\n")
        self.save()

    def transition(self, state: str, message: str):
        if state not in TRANSITIONS.get(self.report["state"], set()):
            raise RuntimeError(f'invalid transition {self.report["state"]} -> {state}')
        self.report["state"] = state
        self.log(state, message)

    def run(self, ipa: Path, authorized: bool, analyze_only=False, key: Path | None = None):
        try:
            if not authorized:
                raise ValueError(
                    "authorization confirmation is required; protected binaries are never decrypted"
                )
            ipa = ipa.resolve(strict=True)
            if ipa.suffix.lower() != ".ipa":
                raise ValueError("input must have .ipa extension")
            self.report["authorizationConfirmed"] = True
            with tempfile.TemporaryDirectory(prefix="job-", dir=self.output) as temporary:
                work = Path(temporary)
                self.transition(
                    "IMPORTED", f"Accepted IPA ({ipa.stat().st_size} bytes); starting bounded extraction"
                )
                extract_ipa(ipa, work / "extracted")
                self.transition("ANALYZING", "Extraction complete; reading plist and executable")
                app = discover_app(work / "extracted")
                info = read_plist(app / "Info.plist")
                self.report["application"] = metadata(info, ipa)
                icon = None
                for candidate in icon_candidates(info, app):
                    try:
                        icon = normalize_png(candidate.read_bytes())
                        (self.output / "icon.png").write_bytes(icon)
                        self.report["icon"] = {
                            "status": "SUPPORTED",
                            "source": candidate.relative_to(app).as_posix(),
                            "path": "icon.png",
                        }
                        break
                    except ValueError as exc:
                        self.log("icon", f"{candidate.name}: {exc}")
                if icon is None:
                    self.report["icon"] = {
                        "status": "PARTIAL",
                        "reason": "No decodable loose PNG icon; Assets.car decoding is not implemented",
                    }
                executable = app / info["CFBundleExecutable"]
                mach = analyze(executable)
                graph = dependency_graph(app, executable, mach)
                self.report["machO"] = mach
                self.report["dependencies"] = graph
                self.log(
                    "ANALYZING",
                    f'Analyzed {len(mach["slices"])} architecture slice(s), {len(graph["nodes"])} Mach-O image(s), {len(graph["edges"])} dependency edge(s)',
                )
                try:
                    selected, program = prove_leaf(executable, mach, graph)
                except Unsupported as exc:
                    self.report["blockers"] = [str(exc)]
                    self.transition("BLOCKED", str(exc))
                    return self.report
                self.report["selectedArchitecture"] = selected["architecture"]
                self.report["conversion"] = program.report()
                self.report["contract"] = "closed-integer-entry-v1"
                if analyze_only:
                    self.transition(
                        "PARTIAL",
                        "Verified leaf conversion plan; packaging was not requested. No APK has been produced.",
                    )
                    return self.report
                tools = Toolchain.discover()
                self.transition(
                    "CONVERTING",
                    f"Reconstructed {program.source_size} input bytes into {len(program.machine_code)} Android ARM64 instruction bytes",
                )
                assets = work / "assets"
                self.report["resources"] = copy_resources(app, assets / "bundle", info["CFBundleExecutable"])
                self.log(
                    "CONVERTING",
                    f'Preserved {len(self.report["resources"])} resource files with relative bundle paths',
                )
                self.transition(
                    "PACKAGING",
                    "Compiling JNI ELF, Android entry point, resources and DEX; signing with development key",
                )
                pending = work / ARTIFACT
                identity = build_apk(
                    work / "package",
                    pending,
                    program.machine_code,
                    self.report["application"],
                    icon,
                    assets,
                    self.report,
                    tools,
                    key or Path(__file__).resolve().parent.parent / ".local/signing/debug.keystore",
                    self.log,
                )
                self.report["output"] = {**identity, "apk": ARTIFACT}
                self.transition("VALIDATING", "Independently validating signed APK and native dependencies")
                validation = validate_apk(
                    pending, tools, identity["package"], identity["entryPoint"], log=self.log
                )
                self.report["validation"] = validation
                shutil.move(str(pending), self.output / ARTIFACT)
                self.transition(
                    "READY",
                    "Signed standalone native APK passed static validation; device execution is not claimed",
                )
        except Exception as exc:
            self.report["error"] = {"type": type(exc).__name__, "message": str(exc)}
            if self.report["state"] not in ("READY", "PARTIAL", "BLOCKED", "FAILED"):
                self.transition("FAILED", str(exc))
            else:
                self.save()
        return self.report
