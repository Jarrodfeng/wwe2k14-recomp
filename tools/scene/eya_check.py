#!/usr/bin/env python3
r"""Lane EYA (S103): the loader reads g_fFurLevel / g_fDifAmbLevel into Material (jboy.h), the mask table treats Param1 as per-material.
Source-level check (0 runs):  py -I tools\scene\eya_check.py [--plant]
--plant checks a copy of jboy.h with the two reads removed: the check must FAIL on it (exit 0 = the planted twin was caught)."""
import os, re, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))


def rd(rel):
    with open(os.path.join(ROOT, rel), encoding='utf-8', errors='replace') as f:
        return f.read()


def problems(jboy, draw_lists):
    bad = []
    for pname, field in (('g_fFurLevel', 'fur_level'), ('g_fDifAmbLevel', 'dif_amb_level')):
        if not re.search(r'pname == "%s"\)\s*mat\.%s\s*=\s*r\.f32\(po \+ 20\)' % (pname, field), jboy):
            bad.append('jboy.h does not read %s into Material::%s' % (pname, field))
    if not re.search(r'\{"Param1",\s*kStMaterial\}', draw_lists):
        bad.append('draw_lists.cpp: Param1 is not a per-material constant (kStMaterial)')
    return bad


def main():
    jboy, dl = rd('src/assets/jboy.h'), rd('src/renderer_scene/draw_lists.cpp')
    if '--plant' in sys.argv:
        jboy = re.sub(r'.*g_fDifAmbLevel.*\n', '', jboy)
        bad = problems(jboy, dl)
        print('%s (plant): the twin without the g_fDifAmbLevel read %s' % ('PASS' if bad else 'FAIL', 'was caught' if bad else 'was NOT caught'))
        return 0 if bad else 1
    bad = problems(jboy, dl)
    for b in bad:
        print('FAIL: ' + b)
    if not bad:
        print('PASS: jboy.h reads g_fFurLevel / g_fDifAmbLevel; Param1 is kStMaterial')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
