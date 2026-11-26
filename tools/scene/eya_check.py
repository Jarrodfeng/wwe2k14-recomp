#!/usr/bin/env python3
r"""Lane EYA (S103): the loader reads g_fFurLevel / g_fDifAmbLevel into Material (the float table src\assets\material_floats.h, which jboy.h calls: S111 GUESTHAIR), the mask table treats Param1 as per-material.
Source-level check (0 runs):  py -I tools\scene\eya_check.py [--plant]
--plant checks a copy of the table with the g_fDifAmbLevel read removed: the check must FAIL on it (exit 0 = the planted twin was caught)."""
import os, re, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))


def rd(rel):
    with open(os.path.join(ROOT, rel), encoding='utf-8', errors='replace') as f:
        return f.read()


def problems(table, draw_lists):
    bad = []
    for pname, field in (('g_fFurLevel', 'fur_level'), ('g_fDifAmbLevel', 'dif_amb_level')):
        if not re.search(r'is\("%s"\)\)\s*mat\.%s\s*=\s*x;' % (pname, field), table):
            bad.append('material_floats.h does not read %s into Material::%s' % (pname, field))
    if not re.search(r'\{"Param1",\s*kStMaterial\}', draw_lists):
        bad.append('draw_lists.cpp: Param1 is not a per-material constant (kStMaterial)')
    return bad


def main():
    table, dl = rd('src/assets/material_floats.h'), rd('src/renderer_scene/draw_lists.cpp')
    if not re.search(r'matfloat::ApplyFloat\(mat, pname, kind, v, &m\.warnings\)', rd('src/assets/jboy.h')):
        print('FAIL: jboy.h does not read its float parameters through the table (material_floats.h)')
        return 1
    if '--plant' in sys.argv:
        table = re.sub(r'.*g_fDifAmbLevel.*\n', '', table)
        bad = problems(table, dl)
        print('%s (plant): the twin without the g_fDifAmbLevel read %s' % ('PASS' if bad else 'FAIL', 'was caught' if bad else 'was NOT caught'))
        return 0 if bad else 1
    bad = problems(table, dl)
    for b in bad:
        print('FAIL: ' + b)
    if not bad:
        print('PASS: the float table (jboy.h calls it) reads g_fFurLevel / g_fDifAmbLevel; Param1 is kStMaterial')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
