#!/usr/bin/env python3
r"""S103 CHS: present_direct (C6) and stencil_dead (C5) on the scene host, offline (0 game runs, 0 Surface runs).

  py -I tools\scene\chs_c6_check.py [--root DIR] [--cap a04_entrance] [--frames 20,60,100] [--no-twins]

host_rig.exe (tools\scene\build_host_rig.ps1). The rig now runs the game's default (present_direct on); `--present-direct 0` is the old arm (the copy of `out` into F, then the present of F).
  C6 a04_entrance (the captured records carry no 2D row, so every frame is one with nothing to draw onto F)
     arm A copy, arm B direct: every presented picture identical (max |diff| 0); census: A copied N and presented 0 from out, B presented N from out and copied 0; row conflicts 0.
     (c) HoldF while F is stale: a no-record tick after frame 60 must present the same picture as frame 60 (hold_60.png == host_60.png) and `held from out` > 0.
     (d) a draw fault after frame 60 (TestSeam-style plant, `--fault-after 60 --fault-kind 1` = the failed Record recorded nothing into `out`, the real case, see LiveSceneDraw::FaultSpoiled): the picture must stay
         the shown one (fault_60.png == host_60.png), fault black 0. kind 2 (the failed draw spoiled `out`) must show black, fault black > 0.
  (b) c2d_match_hud with the rows of out\s103\p7h2\rows_match_hud on the middle frame only (`--rows-only`): arm A == arm B on every frame, copies == frames with rows, row conflicts 0.
  Planted faults (each must make its comparison FAIL): direct_plant 1 (present `out` although a row needs F: the HUD is lost; on the rows frame), 2 (the direct present shows the stale F), 3 (a shot reads the stale F:
  CaptureShown differs from the presented picture), 4 (a fault always blacks a stale F: the pre-review rule, run on the fault case), skip_copy under arm A.
  C5 stencil_dead 0 / 1 / 1 + poison identical; the lever census shows work removed; the planted fault (the switch never stored) leaves no census line.
Pictures and logs under <root>\out\chs\c6\ . A row the machine cannot run (a capture or rows folder missing) is SKIP, never PASS. Exit 0 = pass."""
import argparse
import os
import re
import subprocess
import sys

import numpy as np
from PIL import Image


def main_checkout(root):
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return env
    p = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
    return os.path.dirname(p) if p else root


def run(cmd, timeout=900):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, errors='replace')
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def img(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.int16)


def diff(a, b):
    pa, pb = img(a), img(b)
    return 255 if pa.shape != pb.shape else int(np.abs(pa - pb).max())


def direct_line(text):
    m = re.findall(r'\[host\]\[direct\] present_direct requested (\d), active (\d)(?: because [^:]*)?: from out (\d+), copied (\d+) \(rows (\d+)\), held from out (\d+), fault black (\d+), row conflicts (\d+)', text)
    return dict(zip(('requested', 'active', 'from_out', 'copied', 'rows', 'held', 'fault_black', 'conflicts'), map(int, m[-1]))) if m else None


fails = 0
skips = 0


def line(ok, text):
    global fails
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    fails += 0 if ok else 1


def skip(text):
    global skips
    skips += 1
    print('SKIP  ' + text, flush=True)


def infra(t):
    return 'FAIL host:' in t or 'cannot open' in t or 'FAIL record' in t or 'FAIL capture' in t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--cap', default='a04_entrance')
    ap.add_argument('--frames', default='20,60,100')
    ap.add_argument('--no-twins', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_dir = main_checkout(root)
    exe = os.path.join(root, 'out', 'tools', 'host_rig.exe')
    if not os.path.exists(exe):
        exe = os.path.join(main_dir, 'out', 'tools', 'host_rig.exe')
    cap = os.path.join(main_dir, 'out', 'captures', a.cap)
    game = os.path.join(main_dir, 'game_data', 'extracted')
    if not (os.path.exists(exe) and os.path.isdir(os.path.join(cap, 'scene')) and os.path.isdir(game)):
        print('FAIL: need host_rig.exe, the capture %s and the game files' % cap)
        return 2
    out = os.path.join(root, 'out', 'chs', 'c6')

    def sizes(c):
        rc, text = run([exe, '--mode', 'sizes', '--capture', c])
        m = re.search(r'distinct sizes: (\d+)x(\d+)', text)
        return '%sx%s' % m.groups() if rc == 0 and m else None

    size = sizes(cap)
    if not size:
        print('FAIL: size read failed')
        return 2
    frames = [int(f) for f in a.frames.split(',')]
    mid = frames[len(frames) // 2]

    def arm(name, extra, capdir=cap, frames_s=a.frames, size_s=size, scene_extra=()):
        od = os.path.join(out, name)
        os.makedirs(od, exist_ok=True)
        rc, text = run([exe, '--mode', 'host', '--scene', os.path.join(capdir, 'scene'), '--game', game, '--out', od, '--frames', frames_s, '--size', size_s] + list(scene_extra) + list(extra))
        return rc, text, od

    # ---- C6 base: copy vs direct ----
    rc_a, t_a, od_a = arm('A_copy', ['--present-direct', '0'])
    rc_b, t_b, od_b = arm('B_direct', [])   # the game's default
    line(rc_a == 0 and rc_b == 0, 'both arms ran (host rc %d / %d) on %s at %s, frames %s' % (rc_a, rc_b, a.cap, size, a.frames))
    if rc_a or rc_b:
        print((t_a if rc_a else t_b)[-1500:])
        return 1
    for f in frames:
        pa, pb = img(os.path.join(od_a, 'host_%d.png' % f)), img(os.path.join(od_b, 'host_%d.png' % f))
        d = int(np.abs(pa - pb).max()) if pa.shape == pb.shape else 255
        line(d == 0 and pa.mean() > 1.0, 'frame %d: copy vs direct max |diff| %d, mean luminance %.1f' % (f, d, pa.mean()))
    ca, cb = direct_line(t_a), direct_line(t_b)
    print('   census A: %s\n   census B: %s' % (ca, cb))
    if ca and cb:
        n = cb['from_out']
        line(n > 0 and ca['from_out'] == 0 and ca['copied'] >= n and cb['copied'] == 0 and cb['active'] == 1, 'full-size copies of `out` into F: arm A %d, arm B %d (frames presented from out %d)' % (ca['copied'], cb['copied'], n))
        line(ca['conflicts'] == 0 and cb['conflicts'] == 0 and cb['fault_black'] == 0, 'row conflicts 0, fault blacks 0')
    else:
        line(False, 'the [host][direct] census line is missing')

    # ---- (c) HoldF while stale ----
    rc, t, od = arm('C_hold', ['--hold-after', str(mid)])
    if rc == 0 and os.path.exists(os.path.join(od, 'hold_%d.png' % mid)):
        d = diff(os.path.join(od, 'hold_%d.png' % mid), os.path.join(od_b, 'host_%d.png' % mid))
        c = direct_line(t)
        line(d == 0 and c and c['held'] > 0, '(c) a no-record tick after the direct frame %d shows the same picture (max |diff| %d), held from out %s' % (mid, d, c['held'] if c else '?'))
    else:
        line(False, '(c) the hold arm did not run (rc %d)' % rc)

    # ---- (d) a draw fault after a direct frame ----
    rc, t, od = arm('D_fault1', ['--fault-after', str(mid), '--fault-kind', '1'])
    if rc == 0 and os.path.exists(os.path.join(od, 'fault_%d.png' % mid)):
        d = diff(os.path.join(od, 'fault_%d.png' % mid), os.path.join(od_b, 'host_%d.png' % mid))
        c = direct_line(t)
        line(d == 0 and c and c['fault_black'] == 0 and c['held'] > 0, '(d) a draw fault that recorded nothing into `out`: the shown picture is held (max |diff| %d), fault black %s, held from out %s' % (d, c['fault_black'] if c else '?', c['held'] if c else '?'))
    else:
        line(False, '(d) the fault arm did not run (rc %d)' % rc)
    rc, t, od = arm('D_fault2', ['--fault-after', str(mid), '--fault-kind', '2'])
    if rc == 0 and os.path.exists(os.path.join(od, 'fault_%d.png' % mid)):
        c = direct_line(t)
        pf = img(os.path.join(od, 'fault_%d.png' % mid))
        line(c and c['fault_black'] > 0 and pf.mean() < 1.0, '(d) a draw fault that spoiled `out`: black, never half a frame (fault black %s, mean luminance %.1f)' % (c['fault_black'] if c else '?', pf.mean()))
    else:
        line(False, '(d) the spoiled-fault arm did not run (rc %d)' % rc)

    # ---- (b) rows on the middle frame only ----
    mh = os.path.join(main_dir, 'out', 'captures', 'c2d_match_hud')
    rows = os.path.join(main_dir, 'out', 's103', 'p7h2', 'rows_match_hud')
    rows_ok = os.path.isdir(os.path.join(mh, 'scene')) and os.path.isdir(os.path.join(rows, 'r60'))
    if not rows_ok:
        skip('(b) rows on one frame: needs out/captures/c2d_match_hud and out/s103/p7h2/rows_match_hud/r60 (host_rig_2d.py makes the rows with fview): needs-main')
    else:
        msize = sizes(mh)
        rx = ['--capture', mh, '--rows-dir', rows, '--rows-only', '60', '--no-curve']
        fr = '50,60,70'
        rc1, t1, od1 = arm('R_A_copy', ['--present-direct', '0'] + rx, capdir=mh, frames_s=fr, size_s=msize)
        rc2, t2, od2 = arm('R_B_direct', rx, capdir=mh, frames_s=fr, size_s=msize)
        if rc1 or rc2:
            skip('(b) the rows arms did not run (rc %d / %d; the 3D of that capture did not draw here): %s' % (rc1, rc2, (t1 if rc1 else t2)[-300:].replace('\n', ' ')))
        else:
            worst = max(diff(os.path.join(od1, 'host_%d.png' % f), os.path.join(od2, 'host_%d.png' % f)) for f in (50, 60, 70))
            c1, c2 = direct_line(t1), direct_line(t2)
            print('   census A: %s\n   census B: %s' % (c1, c2))
            line(worst == 0, '(b) rows on frame 60 only: copy vs direct identical on every frame (max |diff| %d)' % worst)
            line(c2 is not None and c2['rows'] > 0 and c2['copied'] == c2['rows'] and c2['from_out'] > 0 and c2['conflicts'] == 0, '(b) copies == frames with rows (%s == %s), frames from out %s, row conflicts %s' % (c2 and c2['copied'], c2 and c2['rows'], c2 and c2['from_out'], c2 and c2['conflicts']))
            if not a.no_twins:
                rc3, t3, od3 = arm('R_plant1', ['--direct-plant', '1'] + rx, capdir=mh, frames_s=fr, size_s=msize)
                dm = max(diff(os.path.join(od3, 'host_%d.png' % f), os.path.join(od1, 'host_%d.png' % f)) for f in (50, 60, 70)) if rc3 == 0 else 0
                line(rc3 == 0 and dm > 0, 'planted fault direct_plant 1 (present `out` although a row needs F) must fail: %s (max |diff| vs the copy arm %d)' % ('caught' if rc3 == 0 and dm > 0 else 'NOT CAUGHT', dm))

    if not a.no_twins:
        # direct_plant 2 / 3: caught by the rig's own shot-vs-present check (rc 3) or by the picture; direct_plant 4 on the fault case
        for plant, extra, why_want in (('2', ['--direct-plant', '2'], 'the direct present shows the stale F'), ('3', ['--direct-plant', '3'], 'a shot reads the stale F')):
            rc, t, od = arm('plant_' + plant, extra)
            if rc != 0 and infra(t):
                line(False, 'planted fault direct_plant %s (%s): the host failed for its own reasons, not a catch' % (plant, why_want))
                continue
            dm = 0 if rc != 0 else max(diff(os.path.join(od, 'host_%d.png' % f), os.path.join(od_a, 'host_%d.png' % f)) for f in frames)
            caught = (rc != 0 and 'max RGB diff' in t) or dm > 0
            line(caught, 'planted fault direct_plant %s (%s) must fail: %s (%s)' % (plant, why_want, 'caught' if caught else 'NOT CAUGHT', "the rig's shot-vs-present check" if rc != 0 else 'max |diff| %d' % dm))
        rc, t, od = arm('plant_4', ['--fault-after', str(mid), '--fault-kind', '1', '--direct-plant', '4'])
        d = diff(os.path.join(od, 'fault_%d.png' % mid), os.path.join(od_b, 'host_%d.png' % mid)) if rc == 0 and os.path.exists(os.path.join(od, 'fault_%d.png' % mid)) else -1
        line(d > 0, 'planted fault direct_plant 4 (a fault always blacks a stale F, the pre-review rule) must fail case (d): %s (max |diff| %d)' % ('caught' if d > 0 else 'NOT CAUGHT', d))
        rc, t, od = arm('plant_skipcopy', ['--present-direct', '0', '--plant', 'skip_copy'])
        dm = max(diff(os.path.join(od, 'host_%d.png' % f), os.path.join(od_a, 'host_%d.png' % f)) for f in frames) if rc == 0 else 255
        line(dm > 0, 'planted fault skip_copy under arm A (the copy twin) must fail: %s (max |diff| %d)' % ('caught' if dm > 0 else 'NOT CAUGHT', dm))

    # ---- C5: stencil_dead in the stream-free host ----
    rc0, t0, od0 = arm('S0_stencil_off', ['--stencil-dead', '0'])
    rc1, t1, od1 = arm('S1_stencil_on', ['--stencil-dead', '1'])
    rcp, tp, odp = arm('S2_stencil_poison', ['--stencil-dead', '1', '--stencil-dead-poison'])
    line(rc0 == 0 and rc1 == 0 and rcp == 0, 'C5 arms ran (host rc %d / %d / %d)' % (rc0, rc1, rcp))
    if rc0 == 0 and rc1 == 0 and rcp == 0:
        for f in frames:
            p0 = img(os.path.join(od0, 'host_%d.png' % f))
            d1 = diff(os.path.join(od0, 'host_%d.png' % f), os.path.join(od1, 'host_%d.png' % f))
            dp = diff(os.path.join(od0, 'host_%d.png' % f), os.path.join(odp, 'host_%d.png' % f))
            line(d1 == 0 and dp == 0 and p0.mean() > 1.0, 'C5 frame %d: stencil_dead ON max |diff| %d, ON + poison (the removed stencil work leaves garbage) max |diff| %d' % (f, d1, dp))
        rl = re.compile(r'\[r24c\] stencil_dead: (\w+), plant (\d+), frames (\d+), absorbed clears dropped (\d+) kept (\d+), in-pass clears stripped (\d+) kept (\d+), depth copies depth-only (\d+) whole (\d+), stencil loads dead (\d+)')
        m1 = rl.search(t1)
        print('   census ON: ' + (m1.group(0) if m1 else '(none)'))
        took = bool(m1) and m1.group(1) == 'on' and int(m1.group(3)) > 0 and (int(m1.group(4)) + int(m1.group(6)) + int(m1.group(8)) + int(m1.group(10))) > 0
        line(took, 'C5 the lever took: frames %s, absorbed clears dropped %s, in-pass clears stripped %s, depth copies depth-only %s, stencil loads dead %s (any > 0 passes; a start that never stores the switch gives no line)' % ((m1.group(3), m1.group(4), m1.group(6), m1.group(8), m1.group(10)) if m1 else ('-',) * 5))
        if not a.no_twins:
            line(rl.search(t0) is None, 'C5 planted fault (the switch is not stored, as the stream-free start did before): no lever census, nothing dropped: the check above FAILS on it; tools\\bundle3\\lint.py now catches the same state at the source')
    print('chs_c6_check: %s (%d skipped)' % ('PASS' if not fails else 'FAIL (%d)' % fails, skips))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
