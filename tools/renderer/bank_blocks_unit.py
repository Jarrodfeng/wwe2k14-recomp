#!/usr/bin/env python3
"""S102 T1b: GPU-free unit test of the constant-bank block map. Builds src\\renderer_adapter\\bank_blocks_test.cpp and runs it over every frame of Bryan's entrance and the Brock
match case zips: every block of every bank (Block), seeded random byte ranges (CopyBytes, Ref), SameMap, zero reads past the table, and "no bank assembled by those reads",
all against the eager expansion. Then the three planted twins (6 = SameMap ignores the last entry), each of which must be caught (exit 1): 4 = a block map skips its patches, 5 = a block map's base off by one.
  py -I tools\\renderer\\bank_blocks_unit.py [--root DIR]
Exit 0 = the clean run passes on both captures and both plants are caught."""
import argparse, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
CAPS = [
    'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip',
    'out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip',
]


def main_checkout(root):
    try:
        c = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if c:
            return os.path.dirname(c)
    except Exception:
        pass
    return root


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    out = os.path.join(root, 'out', 't1b')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'bank_blocks_test.exe')
    ra = os.path.join(root, 'src', 'renderer_adapter')
    b = subprocess.run([CLANG, '-std=c++20', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-Wall', '-Wextra', '-Wno-unused-parameter', '-I', ra, '-I', os.path.join(root, 'src'),
                        os.path.join(ra, 'bank_blocks_test.cpp'), '-o', exe], capture_output=True, text=True)
    if b.returncode:
        print('FAIL  did not compile\n' + b.stdout + b.stderr)
        return 1
    fails = 0
    for cap in CAPS:
        z = os.path.join(main_co, cap)
        if not os.path.isfile(z):
            print('FAIL  input missing: ' + z)
            return 1
        p = subprocess.run([exe, z], capture_output=True, text=True, timeout=1200)
        print(('PASS  ' if p.returncode == 0 else 'FAIL  ') + os.path.basename(os.path.dirname(z)) + ': ' + (p.stdout.strip().split('\n')[-1] if p.stdout.strip() else 'no output (exit %d)' % p.returncode))
        fails += p.returncode != 0
    z = os.path.join(main_co, CAPS[0])
    for plant, what in ((4, 'a block map skips its patches'), (5, "a block map's base off by one"), (6, 'SameMap compares 63 of 64 entries')):
        p = subprocess.run([exe, z, 'plant', str(plant)], capture_output=True, text=True, timeout=1200)
        caught = p.returncode == 1
        print(('PASS  ' if caught else 'FAIL  ') + 'planted twin %d (%s): %s' % (plant, what, 'caught' if caught else 'NOT caught (exit %d)' % p.returncode))
        fails += not caught
    print('RESULT ' + ('PASS' if not fails else 'FAIL (%d)' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
