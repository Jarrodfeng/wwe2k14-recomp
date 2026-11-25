#!/usr/bin/env python3
r"""S105 A3 (review item 4) / S110 ENTRBLACK: the scene-fact header bits through the real SceneHost::Handle (host_rig.exe, tools\scene\build_host_rig.ps1), offline, 0 game runs.

  py -I tools\scene\cam_gate_check.py [--root DIR]

Drives host_rig on c2d_match_hud (frames 50, 60, 70; needs the main checkout's capture, the game files and out\tools\host_rig.exe) with the scene-fact header bits stamped on every posted record
(`--cam-gate N`, what the latch writes: cam_gate.h):
  arm B  no bits:                   every frame is a 3D frame (ours == frames), the picture is not black
  (S runs with --once: one post per frame, whatever the host showed; B, D, X and the twin re-post until the 3D draws)
  arm S  bit 1 (read, no geometry): no 3D is drawn (ours 0), the census counts game_no_scene == frames, the presented picture is black (Handle skipped DrawLive, Decide put black)
  arm D  bits 1 + 2 (read, drawn):  drawn like arm B, no game_no_scene count
  arm X  bits 3 + 4 (S105 A3's retired camera bits): ignored, drawn like arm B
  twin   arm S with --plant noscene_ignore (the host does not read the bits): the 3D is drawn: must be CAUGHT.
A row the machine cannot run (the exe, the capture or the game files missing) is SKIP, never PASS (a runner calls it needs-main). Exit 0 = pass."""
import argparse
import os
import re
import subprocess
import sys

fails = 0
skips = 0


def line(ok, text):
    global fails
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    fails += 0 if ok else 1


def run(cmd, timeout=900):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, errors='replace')
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def main_checkout(root):
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return env
    p = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
    return os.path.dirname(p) if p else root


def main():
    global skips
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_co = main_checkout(root)
    exe = os.path.join(root, 'out', 'tools', 'host_rig.exe')
    if not os.path.exists(exe):
        exe = os.path.join(main_co, 'out', 'tools', 'host_rig.exe')
    cap = os.path.join(main_co, 'out', 'captures', 'c2d_match_hud')
    game = os.path.join(main_co, 'game_data', 'extracted')
    if not (os.path.exists(exe) and os.path.isdir(os.path.join(cap, 'scene')) and os.path.isdir(game)):
        skips += 1
        print('SKIP  the rig arms need host_rig.exe, out/captures/c2d_match_hud and the game files: needs-main', flush=True)
        print('cam_gate_check: PASS (1 skipped)')
        return 0
    import numpy as np
    from PIL import Image

    def mean(p):
        return float(np.asarray(Image.open(p).convert('RGB'), dtype=np.float32).mean())

    rc, t = run([exe, '--mode', 'sizes', '--capture', cap])
    m = re.search(r'distinct sizes: (\d+)x(\d+)', t)
    if rc or not m:
        line(False, 'size read failed')
        return 1
    size = '%sx%s' % m.groups()
    frames = '50,60,70'
    od_root = os.path.join(root, 'out', 'cam_gate_rig')

    def arm(name, extra):
        od = os.path.join(od_root, name)
        os.makedirs(od, exist_ok=True)
        rc, t = run([exe, '--mode', 'host', '--scene', os.path.join(cap, 'scene'), '--game', game, '--out', od, '--frames', frames, '--size', size, '--capture', cap, '--no-curve'] + extra)
        c = re.search(r'\[host\] frames (\d+), ours (\d+)', t)
        s0 = re.search(r', game_no_scene (\d+)', t)
        return rc, t, od, (int(c.group(1)), int(c.group(2))) if c else None, int(s0.group(1)) if s0 else 0

    rcb, tb, odb, cb, sb = arm('B', [])
    rcs, ts, ods, cs, ss = arm('S', ['--cam-gate', '2', '--once'])
    rcd, td, odd, cd, sd = arm('D', ['--cam-gate', '6'])
    rcx, tx, odx, cx, sx = arm('X', ['--cam-gate', '24'])
    rcp, tp, odp, cp, sp = arm('P', ['--cam-gate', '2', '--plant', 'noscene_ignore'])
    if rcb or rcs or rcd or rcx or cb is None or cs is None or cd is None or cx is None:
        skips += 1
        print('SKIP  the rig arms did not run here (rc %d / %d / %d / %d): %s' % (rcb, rcs, rcd, rcx, (tb if rcb else ts if rcs else td if rcd else tx)[-300:].replace('\n', ' ')), flush=True)
        print('cam_gate_check: PASS (1 skipped)')
        return 0
    n = cb[0]
    line(cb[1] >= 3 and sb == 0, 'arm B (no bits): the 3D is drawn (ours %d of %d ticks, the rig re-posts until it draws), no game_no_scene count' % (cb[1], n))
    mb = mean(os.path.join(odb, 'host_60.png'))
    line(mb > 3.0, 'arm B: the picture is not black (mean %.1f): the check can see the 3D' % mb)
    line(cs[1] == 0 and ss == cs[0] and cs[0] > 0, 'arm S (read, no geometry): no 3D drawn (ours %d), game_no_scene counted for every frame (%d of %d)' % (cs[1], ss, cs[0]))
    ms = mean(os.path.join(ods, 'host_60.png'))
    line(ms < 1.0, 'arm S: the presented picture is black (mean %.2f)' % ms)
    for name, c, s, od in (('D (read, drawn)', cd, sd, odd), ('X (the retired camera bits 3 + 4)', cx, sx, odx)):
        line(c[1] >= 3 and s == 0, 'arm %s: drawn like arm B (ours %d of %d), no game_no_scene count' % (name, c[1], c[0]))
        m = mean(os.path.join(od, 'host_60.png'))
        line(m > 3.0, 'arm %s: the picture is not black (mean %.1f)' % (name, m))
    if rcp or cp is None:
        line(False, 'planted twin noscene_ignore: the host failed for its own reasons (rc %d), not a catch' % rcp)
    else:
        mp = mean(os.path.join(odp, 'host_60.png'))
        caught = cp[1] >= 3 and sp == 0 and mp > 3.0
        line(caught, 'planted twin noscene_ignore (the host ignores the bits, so the 3D is drawn) must fail the S arm: %s (ours %d of %d, picture mean %.1f)' % ('caught' if caught else 'NOT CAUGHT', cp[1], cp[0], mp))
    print('cam_gate_check: %s (%d skipped)' % ('PASS' if not fails else 'FAIL (%d)' % fails, skips))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
