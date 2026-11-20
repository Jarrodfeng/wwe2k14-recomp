#!/usr/bin/env python3
"""S105 A4 piece 3 check: builds tools\\scene\\refused_row_note_unit.cpp (src\\renderer_host\\refused_row_note.h, -DRDR_REFUSED_ROW_PLANTS), runs it clean, then each planted twin
(each must fail its NAMED check). 0 game runs, no game, no SDK.

  py -I tools\\scene\\refused_row_note_check.py [--root DIR]
Exit 0 = pass, 1 = fail."""
# entry: wwe2k14::scene_host::refused_row::Gate::Note (through refused_row_note_unit.cpp)
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {1: 'gate.rearm', 2: 'gate.once', 3: 'line.opaque_under', 4: 'serve.only_final', 5: 'serve.redundant_first'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's105', 'a4')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'refused_row_note_unit.exe')
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-DNOMINMAX', '-DRDR_REFUSED_ROW_PLANTS', '-fuse-ld=lld']
    b = subprocess.run([CLANG] + flags + [os.path.join(root, 'tools', 'scene', 'refused_row_note_unit.cpp'), '-o', exe], capture_output=True, text=True)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    r = subprocess.run([exe], capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode:
        print('FAIL: the unit test failed')
        return 1
    bad = 0
    for plant, name in PLANTS.items():
        p = subprocess.run([exe, '--plant', str(plant)], capture_output=True, text=True)
        failed = [ln for ln in p.stdout.splitlines() if ln.startswith('FAIL')]
        ok = p.returncode != 0 and any(ln.startswith('FAIL ' + name + ' ') for ln in failed)
        print('twin %d %-20s %s' % (plant, name, 'caught' if ok else 'NOT CAUGHT'))
        bad += 0 if ok else 1
    print('refused_row_note_check: %s' % ('FAIL' if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
