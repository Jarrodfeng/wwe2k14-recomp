"""Lane FAN selftest (S108): runs out/weapons/fan_tassel_unit.exe (build it first: tools/weapons/build_fan_tassel.ps1).
The clean run must pass every check; each planted twin must fail exactly its named checks. With --game / --dlc the real check on the
player's own files (Fandango's prop 2591, read-only) runs too, clean and planted.
  py -I tools/weapons/fan_tassel_selftest.py [--root .] [--game DIR --dlc DIR]
"""
import os
import subprocess
import sys

PLANTS = {
    "ring_cap96": ["tassel.all_rows", "tassel.over_bound_refused", "tassel.pool_full_whole", "tassel.one_copy_a_frame"],   # the old cut at 96: rows past 95 missing, a garbage count cut instead of refused, 1000-row calls cut to fit, the 659-row model holds 96
    "event_rows_append": ["tassel.one_copy_a_frame"],
}
REAL_PLANTS = {"ring_cap96": ["tassel.real_vertices"], "bind_u8": ["tassel.real_vertices"]}   # review: bind_u8 = the bind narrows a row number to 8 bits (the reference is the file's own map, not the bind's output)


def run(exe, *args):
    p = subprocess.run([exe, *args], capture_output=True, text=True, timeout=300)
    return p.returncode, p.stdout


def failed_of(out):
    return sorted(ln.split()[1] for ln in out.splitlines() if ln.startswith("FAIL  ") and len(ln.split()) > 1 and ln.split()[1].startswith("tassel."))


def arg(name):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else None


def main():
    root = arg("--root") or "."
    exe = os.path.join(root, "out", "weapons", "fan_tassel_unit.exe")
    if not os.path.exists(exe):
        print("FAIL: %s is not built (tools/weapons/build_fan_tassel.ps1)" % exe)
        return 1
    bad = 0
    suites = [("selftest", [], PLANTS)]
    game, dlc = arg("--game"), arg("--dlc")
    if game and dlc:
        suites.append(("real", ["--game", game, "--dlc", dlc], REAL_PLANTS))
    for mode, extra, plants in suites:
        rc, out = run(exe, mode, *extra)
        if rc != 0 or "FAIL  " in out:
            print(out)
            print("FAIL: the clean %s run did not pass" % mode)
            bad += 1
        else:
            print("PASS clean %s: %d checks" % (mode, out.count("PASS  tassel.")))
            for ln in out.splitlines():
                if ln.startswith("      "):
                    print(ln)
        for plant, checks in plants.items():
            rc, out = run(exe, mode, *extra, "--plant", plant)
            got = failed_of(out)
            if rc == 1 and got == sorted(checks):
                print("PASS %s plant %s fails exactly %s" % (mode, plant, ", ".join(checks)))
            else:
                print(out)
                print("FAIL: %s plant %s must fail exactly %s, got rc %d, failed %s" % (mode, plant, checks, rc, got))
                bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
