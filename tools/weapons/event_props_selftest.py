"""Lane BLF selftest: runs out/weapons/event_props_test.exe (build it first: tools/weapons/build_event_props.ps1).
The clean run must pass every check; each planted twin must fail exactly its named check.
  py -I tools/weapons/event_props_selftest.py [--root .]
"""
import os
import subprocess
import sys

PLANTS = {"event_no_free": "event.reused_address", "event_instance0": "event.two_models_two_slots", "shadow_as_colour": "shadow.event_colour_kept"}


def run(exe, *args):
    p = subprocess.run([exe, *args], capture_output=True, text=True, timeout=120)
    return p.returncode, p.stdout


def main():
    root = "."
    if "--root" in sys.argv:
        root = sys.argv[sys.argv.index("--root") + 1]
    exe = os.path.join(root, "out", "weapons", "event_props_test.exe")
    if not os.path.exists(exe):
        print("FAIL: %s is not built (tools/weapons/build_event_props.ps1)" % exe)
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
