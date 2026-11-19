#!/usr/bin/env python3
r"""S103 SVO: goto's `scene:` stage push in the stream-free (OFF) build, offline (0 game runs).

  py -I tools\scene\svo_check.py --unit  [--root DIR]     the guest-thread stamp (stage_stamp_unit.cpp), needs no capture
  py -I tools\scene\svo_check.py --rig   [--root DIR]     the host path (host_rig.exe on c2d_match_hud + its rows), needs the main checkout's captures

--unit builds tools\scene\stage_stamp_unit.cpp against the tree's own live_scene.cpp + record_io.cpp and runs it: the stamp (kSceneStagePlaying) is in the record the sink gets from live::EndFrame and from
LatchFrame + EmitLatched while the director says "playing" and the switch is on; not with the switch off; cleared on the next frame; taken at build time. The planted twin (--plant nostamp: the switch never
reaches the stamp) must make it FAIL.

--rig drives the real SceneHost (host_rig.exe, tools\scene\build_host_rig.ps1) with c2d_match_hud: the 2D rows of out\s103\p7h2\rows_match_hud\r60 on frame 60 only, frames 50 / 60 / 70.
  arm N  no stamp, rows:        frame 60 draws rows (drawn > 0) and its picture differs from the no-rows baseline B (the test can see the rows)
  arm S  stamped, rows:         frame 60 draws NO row (rows seen 0, drawn 0), every picture equals B (max |diff| 0), the census says stage_rows_dropped > 0 and the "[host][stage_view] on" line is printed once
  twin   stamped, --plant stage_ignore (the host ignores the stamp): frame 60 differs from B: must be CAUGHT.
A row the machine cannot run (a capture or rows folder missing) is SKIP, never PASS (the gate calls it needs-main). Exit 0 = pass."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
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


def unit(root):
    out = os.path.join(root, 'out', 'svo')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'stage_stamp_unit.exe')
    srcs = [os.path.join(root, p) for p in ('tools/scene/stage_stamp_unit.cpp', 'src/renderer_scene/live_scene.cpp', 'src/renderer_scene/record_io.cpp')]
    rc, t = run([CLANG, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-DNOMINMAX', '-o', exe] + srcs)
    line(rc == 0, 'stage_stamp_unit builds against the tree\'s live_scene.cpp + record_io.cpp' + ('' if rc == 0 else ':\n' + t[-1500:]))
    if rc:
        return
    rc, t = run([exe])
    n_pass = len(re.findall(r'^PASS ', t, re.M))
    line(rc == 0 and 'all passed' in t and 'FAIL' not in t and n_pass >= 13, 'the stamp unit: %d checks pass (EndFrame and LatchFrame + EmitLatched, switch on / off, cleared, build-time, file round trip)' % n_pass)
    rc, t = run([exe, '--plant', 'nostamp'])
    line(rc != 0 and re.search(r'^FAIL .*switch on, stage push playing: the record is stamped', t, re.M) is not None, 'planted twin nostamp (the switch never reaches the stamp) must fail: %s' % ('caught' if rc != 0 else 'NOT CAUGHT'))


def rig(root):
    global skips
    main = main_checkout(root)
    exe = os.path.join(root, 'out', 'tools', 'host_rig.exe')
    if not os.path.exists(exe):
        exe = os.path.join(main, 'out', 'tools', 'host_rig.exe')
    cap = os.path.join(main, 'out', 'captures', 'c2d_match_hud')
    rows = os.path.join(main, 'out', 's103', 'p7h2', 'rows_match_hud')
    game = os.path.join(main, 'game_data', 'extracted')
    if not (os.path.exists(exe) and os.path.isdir(os.path.join(cap, 'scene')) and os.path.isdir(os.path.join(rows, 'r60')) and os.path.isdir(game)):
        skips += 1
        print('SKIP  the rig arms need host_rig.exe, out/captures/c2d_match_hud, out/s103/p7h2/rows_match_hud/r60 and the game files: needs-main', flush=True)
        return
    import numpy as np
    from PIL import Image

    def img(p):
        return np.asarray(Image.open(p).convert('RGB'), dtype=np.int16)

    def diff(a, b):
        pa, pb = img(a), img(b)
        return 255 if pa.shape != pb.shape else int(np.abs(pa - pb).max())

    rc, t = run([exe, '--mode', 'sizes', '--capture', cap])
    m = re.search(r'distinct sizes: (\d+)x(\d+)', t)
    if rc or not m:
        line(False, 'size read failed')
        return
    size = '%sx%s' % m.groups()
    frames = '50,60,70'
    od_root = os.path.join(root, 'out', 'svo', 'rig')

    def arm(name, extra):
        od = os.path.join(od_root, name)
        os.makedirs(od, exist_ok=True)
        rc, t = run([exe, '--mode', 'host', '--scene', os.path.join(cap, 'scene'), '--game', game, '--out', od, '--frames', frames, '--size', size, '--capture', cap, '--no-curve'] + extra)
        return rc, t, od

    def row60(t):
        m = re.search(r'rows frame 60: (\d+) rows \((\d+) in the record, holes \d+\), drawn (\d+)', t)
        return tuple(map(int, m.groups())) if m else None

    rx = ['--rows-dir', rows, '--rows-only', '60']
    rcb, tb, odb = arm('B_norows', [])
    rcn, tn, odn = arm('N_rows', rx)
    rcs, ts, ods = arm('S_stamped', rx + ['--stage-stamp'])
    rcp, tp, odp = arm('P_stamped_ignored', rx + ['--stage-stamp', '--plant', 'stage_ignore'])
    if rcb or rcn or rcs:
        skips += 1
        print('SKIP  the rig arms did not run (rc %d / %d / %d; the 3D of that capture did not draw here): %s' % (rcb, rcn, rcs, (tb if rcb else tn if rcn else ts)[-300:].replace('\n', ' ')), flush=True)
        return
    rn, rs = row60(tn), row60(ts)
    print('   N frame 60 (seen, in record, drawn): %s   S: %s' % (rn, rs))
    line(rn is not None and rn[2] > 0, 'arm N (no stamp): frame 60 draws its rows (drawn %s)' % (rn[2] if rn else '?'))
    dn = diff(os.path.join(odn, 'host_60.png'), os.path.join(odb, 'host_60.png'))
    line(dn > 0, 'arm N: the rows change the picture against the no-rows baseline (max |diff| %d): the check can see them' % dn)
    line(rs is not None and rs[0] == 0 and rs[2] == 0, 'arm S (stamped): frame 60 draws no row (seen %s, drawn %s)' % (rs[0] if rs else '?', rs[2] if rs else '?'))
    worst = max(diff(os.path.join(ods, 'host_%d.png' % f), os.path.join(odb, 'host_%d.png' % f)) for f in (50, 60, 70))
    line(worst == 0, 'arm S: every picture equals the no-rows baseline (max |diff| %d): the 3D shows as it is' % worst)
    m = re.search(r'\[host\]\[stage_view\] stage_frames (\d+), stage_rows_dropped (\d+)', ts)
    line(m is not None and int(m.group(2)) > 0, 'arm S: the census counts the rows left out (%s)' % (m.group(0) if m else 'no line'))
    line(len(re.findall(r'\[host\]\[stage_view\] on: ', ts)) == 1, 'arm S: the "on" line is printed once (%d)' % len(re.findall(r'\[host\]\[stage_view\] on: ', ts)))
    line('[host][stage_view]' not in tn and '[host][stage_view]' not in tb, 'arms N and B (no stamp): no stage_view line at all')
    if rcp:
        line(False, 'planted twin stage_ignore: the host failed for its own reasons (rc %d), not a catch' % rcp)
    else:
        dp = diff(os.path.join(odp, 'host_60.png'), os.path.join(odb, 'host_60.png'))
        line(dp > 0, 'planted twin stage_ignore (a stamped record still draws its rows) must fail: %s (max |diff| vs baseline %d)' % ('caught' if dp > 0 else 'NOT CAUGHT', dp))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--unit', action='store_true')
    ap.add_argument('--rig', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    if not (a.unit or a.rig):
        a.unit = a.rig = True
    if a.unit:
        unit(root)
    if a.rig:
        rig(root)
    print('svo_check: %s (%d skipped)' % ('PASS' if not fails else 'FAIL (%d)' % fails, skips))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
