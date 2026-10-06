"""Differential test entry point (see tests/difftest/run.py).

Default runs a smoke subset; set DIFFTEST_FULL=1 for the whole clean set.
"""
import os
import subprocess
import sys


def test_lifted_c_matches_unicorn():
    env = dict(os.environ)
    if env.get("DIFFTEST_FULL") != "1":
        env.setdefault("DIFFTEST_SUBSET", "40")
        env.setdefault("DIFFTEST_SEEDS", "2")
    r = subprocess.run([sys.executable, "tests/difftest/run.py"],
                       capture_output=True, text=True, timeout=2400, env=env)
    print(r.stdout[-4000:])
    print(r.stderr[-2000:], file=sys.stderr)
    assert r.returncode == 0, "difftest mismatches found"
