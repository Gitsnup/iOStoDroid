import argparse
import json
import sys
from pathlib import Path
from .pipeline import Pipeline
from .apk import Toolchain, validate_apk


def main():
    parser = argparse.ArgumentParser(
        description="Authorized IPA inspection and restricted offline native conversion (no emulation)"
    )
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("analyze", "convert"):
        p = sub.add_parser(name)
        p.add_argument("ipa", type=Path)
        p.add_argument("--output", type=Path, required=True, help="new, non-existing job directory")
        p.add_argument(
            "--authorized", action="store_true", help="confirm ownership/permission to convert this IPA"
        )
        p.add_argument("--debug-key", type=Path)
    p = sub.add_parser("validate")
    p.add_argument("apk", type=Path)
    p.add_argument("--package", required=True)
    p.add_argument("--entry", required=True)
    p.add_argument(
        "--converter-app",
        action="store_true",
        help="validate the importer APK rather than a converted program",
    )
    args = parser.parse_args()
    try:
        if args.command == "validate":
            result = validate_apk(
                args.apk, Toolchain.discover(), args.package, args.entry, not args.converter_app
            )
            print(json.dumps(result, indent=2))
            return 0
        result = Pipeline(args.output).run(
            args.ipa, args.authorized, args.command == "analyze", args.debug_key
        )
        print(
            json.dumps(
                {
                    "state": result["state"],
                    "report": str(args.output / "report.json"),
                    "blockers": result.get("blockers", []),
                    "error": result.get("error"),
                },
                indent=2,
            )
        )
        return {"READY": 0, "PARTIAL": 2, "BLOCKED": 3, "FAILED": 1}[result["state"]]
    except Exception as exc:
        print(f"{type(exc).__name__}: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
