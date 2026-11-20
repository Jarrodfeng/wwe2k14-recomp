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
if 'Traceback (most recent call last)' in (r.stderr or '') + (r.stdout or ''):   # S105 E review 6: a crash is not a caught fault
    print((r.stderr or '').strip()[-300:])
    print(f'expect_fail: FAIL: the planted run crashed (exit {r.returncode}, a traceback): that is not the check catching the fault')
    sys.exit(1)
print(f'PLANTED-OK: the planted fault exited {r.returncode} as it must (S105 E: the last line of a twin that passed, never an inner FAIL line)')
