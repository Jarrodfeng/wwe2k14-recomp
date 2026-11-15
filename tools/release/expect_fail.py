"""Run a command that MUST exit non-zero (a planted-fault twin): exit 0 if it did, 1 if it exited 0. Session 102 W1/W2.
    py -I tools\\release\\expect_fail.py CMD [ARGS...]
"""
import subprocess, sys

if len(sys.argv) < 2:
    print('expect_fail: no command given')
    sys.exit(2)
r = subprocess.run(sys.argv[1:], capture_output=True, text=True)
print((r.stdout or '').strip()[-300:])
if r.returncode == 0:
    print('expect_fail: FAIL: the planted fault exited 0 (not caught)')
    sys.exit(1)
print(f'expect_fail: PASS (the planted fault exited {r.returncode} as it must)')
