#!/usr/bin/env python3
"""S102 lane R24-C: the stencil census (docs/renderer/DEPTH_NOSTENCIL.md). Offline, 0 game runs, 0 Surface runs.

  py -I tools\\r24c\\r24c_census.py [--root DIR] [--captures bryan,brock,match] [--out DIR]

fview headless with --depth-census on each capture under the whole bundle the players run (and the LOW combination), then one table: per depth image, size, whether the
stream ever stencil-tests (with a real effect), writes or clears-to-nonzero it, and the pass classes that used it. Exit 0 always (it is a report); r24c_check.py is the gate."""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import r24c_common as C


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=C.ROOT)
    ap.add_argument('--captures', default='bryan,brock,match')
    ap.add_argument('--out', default=None)
    ap.add_argument('--low', action='store_true', help='also the LOW combination (shadow map 256, half, copy-alias 2)')
    a = ap.parse_args()
    main_co = C.main_checkout(a.root)
    work = a.out or os.path.join(a.root, 'out', 'r24c')
    os.makedirs(work, exist_ok=True)
    exe = os.path.join(a.root, 'out', 'renderer', 'dev', 'fview.exe')
    game = os.path.join(main_co, 'game_data', 'extracted')
    for name in a.captures.split(','):
        z = os.path.join(main_co, C.CAPTURES[name])
        arms = [('bundle', C.BUNDLE)] + ([('low', C.BUNDLE + C.LOW)] if a.low else [])
        for arm, flags in arms:
            r = C.run_fview(exe, z, game, work, '%s.%s.census' % (name, arm), flags + ['--depth-census'])
            print('== %s / %s: exit %d, validation errors %d, frames %d' % (name, arm, r.rc, r.valid, r.frames))
            for ln in r.lines('[r24c]'):
                print('  ' + ln)


if __name__ == '__main__':
    main()
