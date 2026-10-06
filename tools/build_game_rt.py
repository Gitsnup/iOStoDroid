"""Build the host runtime harness: codegen + gcc compile. Prints paths."""

import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from radek.game import codegen

BUILD = os.path.join(ROOT, "build", "game_rt")
IPA = os.path.join(ROOT, "tests", "data", "AngryBirds_v1.0_os30.ipa")


def main() -> int:
    opt = os.environ.get("RT_OPT", "-O0")
    os.makedirs(BUILD, exist_ok=True)
    rep = codegen.generate(IPA, BUILD)
    print("codegen:", rep)
    for hdr in ("cpu.h", "rt_core.h", "rt_host.c", "rt_core.c"):
        shutil.copy(os.path.join(ROOT, "radek", "game", "rt", hdr),
                    os.path.join(BUILD, hdr))
    cmd = ["gcc", opt, "-o", os.path.join(BUILD, "rt_run"),
           os.path.join(BUILD, "rt_host.c"),
           os.path.join(BUILD, "rt_core.c"),
           os.path.join(BUILD, "game_all.c"),
           os.path.join(BUILD, "rt_gen.c"), "-lm"]
    print("cc:", " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-4000:])
        print(r.stderr[-4000:])
        return 1
    print("built:", os.path.join(BUILD, "rt_run"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
