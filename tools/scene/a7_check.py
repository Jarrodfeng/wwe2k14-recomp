#!/usr/bin/env python3
"""S103 lane A7 gate check: bit 4 from the record by FIXED set ids (docs\\renderer\\INPUTS_A7.md, S103_FABLE_REVIEW.md 1.5). 0 game runs, no game, no SDK.
  1. builds tools\\scene\\light_fixed_unit.cpp, runs it and its five planted twins (each must exit 1 and fail its NAMED check: ring30for31, remap_ignored, override_ignored, absent_filled, census_equal_always);
  2. builds tools\\scene\\a7_proof.cpp and runs it on the scene records of run 27 and run 28's cases (out\\s103\\run27_case.zip / run28_case.zip, shared captures, read-only): every ring canvas / pieces / ropes / arena-slot
     LightRef's walked state must equal, word for word, the block the fixed-id rule picks (this latch's, or the previous latch's: the walk copies an object before the update task rewrites it);
  3. the planted twin ring30for31 on the same records must FAIL (the ring pieces differ).
The remap and override twins cannot fail on these records (the match records carry no pool, no remap and no override word, counted in the first output line), so they are proved on the synthetic rows of step 1.
  py -I tools\\scene\\a7_check.py [--root DIR]
Exit 0 = pass, 1 = fail, 2 = the captures are not on this machine (SKIP: the unit part still ran)."""
import argparse
import os
import re
import subprocess
import sys
import zipfile

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {'ring30for31': 'FIXED RING PIECES', 'remap_ignored': 'FIXED REMAP', 'override_ignored': 'FIXED OVERRIDE', 'absent_filled': 'FIXED ABSENT', 'census_equal_always': 'FIXED CENSUS', 'direct_replaced': 'FIXED KEEP DIRECT'}


def build(root, srcs, exe):
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-fuse-ld=lld']
    b = subprocess.run([CLANG] + flags + [os.path.join(root, s) for s in srcs] + ['-o', exe], capture_output=True, text=True)
    if b.returncode:
        print('FAIL: did not compile %s\n%s%s' % (srcs[0], b.stdout, b.stderr))
        return False
    return True


def captures(root):
    cands = [os.path.join(root, 'out', 's103')]
    main = os.environ.get('RDR_MAIN_CHECKOUT')
    if main:
        cands.append(os.path.join(main, 'out', 's103'))
    cands.append(os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(root)))), 'out', 's103'))   # <main>\.claude\worktrees\<name> -> <main>
    cands.append(r'C:\Users\Jarrod\wwe2k14-recomp\out\s103')
    for c in cands:
        if all(os.path.isfile(os.path.join(c, 'run%d_case.zip' % n)) for n in (27, 28)):
            return c
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's103', 'a7')
    os.makedirs(out, exist_ok=True)
    bad = 0
    unit = os.path.join(out, 'light_fixed_unit.exe')
    if not build(root, ['tools/scene/light_fixed_unit.cpp'], unit):
        return 1
    r = subprocess.run([unit], capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode:
        print('FAIL: the unit failed')
        return 1
    for plant, name in PLANTS.items():
        p = subprocess.run([unit, '--plant', plant], capture_output=True, text=True)
        ok = p.returncode != 0 and any(ln.startswith('FAIL') and name in ln for ln in p.stdout.splitlines())
        print('twin %-20s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', name))
        bad += 0 if ok else 1
    proof = os.path.join(out, 'a7_proof.exe')
    if not build(root, ['tools/scene/a7_proof.cpp', 'src/renderer_scene/record_io.cpp'], proof):
        return 1
    cap = captures(root)
    if not cap:
        print('SKIP: out\\s103\\run27_case.zip / run28_case.zip not found (shared captures): the record proof did not run')
        return 2 if not bad else 1
    for n in (27, 28):
        d = os.path.join(out, 'case%d' % n)
        if not os.path.isdir(d) or len(os.listdir(d)) < 100:
            os.makedirs(d, exist_ok=True)
            with zipfile.ZipFile(os.path.join(cap, 'run%d_case.zip' % n)) as z:
                for nm in z.namelist():
                    if nm.endswith('.rsc'):
                        with open(os.path.join(d, os.path.basename(nm)), 'wb') as f:
                            f.write(z.read(nm))
        p = subprocess.run([proof, d], capture_output=True, text=True)
        print('--- run %d case: %d scene records' % (n, len(os.listdir(d))))
        print(p.stdout.strip())
        if p.returncode:
            print('FAIL: run %d: a fixed-id class differs from its walked state' % n)
            bad += 1
        m = re.search(r'^arena slots \(l\(N\)\)\s+equal (\d+)', p.stdout, re.M)
        if not m or int(m.group(1)) == 0:
            print('FAIL: run %d: no arena-slot LightRef was joined (the slot -> model join is empty)' % n)
            bad += 1
        pl = subprocess.run([proof, d, '--plant', 'ring30for31'], capture_output=True, text=True)
        ok = pl.returncode != 0 and re.search(r'^ring pieces \(31\)\s+equal \d+ equal_to_previous_latch \d+ differ [1-9]', pl.stdout, re.M)
        print('twin ring30for31 on the records: %s' % ('caught' if ok else 'NOT CAUGHT'))
        bad += 0 if ok else 1
    print('A7 CHECK %s' % ('FAIL' if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
