#!/usr/bin/env python3
"""S102 lane G12: the copies that still run in a Bryan frame (docs/renderer/COPIES_LEFT.md). Offline, 0 game runs.
  py -I tools\\g12\\g12_census.py [--root DIR] [--frame N]
fview lists every copy of one frame with the planner's verdict (copy_alias.h: --copy-alias-list), with the whole bundle the players run plus the LOW levers and --alias-fullsize 1.
Prints the copies grouped by (source class, kept-because, size)."""
import argparse
import collections
import os
import re
import subprocess
import sys

BUNDLE = ['--crowd-ours', '--crowd-cards', '--arena-ours', '--wrestlers-ours', '--caw-ours', '--ring-ours', '--ui-ours', '--ui-quads', '--ui-text-ours', '--flash-ours', '--props-ours', '--post-ours',
          '--movie-ours', '--screens-ours', '--shadow-casters-ours', '--rim-ours', '--circles-ours', '--plane-shadow-ours', '--small-passes-ours', '--rim-chain-ours', '--particles-ours', '--shadows-ours']
LOW = ['--shadow-map-size', '256', '--shadow-half', '--copy-alias', '2', '--copy-alias-classes', '11', '--alias-fullsize', '1']
CAPS = {'bryan': ('out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip', 'out/s101/lane92/case_20261112_122719_969_0_pc_run_exit/capture')}


def main_checkout(here):
    try:
        c = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if c:
            return os.path.dirname(c)
    except Exception:
        pass
    return here


L = re.compile(r"\[copy_alias\] f(\d+) op (\d+) copy (\d+) -> (\d+) class ([a-z ]+?) why '(.*?)' rect (\d+)x(\d+)->(\d+)x(\d+) refs m(\d+) l(\d+) / m(\d+) l(\d+)")


def listing(root, frame, extra=()):
    main = main_checkout(root)
    z = os.path.join(main, CAPS['bryan'][0])
    exe = os.path.join(root, 'out', 'renderer', 'dev', 'fview.exe')
    cmd = [exe, '--case', z, '--replay', '--frames', str(frame + 20), '--validation', '1', '--game', os.path.join(main, 'game_data', 'extracted')] + BUNDLE + LOW + list(extra) + ['--copy-alias-list', str(frame)]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=900, errors='replace')
    text = p.stdout + p.stderr
    rows = []
    for m in L.finditer(text):
        g = m.groups()
        rows.append(dict(op=int(g[1]), src=int(g[2]), dst=int(g[3]), cls=g[4], why=g[5], w=int(g[6]), h=int(g[7]), sm=int(g[10]), sl=int(g[11]), dm=int(g[12]), dl=int(g[13])))
    v = re.findall(r'validation errors (\d+)', text)
    return rows, (int(v[-1]) if v else -1), p.returncode, text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')))
    ap.add_argument('--frame', type=int, default=100)
    a = ap.parse_args()
    rows, val, rc, _ = listing(a.root, a.frame)
    print('frame %d: %d copies listed, validation errors %d, exit %d' % (a.frame, len(rows), val, rc))
    groups = collections.OrderedDict()
    for r in rows:
        k = (r['cls'], r['why'], '%dx%d' % (r['w'], r['h']))
        groups.setdefault(k, []).append(r)
    for k, v in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        print('%3d  %-13s %-9s %s   ops %s' % (len(v), k[0], k[2], k[1], ','.join(str(x['op']) for x in v[:8])))
    return 0


if __name__ == '__main__':
    sys.exit(main())
