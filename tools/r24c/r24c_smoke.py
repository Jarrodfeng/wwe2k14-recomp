#!/usr/bin/env python3
"""S102 lane R24-C: the smoke (minutes): one capture, depth_nostencil off vs on under the player bundle, N frames. py -I tools\\r24c\\r24c_smoke.py [--capture bryan] [--frames 60]"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import r24c_common as C

ap = argparse.ArgumentParser()
ap.add_argument('--root', default=C.ROOT)
ap.add_argument('--capture', default='bryan')
ap.add_argument('--frames', type=int, default=60)
a = ap.parse_args()
main_co = C.main_checkout(a.root)
work = os.path.join(a.root, 'out', 'r24c')
os.makedirs(work, exist_ok=True)
exe = os.path.join(a.root, 'out', 'renderer', 'dev', 'fview.exe')
game = os.path.join(main_co, 'game_data', 'extracted')
z = os.path.join(main_co, C.CAPTURES[a.capture])
off = C.run_fview(exe, z, game, work, a.capture + '.smoke.off', C.BUNDLE + ['--depth-census'], frames=a.frames)
on = C.run_fview(exe, z, game, work, a.capture + '.smoke.on', C.BUNDLE + ['--depth-census', '--depth-nostencil', '1'], frames=a.frames)
for name, r in (('off', off), ('on', on)):
    print('%s: rc %d validation %d frames hashed %d' % (name, r.rc, r.valid, len(r.hashes) - 1))
    for ln in r.lines('[rdr] depth format') + r.lines('[r24c]') + r.lines('[rdr][depth_nostencil]'):
        print('   ' + ln[:230])
print('differing frames:', on.differing(off))
