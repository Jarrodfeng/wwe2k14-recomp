#!/usr/bin/env python3
"""S111 FLASHCAM review check: builds tools\\scene\\flash_camera_unit.cpp (the game's own keep rule of a crowd flash, census only: src\\scene_live\\flash_keep.h, sub_823B2470;
0 game runs, no game, no SDK), runs it, then its planted twins (each must exit 1 and fail its NAMED check), then the wiring needles (the census rides the rows of the record's own
period, the after-post draw prints it). The record's period itself (one record, one clock) is proven by tools\\scene\\flash_fx_check.py (period.rows_take_record_camera).
  py -I tools\\scene\\flash_camera_check.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {'screen_1280': 'keep.screen_words', 'no_char_box': 'keep.char_box'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's111', 'flashcam')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'flash_camera_unit.exe')
    srcs = [os.path.join(root, 'tools', 'scene', 'flash_camera_unit.cpp')]
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-fuse-ld=lld']
    b = subprocess.run([CLANG] + flags + srcs + ['-o', exe], capture_output=True, text=True, cwd=out)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    r = subprocess.run([exe], capture_output=True, text=True, cwd=out)
    print(r.stdout.strip())
    if r.returncode:
        print('FAIL: the unit test failed')
        return 1
    bad = 0
    for plant, name in PLANTS.items():
        p = subprocess.run([exe, '--plant', plant], capture_output=True, text=True, cwd=out)
        failed = [ln for ln in p.stdout.splitlines() if ln.startswith('FAIL')]
        ok = p.returncode != 0 and any(ln.startswith('FAIL ' + name) for ln in failed)
        print('twin %-14s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', name))
        bad += 0 if ok else 1

    def text(rel):
        with open(os.path.join(root, rel), encoding='utf-8') as f:
            return f.read()
    need = [
        ('src/scene_live/flash_fx_contrib.h', 'flashkeep::Stamp(kc, have, r)', 'every row of a latch is judged with the slot camera of the walk that drew it'),
        ('src/renderer_scene/live_flashes.cpp', 'if (r.flags & kFxfKeepCensus)', 'the applier carries the keep census to the sprite'),
        ('src/renderer_scene/front_end_flashes.h', '[scene][flashes] point:', 'the per-flash point line (tested item point, drawn centre, depth, keep rule, block projection)'),
        ('src/renderer_scene/front_end.cpp', '" kept_off_picture "', 'the scene census carries the keep totals (FlashLine)'),
    ]
    for rel, needle, what in need:
        if needle not in text(rel):
            print('FAIL: wiring: %s (%s)' % (what, rel))
            bad += 1
    if bad:
        print('FAIL: flash_camera_check, %d problem(s)' % bad)
        return 1
    print('PASS: flash_camera_check')
    return 0


if __name__ == '__main__':
    sys.exit(main())
