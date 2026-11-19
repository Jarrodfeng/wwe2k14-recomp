"""Lane G (S103) gate check: builds out/tools/pacing_unit.exe, runs it clean (must exit 0) and with each planted fault (each must exit 1 with its message).
   py -I tools/scene/pacing_unit_selftest.py            exit 0 = all good
"""
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
EXE = os.path.join(ROOT, "out", "tools", "pacing_unit.exe")

PLANTS = [  # (flag, a substring the failure must print)
    ("--plant-queue", "the newest at its take"),
    ("--plant-nofree", "live draws"),
    ("--plant-stale", "differ from the oracle"),
    ("--plant-barrier", "barrier"),
    ("--plant-specmask", "SpecularCol"),   # S103 SL: pixel-free; the mask without the light fails the table check and the light-change stale check
]


def run(args):
    p = subprocess.run([EXE] + args, capture_output=True, text=True, timeout=300)
    return p.returncode, p.stdout + p.stderr


def main():
    b = subprocess.run(["pwsh", "-NoProfile", "-File", os.path.join(ROOT, "tools", "scene", "build_pacing_unit.ps1")], capture_output=True, text=True, timeout=600)
    if b.returncode != 0:
        print(b.stdout + b.stderr)
        print("pacing_unit_selftest: BUILD FAILED")
        return 2
    rc, out = run([])
    print(out)
    if rc != 0:
        print("pacing_unit_selftest: the clean run FAILED")
        return 1
    bad = 0
    for flag, needle in PLANTS:
        rc, out = run([flag])
        ok = rc == 1 and needle in out
        print("plant %-16s %s (exit %d)" % (flag, "caught" if ok else "NOT CAUGHT", rc))
        bad += 0 if ok else 1
    print("pacing_unit_selftest: %s" % ("PASS" if not bad else "FAIL"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
