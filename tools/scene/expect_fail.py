r"""Helper (S103 IXF): runs a command and PASSES only when it exits non-zero (a planted twin: the check must catch the plant).
  py -I tools\scene\expect_fail.py CMD [ARGS...]
"""
import os
import subprocess
import sys

cmd = [os.path.normpath(sys.argv[1])] + sys.argv[2:]
p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
if p.returncode == 0:
    print('FAIL: the planted twin was NOT caught (exit 0): ' + ' '.join(sys.argv[1:]))
    sys.exit(1)
print(f'plant caught (exit {p.returncode}): ' + ' '.join(sys.argv[1:]))
sys.exit(0)
