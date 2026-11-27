#!/usr/bin/env python3
"""S111 lane CAARING check: builds tools\\scene\\car_rope_unit.cpp (src\\renderer_scene\\car_rope_colours.h: a created arena's twelve rope colours, read from a fake ring table,
carried as kFxCarRope rows, handed to our rope instances), runs it, then its planted twins (each must exit 1 and fail its NAMED check), then the wiring text checks
(the latch writes the rows, the record reader accepts the kind, the ring applier and the front end use the colour).
  py -I tools\\scene\\car_rope_check.py [--root DIR]
Exit 0 = pass, 1 = fail. 0 game runs, no game files, no SDK."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {'white': 'apply.recomputed_rope_coloured', 'zero_is_one': 'read.zero_and_negative_are_zero', 'grid_only': 'apply.rids_rows_coloured'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's111', 'caaring')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'car_rope_unit.exe')
    src = os.path.join(root, 'tools', 'scene', 'car_rope_unit.cpp')
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-fuse-ld=lld']
    b = subprocess.run([CLANG] + flags + [src, '-o', exe], capture_output=True, text=True, cwd=out)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    r = subprocess.run([exe], capture_output=True, text=True, cwd=out)
    print(r.stdout.strip())
    if r.returncode:
        print('FAIL: the unit test failed\n' + r.stderr)
        return 1
    bad = 0
    for plant, name in PLANTS.items():
        p = subprocess.run([exe, '--plant', plant], capture_output=True, text=True, cwd=out)
        failed = [ln for ln in p.stdout.splitlines() if ln.startswith('FAIL')]
        ok = p.returncode == 1 and any(name in ln for ln in failed)
        print('twin %-12s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', name))
        bad += 0 if ok else 1

    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
    from read_unit import read_path  # a split file is read with its parts expanded (tools/split/read_unit.py)

    def text(rel):
        return read_path(os.path.join(root, rel))
    need = [
        ('src/renderer_scene/rows_stage_fx.h', 'kFxCarRope = 9', 'the row kind exists'),
        ('src/renderer_scene/record_io.cpp', 'KeepKnownKinds(tmp, kFxScreen, kFxLast, out.c_errors)', 'the record reader keeps every stage-fx kind up to kFxLast (rows_stage_fx.h) and skips a newer one (S111 FXSKIP)'),
        ('src/scene_live/stage_fx_contrib.h', 'car_rope::ReadRopeColours(', 'the latch reads the twelve records of a created arena'),
        ('src/scene_live/stage_fx_contrib.h', 'car_rope::ToRows(f->rope', 'the writer emits the kFxCarRope rows'),
        ('src/renderer_scene/rf_ring.cpp', 'car_rope::FromRows(t.stage_fx', 'the ring applier reads the rows'),
        ('src/renderer_scene/rf_ring.cpp', 'car_rope::RopeColourOf(*e, alt, rope_colours, o.tint)', 'each rope instance takes its colour'),
        ('src/renderer_scene/scene_record.h', 'kObjAmbientColour = 16777216', 'the object flag exists'),
        ('src/renderer_scene/front_end.cpp', 'if (c.amb_rgb) for (int i = 0; i < 3; ++i) out[i] = c.amb_rgb[i];', 'Resolve feeds the colour as the rgb of MatAmbCol'),
        ('src/renderer_scene/front_end.cpp', 'if (ctx.amb_rgb) st.h[5] = HashBytes(ctx.amb_rgb', 'the colour is in the kept draw\'s material stamp'),
        ('src/renderer_scene/front_end.cpp', 'oi.amb_rgb = obj.tint', 'the object carries the colour to its draws'),
    ]
    for rel, needle, why in need:
        ok = needle in text(rel)
        print('%s  wire %-40s %s' % ('PASS' if ok else 'FAIL', rel, why))
        bad += 0 if ok else 1
    print('car_rope_check: %s' % ('PASS' if bad == 0 else 'FAIL (%d)' % bad))
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
