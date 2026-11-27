"""S111 CHAMBERLIGHT2 selftest: compiles out/weapons/chamber_inherit_test.exe from tools/weapons/chamber_inherit_test.cpp with clang (run here so the prover's parallel lines never race it), then runs it.
The clean run must pass every check; each planted twin must fail exactly its named checks.
  py -I tools/weapons/chamber_inherit_selftest.py [--root .]
"""
import os
import subprocess
import sys

CLANG = r"C:\Program Files\LLVM\bin\clang++.exe"
FLAGS = ["-std=c++20", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers", "-Wno-deprecated-declarations", "-Wno-unused-but-set-variable", "-g", "-O1", "-D_CRT_SECURE_NO_WARNINGS"]
PLANTS = {
    "props_set": ["ch.set", "ch.not_char"],               # the rule before CHAMBERLIGHT2: no inherited light (the arena props set)
    "first_char": ["ch.last", "ch.set", "ch.not_char"],   # the first listed character instead of the last
    "any_row": ["ch.not_char"],                           # every non-CWeapon list row taken for a character (the rule before the review)
}


def run(exe, *args):
    p = subprocess.run([exe, *args], capture_output=True, text=True, timeout=120)
    return p.returncode, p.stdout


def main():
    root = "."
    if "--root" in sys.argv:
        root = sys.argv[sys.argv.index("--root") + 1]
    out_dir = os.path.join(root, "out", "weapons")
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, "chamber_inherit_test.exe")
    src = os.path.join(root, "tools", "weapons", "chamber_inherit_test.cpp")
    build = subprocess.run([CLANG, *FLAGS, "-fuse-ld=lld", src, "-o", exe], capture_output=True, text=True, timeout=300)
    if build.returncode != 0 or not os.path.exists(exe):
        print(build.stdout + build.stderr)
        print("FAIL: chamber_inherit_test.cpp did not build")
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
