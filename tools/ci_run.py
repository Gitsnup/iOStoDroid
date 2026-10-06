#!/usr/bin/env python3
"""Stream real tool output, annotate failures, and propagate the exact exit code."""
from collections import deque
import subprocess
import sys

if len(sys.argv) < 2:
    sys.exit("usage: ci_run.py command [args...]")
lines = deque(maxlen=65)
with subprocess.Popen(
    sys.argv[1:], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1
) as process:
    for line in process.stdout:
        print(line, end="", flush=True)
        lines.append(line)
    code = process.wait()
if code:
    message = "".join(lines).replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    # GitHub truncates workflow-command annotations at roughly 4 KiB. Preserve
    # the end of the output, where Gradle and test runners print the failure,
    # rather than losing it behind warnings and successful task lines.
    if len(message) > 3500:
        message = "[earlier output truncated]%0A" + message[-3470:]
    print(f"::error title=Command failed ({code})::{message}")
sys.exit(code if code >= 0 else 128 - code)
