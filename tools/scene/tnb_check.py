#!/usr/bin/env python3
"""S111 PAGEPOOLS: builds tools\\scene\\tnb_unit.cpp (the entrance page's pools / spot / disks through the real latch, Emit, the record and stage_look::Pools: 0 game runs, no game, no SDK),
runs it, then its planted twins (each must exit 1 and fail its NAMED check, tnb_unit.cpp's header list).
  py -I tools\\scene\\tnb_check.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {
    'entrant_only': 'tnb.partner_disk_at_bound',
    'bind_ignored': 'tnb.phase4_disks_at_bound',
    'key_ignored': 'tnb.partner_is_disk_by_key',
    'size_ignored': 'tnb.disk_half_from_size',
    'pos_first': 'tnb.dark_track_bad_position_ok',
    'size_words': 'tnb.size_words_carried',
    'page_from_lights': 'pagepools.page_of_own_latch',   # S111 PAGEPOOLS: the record without its page rows (the page derived from the model-class light objects of latch r - 1)
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's111', 'tnb')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'tnb_unit.exe')
    srcs = [os.path.join(root, 'tools', 'scene', 'tnb_unit.cpp'), os.path.join(root, 'src', 'renderer_scene', 'record_io.cpp'), os.path.join(root, 'src', 'renderer_scene', 'live_scene.cpp'),
            os.path.join(root, 'src', 'scene_live', 'wrestler_contrib.cpp'), os.path.join(root, 'src', 'renderer_scene', 'live_map_e2_input.cpp'), os.path.join(root, 'src', 'renderer_scene', 'live_map_rope_input.cpp')]
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-fuse-ld=lld']
    b = subprocess.run([CLANG] + flags + srcs + ['-o', exe], capture_output=True, text=True)
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
        p = subprocess.run([exe, '--plant', plant], capture_output=True, text=True)
        failed = [ln for ln in p.stdout.splitlines() if ln.startswith('FAIL')]
        ok = p.returncode != 0 and any(name in ln for ln in failed)
        print('twin %-16s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', name))
        bad += 0 if ok else 1
    if bad:
        print('FAIL: %d twin(s) not caught' % bad)
        return 1
    print('PASS: unit, %d planted twins caught' % len(PLANTS))
    return 0


if __name__ == '__main__':
    sys.exit(main())
