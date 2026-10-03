#!/usr/bin/env python3
"""Stream real tool output, annotate failures, and propagate the exact exit code."""
from collections import deque
import subprocess
import sys

if len(sys.argv) < 2:
    sys.exit('usage: ci_run.py command [args...]')
lines = deque(maxlen=65)
with subprocess.Popen(sys.argv[1:], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1) as process:
    for line in process.stdout:
        print(line, end='', flush=True)
        lines.append(line)
    code = process.wait()
if code:
    message = ''.join(lines).replace('%', '%25').replace('\r', '%0D').replace('\n', '%0A')
    print(f'::error title=Command failed ({code})::{message}')
sys.exit(code if code >= 0 else 128-code)
