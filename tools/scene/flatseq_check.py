#!/usr/bin/env python3
"""S111 FLATSEQ: a flat frame's particle groups through the REAL SceneHost::DrawRows (out\\tools\\host_rig.exe --mode flatseq, tools\\scene\\host_rig_flatseq.h: a headless offscreen
renderer, no window, no game, no capture), then its planted twins (each must exit 1 and print FAIL flatseq.order). It builds host_rig.exe first (tools\\scene\\build_host_rig.ps1, compile
only, unchanged objects reused; --no-build skips it), so a proof that runs its lines in parallel cannot run the rig before it is built.
  py -I tools\\scene\\flatseq_check.py [--root DIR] [--no-build]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import subprocess
import sys

PLANTS = ('flat_parts_first', 'old_count', 'end')


def run(exe, root, extra):
    p = subprocess.run([exe, '--mode', 'flatseq'] + extra, capture_output=True, text=True, timeout=600, cwd=root)
    lines = [ln for ln in (p.stdout + '\n' + p.stderr).splitlines() if ln.startswith(('flatseq', 'FAIL flatseq', 'host_rig: FAIL'))]
    return p.returncode, lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')))
    ap.add_argument('--no-build', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    exe = os.path.join(root, 'out', 'tools', 'host_rig.exe')
    if not a.no_build:
        b = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'scene', 'build_host_rig.ps1')], capture_output=True, text=True, cwd=root)
        if b.returncode:
            print('FAIL: build_host_rig.ps1 failed\n' + (b.stdout + b.stderr)[-4000:])
            return 1
    if not os.path.isfile(exe):
        print('FAIL: no %s (pwsh -NoProfile -File tools\\scene\\build_host_rig.ps1)' % exe)
        return 1
    rc, lines = run(exe, root, [])
    print('clean run: rc %d' % rc)
    for ln in lines:
        print('  ' + ln)
    if rc != 0 or 'flatseq result: PASS' not in lines:
        print('FAIL: the flat frame\'s rows and particle groups through SceneHost::DrawRows')
        return 1
    bad = 0
    for plant in PLANTS:
        rc, lines = run(exe, root, ['--plant', plant])
        ok = rc == 1 and any(ln.startswith('FAIL flatseq.order') for ln in lines)
        print('twin %-16s %s (rc %d)' % (plant, 'caught' if ok else 'NOT CAUGHT', rc))
        if not ok:
            for ln in lines:
                print('  ' + ln)
        bad += 0 if ok else 1
    if bad:
        print('FAIL: %d twin(s) not caught' % bad)
        return 1
    print('OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
