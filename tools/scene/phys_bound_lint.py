#!/usr/bin/env python3
"""S103 PN23F lint (review A8): every physical-memory reader in src\\scene_live checks the range itself (guest_tex::PhysRangeOk: the read ends inside the 512 MB of physical memory), never leaning on a fault handler.

  py -I tools\\scene\\phys_bound_lint.py --root .

Each `TranslatePhysical` in src\\scene_live\\*.h must have a `PhysRangeOk(` in the 12 lines before it. Planted twin (self-test): the same text with the check removed FAILS. 0 game runs.
(The adapter's own reader.h Phys() and input_script.h are other domains and are not covered here.)"""
import argparse
import glob
import os
import re
import sys


def problems(path, text):
    bad = []
    lines = text.splitlines()
    for i, ln in enumerate(lines):
        if 'TranslatePhysical' in ln and not ln.lstrip().startswith('//'):
            if not any('PhysRangeOk(' in x for x in lines[max(0, i - 12):i + 1]):
                bad.append('%s:%d TranslatePhysical without PhysRangeOk in the 12 lines before it' % (os.path.basename(path), i + 1))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    bad, readers = [], 0
    for p in sorted(glob.glob(os.path.join(root, 'src', 'scene_live', '*.h'))):
        t = open(p, encoding='utf-8', errors='replace').read()
        n = sum(1 for ln in t.splitlines() if 'TranslatePhysical' in ln and not ln.lstrip().startswith('//'))
        readers += n
        bad += problems(p, t)
        if n:   # planted twin: the same file without the check must FAIL
            twin = problems(p, t.replace('PhysRangeOk(', 'PhysRangeOkX('))
            if len(twin) != n:
                bad.append('%s: the planted twin (check removed) was not caught (%d of %d)' % (os.path.basename(p), len(twin), n))
    if readers == 0:
        bad.append('no TranslatePhysical reader found in src\\scene_live: the lint is blind')
    for b in bad:
        print('FAIL: ' + b)
    print('%s: phys_bound_lint: %d physical readers in src\\scene_live, each range-checked' % ('FAIL' if bad else 'PASS', readers))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
