#!/usr/bin/env python3
"""S103 lane LH check: the planned light hold (a deliberate walked frame that refreshes an expiring light entry) is gone for the arena slot models (docs\\systems\\wrestler_light_hold.md). 0 game runs, no game, no SDK.
  1. builds tools\\scene\\lh_proof.cpp and runs it on the scene records of run 27 and run 28's cases (out\\s103\\run27_case.zip / run28_case.zip, shared captures, read-only): for every arena slot model the entry
     the latch COMPUTES from the record equals the walk's entry one frame later (index, mode), and with the new rule 0 entries stay walk-learned (so none ages and plans a hold: `would_hold 0`);
  2. the planted twins on the same records must FAIL: old_rule (the old hold: an equal learned entry is kept and ages -> would_hold > 0, name LH HOLD), keep_all_default (same, name LH HOLD),
     wrong_index (the refreshed entry is not the walk's next entry, name LH NEXT).
The unit twin of the live path (inputs_unit.cpp lh_old_hold) runs in inputs_check.py.
  py -I tools\\scene\\lh_check.py [--root DIR]
Exit 0 = pass, 1 = fail, 2 = the captures are not on this machine (SKIP)."""
import argparse
import os
import subprocess
import sys
import zipfile

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {'old_rule': 'LH HOLD', 'keep_all_default': 'LH HOLD', 'wrong_index': 'LH NEXT'}


def captures(root):
    cands = [os.path.join(root, 'out', 's103')]
    main = os.environ.get('RDR_MAIN_CHECKOUT')
    if main:
        cands.append(os.path.join(main, 'out', 's103'))
    cands.append(os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(root)))), 'out', 's103'))
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
    out = os.path.join(root, 'out', 's103', 'lh')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'lh_proof.exe')
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-fuse-ld=lld']
    b = subprocess.run([CLANG] + flags + [os.path.join(root, 'tools/scene/lh_proof.cpp'), os.path.join(root, 'src/renderer_scene/record_io.cpp'), '-o', exe], capture_output=True, text=True)
    if b.returncode:
        print('FAIL: did not compile lh_proof.cpp\n%s%s' % (b.stdout, b.stderr))
        return 1
    cap = captures(root)
    if not cap:
        print('SKIP: out\\s103\\run27_case.zip / run28_case.zip not found (shared captures): the record proof did not run')
        return 2
    bad = 0
    dirs = []
    for n in (27, 28):
        d = os.path.join(out, 'case%d' % n)
        if not os.path.isdir(d) or len(os.listdir(d)) < 100:
            os.makedirs(d, exist_ok=True)
            with zipfile.ZipFile(os.path.join(cap, 'run%d_case.zip' % n)) as z:
                for nm in z.namelist():
                    if nm.endswith('.rsc'):
                        with open(os.path.join(d, os.path.basename(nm)), 'wb') as f:
                            f.write(z.read(nm))
        dirs.append(d)
        p = subprocess.run([exe, d], capture_output=True, text=True)
        print('run%d: %s' % (n, p.stdout.strip().replace('\n', ' | ')))
        if p.returncode:
            print('FAIL: the proof failed on run %d' % n)
            bad += 1
    for plant, name in PLANTS.items():
        p = subprocess.run([exe, '--plant', plant, dirs[0]], capture_output=True, text=True)
        ok = p.returncode != 0 and any(ln.startswith('FAIL') and name in ln for ln in p.stdout.splitlines())
        print('twin %-18s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', name))
        bad += 0 if ok else 1
    print('PASS: lh' if not bad else 'FAIL: lh')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
