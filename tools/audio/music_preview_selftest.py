#!/usr/bin/env python3
"""Lane 281: builds and runs tools\\audio\\music_preview_unit.cpp (the Create-an-Entrance MUSIC preview's decision logic,
src\\music_preview_core.h; no game files) and its planted twins.
  py -I tools\\audio\\music_preview_selftest.py [--root DIR]
The clean run must exit 0; each planted fault (a missed stop on a switch, a missed stop on close, the game not silenced,
a call the game ignores still played, a stock song taken over) must exit 1."""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
PLANTS = ('no_stop_on_switch', 'no_stop_on_close', 'game_keeps_id', 'play_when_locked', 'play_stock')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    root = os.path.abspath(ap.parse_args().root)
    clang = r'C:\Program Files\LLVM\bin\clang++.exe'
    if not os.path.isfile(clang):
        print('FAIL  clang++ is missing')
        return 1
    out_dir = os.path.join(root, 'out', 'tools')
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, 'music_preview_unit.exe')
    p = subprocess.run([clang, '-std=c++20', '-O1', '-Wall', '-Wextra', os.path.join(root, 'tools', 'audio', 'music_preview_unit.cpp'), '-o', exe],
                       capture_output=True, text=True, timeout=600)
    if p.returncode:
        print(p.stdout + p.stderr)
        print('FAIL  build')
        return 1
    bad = 0
    r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    ok = r.returncode == 0 and 'music_preview_unit: PASS' in r.stdout
    print(('PASS' if ok else 'FAIL') + '  clean: ' + r.stdout.strip().splitlines()[-1])
    bad += not ok
    for plant in PLANTS:
        r = subprocess.run([exe, plant], capture_output=True, text=True, timeout=60)
        ok = r.returncode == 1
        print(('PASS' if ok else 'FAIL') + '  planted %s fails (exit %d)' % (plant, r.returncode))
        bad += not ok
    print('music_preview_selftest: %s' % ('PASS' if not bad else 'FAIL (%d)' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
