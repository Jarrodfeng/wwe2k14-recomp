"""S111 WEAPONLIGHT selftest: builds out/weapons/weapon_light_test.exe (tools/weapons/build_weapon_light.ps1, run here so the prover's parallel lines never race it), then runs it.
The clean run must pass every check; each planted twin must fail exactly its named checks.
  py -I tools/weapons/weapon_light_selftest.py [--root .]
"""
import os
import subprocess
import sys

PLANTS = {
    "props_set": ["wl.kinds", "wl.ids", "wl.missing"],   # the rule before WEAPONLIGHT: no pick, the arena props set
    "all_fifty": ["wl.kinds", "wl.ids", "wl.missing"],   # set 50 for every id (the listed ids' 100 lost)
}


def run(exe, *args):
    p = subprocess.run([exe, *args], capture_output=True, text=True, timeout=120)
    return p.returncode, p.stdout


def main():
    root = "."
    if "--root" in sys.argv:
        root = sys.argv[sys.argv.index("--root") + 1]
    build = subprocess.run(["pwsh", "-NoProfile", "-File", os.path.join(root, "tools", "weapons", "build_weapon_light.ps1")], capture_output=True, text=True, timeout=300)
    if build.returncode != 0:
        print(build.stdout + build.stderr)
        print("FAIL: tools/weapons/build_weapon_light.ps1 failed")
        return 1
    exe = os.path.join(root, "out", "weapons", "weapon_light_test.exe")
    if not os.path.exists(exe):
        print("FAIL: %s is not built (tools/weapons/build_weapon_light.ps1)" % exe)
        return 1
    bad = 0
    rc, out = run(exe)
    if rc != 0 or "FAIL  " in out:
        print(out)
        print("FAIL: the clean run did not pass")
        bad += 1
    else:
        print("PASS clean run: %d checks" % out.count("PASS "))
    for plant, checks in PLANTS.items():
        rc, out = run(exe, "--plant", plant)
        failed = [ln.split()[1] for ln in out.splitlines() if ln.startswith("FAIL  ")]
        if rc == 1 and failed == checks:
            print("PASS plant %s fails exactly %s" % (plant, checks))
        else:
            print(out)
            print("FAIL: plant %s must fail exactly %s, got rc %d, failed %s" % (plant, checks, rc, failed))
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
