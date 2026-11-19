"""Lane LDR selftest: runs out/weapons/ladder_test.exe (build it first: tools/weapons/build_ladder.ps1).
The clean run must pass every check; each planted twin must fail exactly its named check.
  py -I tools/weapons/ladder_selftest.py [--root .]
"""
import os
import subprocess
import sys

PLANTS = {
    "ladder_no_path": "ldr.ladder_title",   # the title-keyed /STGL/%04d rig read as LADM (the path's PACH ignored)
    "steps_skip": "ldr.steps_variant",      # the old steps skip: the game's own variant never reaches the picture
    "belt_raw": "ldr.belt_subkey",          # the manager's belt keeps the raw id 14: skipped, no title sub-key
    "fixed_copies": "ldr.desk_once",        # the arena loader's fixed steps / desk drawn together with the rows: the double desk
    "light_flat": "ldr.light_additive",      # the ladder light pool / shaft left opaque with the props set (LDR2: the dark-blue column)
}


def run(exe, *args):
    p = subprocess.run([exe, *args], capture_output=True, text=True, timeout=120)
    return p.returncode, p.stdout


def main():
    root = "."
    if "--root" in sys.argv:
        root = sys.argv[sys.argv.index("--root") + 1]
    exe = os.path.join(root, "out", "weapons", "ladder_test.exe")
    if not os.path.exists(exe):
        print("FAIL: %s is not built (tools/weapons/build_ladder.ps1)" % exe)
        return 1
    bad = 0
    rc, out = run(exe)
    if rc != 0 or "FAIL  " in out:
        print(out)
        print("FAIL: the clean run did not pass")
        bad += 1
    else:
        print("PASS clean run: %d checks" % out.count("PASS "))
    for plant, check in PLANTS.items():
        rc, out = run(exe, "--plant", plant)
        failed = [ln.split()[1] for ln in out.splitlines() if ln.startswith("FAIL  ")]
        if rc == 1 and failed == [check]:
            print("PASS plant %s fails exactly %s" % (plant, check))
        else:
            print(out)
            print("FAIL: plant %s must fail exactly %s, got rc %d, failed %s" % (plant, check, rc, failed))
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
