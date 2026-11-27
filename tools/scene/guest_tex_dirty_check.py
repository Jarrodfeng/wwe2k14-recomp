#!/usr/bin/env python3
"""S111 DIRTYOWN check: builds tools\\scene\\guest_tex_dirty_unit.cpp (the dirty table src\\scene_live\\guest_tex_dirty.h: an owner bit per reader, a table that grows instead of refusing; and the belt key
store's answer to a refused watch, src\\scene_live\\belt_plate.h Store::ReadKey), runs it, then its planted twins (each must exit 1 and FAIL its named check). 0 game runs, no SDK.
  py -I tools\\scene\\guest_tex_dirty_check.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {'owner_ignored': 'own.other_still_sees', 'no_growth': 'grow.need', 'refusal_silent': 'belt.refused_rereads', 'gen_kept': 'belt.header_change_rereads',
          'header_unseen': 'belt.same_handle_new_header'}   # S111 KEYFRESH


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')))
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's111', 'dirtyown')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'guest_tex_dirty_unit.exe')
    srcs = [os.path.join(root, 'tools', 'scene', 'guest_tex_dirty_unit.cpp'), os.path.join(root, 'src', 'scene_live', 'particle_reader.cpp')]
    b = subprocess.run([CLANG, '-O1', '-g', '-std=c++20', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-Wno-unused-command-line-argument'] + srcs + ['-o', exe], capture_output=True, text=True, cwd=out)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr[-4000:])
        return 1
    r = subprocess.run([exe], capture_output=True, text=True, cwd=out)
    print(r.stdout.strip())
    if r.returncode:
        print('FAIL: the unit failed\n' + r.stderr)
        return 1
    bad = 0
    for plant, name in PLANTS.items():
        p = subprocess.run([exe, '--plant', plant], capture_output=True, text=True, cwd=out)
        failed = [ln for ln in p.stdout.splitlines() if ln.startswith('FAIL')]
        ok = p.returncode == 1 and any(('  ' + name + '  ') in ln for ln in failed)
        print('twin %-15s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', name))
        bad += 0 if ok else 1
    print('RESULT', 'OK' if bad == 0 else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
