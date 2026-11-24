#!/usr/bin/env python3
"""S103 lane A8 check: the wrestlers' light object computed at the latch (docs\\renderer\\INPUTS_A8.md). 0 game runs, no game, no SDK.
  1. builds tools\\scene\\a8_proof.cpp (with record_io.cpp and the pure module src\\renderer_scene\\wrestler_light_latch.h);
  2. the module's unit (--unit) must pass, and each of its six planted twins must FAIL its NAMED check (stale / early -> HOOK, wrong_slot -> CENSUS, always_equal -> CENSUS differ, no_changed_gate -> CHANGED GATE, no_walk_guard -> WALK GUARD);
  3. the replay over the scene records of run 27, 28 and 29's cases (out\\s103\\runNN_case.zip, shared captures, read-only): `wrestler_light latch==walk, differ 0` on changed frames AND on all frames, and the synthesis equals the walk,
     with changed frames > 0; the twins stale / early / wrong_slot on the same records must FAIL (the other three live in the unit only: a record has no way to show a wrongly counting census).
  py -I tools\\scene\\a8_check.py [--root DIR]
Exit 0 = pass, 1 = fail, 2 = the captures are not on this machine (SKIP: the unit part still ran)."""
import argparse
import os
import re
import subprocess
import sys
import zipfile

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
UNIT_PLANTS = {'stale': 'A8 HOOK', 'early': 'A8 HOOK', 'wrong_slot': 'A8 CENSUS', 'always_equal': 'A8 CENSUS', 'no_changed_gate': 'A8 CHANGED GATE', 'no_walk_guard': 'A8 WALK GUARD', 'ignore_ours': 'A8 OURS GATE'}
RECORD_PLANTS = ('stale', 'early', 'wrong_slot')


def build(root, srcs, exe):
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-fuse-ld=lld']
    b = subprocess.run([CLANG] + flags + [os.path.join(root, s) for s in srcs] + ['-o', exe], capture_output=True, text=True)
    if b.returncode:
        print('FAIL: did not compile %s\n%s%s' % (srcs[0], b.stdout, b.stderr))
        return False
    return True


def captures(root, runs):
    cands = [os.path.join(root, 'out', 's103')]
    main = os.environ.get('RDR_MAIN_CHECKOUT')
    if main:
        cands.append(os.path.join(main, 'out', 's103'))
    cands.append(os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(root)))), 'out', 's103'))   # <main>\.claude\worktrees\<name> -> <main>
    cands.append(r'C:\Users\Jarrod\wwe2k14-recomp\out\s103')
    for c in cands:
        if all(os.path.isfile(os.path.join(c, 'run%d_case.zip' % n)) for n in runs):
            return c
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's103', 'a8')
    os.makedirs(out, exist_ok=True)
    bad = 0
    exe = os.path.join(out, 'a8_proof.exe')
    if not build(root, ['tools/scene/a8_proof.cpp', 'src/renderer_scene/record_io.cpp'], exe):
        return 1
    r = subprocess.run([exe, '--unit'], capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode:
        print('FAIL: the a8 unit failed')
        return 1
    for plant, name in UNIT_PLANTS.items():
        p = subprocess.run([exe, '--unit', '--plant', plant], capture_output=True, text=True)
        ok = p.returncode != 0 and any(ln.startswith('FAIL') and name in ln for ln in p.stdout.splitlines())
        print('unit twin %-16s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', name))
        bad += 0 if ok else 1
    runs = (27, 28, 29)
    cap = captures(root, runs)
    if not cap:
        print('SKIP: out\\s103\\run27 / 28 / 29_case.zip not found (shared captures): the record replay did not run')
        return 2 if not bad else 1
    for n in runs:
        d = os.path.join(out, 'case%d' % n)
        if not os.path.isdir(d) or len(os.listdir(d)) < 100:
            os.makedirs(d, exist_ok=True)
            with zipfile.ZipFile(os.path.join(cap, 'run%d_case.zip' % n)) as z:
                for nm in z.namelist():
                    if nm.endswith('.rsc'):
                        with open(os.path.join(d, os.path.basename(nm)), 'wb') as f:
                            f.write(z.read(nm))
        p = subprocess.run([exe, d], capture_output=True, text=True)
        print('--- run %d case: %d scene records' % (n, len(os.listdir(d))))
        print(p.stdout.strip())
        m = re.search(r'wrestler_light latch==walk (\d+), differ (\d+) \(changed frames only: latch==walk (\d+), differ (\d+), of (\d+) changed\)', p.stdout)
        if p.returncode or not m or int(m.group(2)) or int(m.group(4)) or int(m.group(3)) == 0:
            print('FAIL: run %d: the latch block differs from the walk on a changed frame (or no frame changed)' % n)
            bad += 1
        for plant in RECORD_PLANTS:
            pl = subprocess.run([exe, d, '--plant', plant], capture_output=True, text=True)
            ok = pl.returncode != 0
            print('record twin %-12s run %d: %s' % (plant, n, 'caught' if ok else 'NOT CAUGHT'))
            bad += 0 if ok else 1
    print('A8 CHECK %s' % ('FAIL' if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
