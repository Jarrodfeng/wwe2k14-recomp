#!/usr/bin/env python3
"""S105 A2: builds and runs tools\\renderer\\wide_frame_unit.cpp (src\\renderer_host\\wide_frame.h: the stream-free host's ultrawide / tall-screen frame shape; 0 game runs, no GPU, no SDK),
then its planted twins (each must exit 1 and name its check), then the wiring lints:
  - the old refusal is gone: host_start.cpp has no kSceneFollowsWide and no RefuseSetting("ULTRAWIDE / TALL SCREEN FILL"), and present_fill is never set from Applied (the picture is never stretched);
  - the host passes the GAME's size, not F's, to the 2D drawing (scene_host.cpp DrawRows / RowsMayDraw / NoteStageRows calls name game_w_ / game_h_ behind the planted-twin switch only);
  - rf::BuildFrame applies the tall rule (record_frame.cpp names TallScale);
  - the two crowd ports scale the projection (crowd_latch_reader.h: ScaleScreenProjection is called in InitSingleScreen and InitScreenTest) and start_settings.cpp sets the scale.
  py -I tools\\renderer\\wide_frame_unit.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {1: 'box.centred', 2: 'tall.horizontal_kept', 3: 'menu.game_shape', 4: 'fullview.fade', 5: 'rowview.panel_tint_boxed', 6: 'rowview.box_origin', 7: 'rowview.hud_scissor', 8: 'scenebox.centred'}


def read(root, rel):
    with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
        return f.read()


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    return re.sub(r'//[^\n]*', '', s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    bad = []

    def need(ok, what):
        print(('ok   ' if ok else 'FAIL ') + what)
        if not ok:
            bad.append(what)

    if not os.path.exists(CLANG):
        print('FAIL: no compiler at ' + CLANG)
        return 1
    outdir = os.path.join(root, 'out', 'tools')
    os.makedirs(outdir, exist_ok=True)
    exe = os.path.join(outdir, 'wide_frame_unit.exe')
    b = subprocess.run([CLANG, '-std=c++20', '-Wall', '-Wextra', '-o', exe, os.path.join(root, 'tools', 'renderer', 'wide_frame_unit.cpp')], capture_output=True, text=True)
    if b.returncode != 0:
        print(b.stdout + b.stderr)
        print('FAIL: build')
        return 1
    r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    print(r.stdout.rstrip())
    need(r.returncode == 0, 'wide_frame_unit: every check passed')
    for n, check in PLANTS.items():
        p = subprocess.run([exe, 'plant', str(n)], capture_output=True, text=True, timeout=120)
        fail_lines = [ln for ln in p.stdout.splitlines() if ln.startswith('FAIL ')]
        need(p.returncode == 1 and any(check in ln for ln in fail_lines), 'plant %d caught by %s' % (n, check))

    hs = strip_comments(read(root, 'src/renderer_host/host_start.cpp'))
    need('kSceneFollowsWide' not in hs and 'ULTRAWIDE / TALL SCREEN FILL' not in hs, 'the old refusal (kSceneFollowsWide / RefuseSetting ULTRAWIDE) is gone from host_start.cpp')
    need(re.search(r'ro\.present_fill\s*=\s*false\s*;', hs) is not None and 'applied.present_fill' not in hs, 'host_start.cpp never stretches (present_fill = false, Applied::present_fill unread)')
    need(re.search(r'o\.wide\s*=\s*applied\.wide', hs) is not None and 'FillNow()' in hs, 'host_start.cpp passes the factors and the in-play verdict to the host')
    sc = strip_comments(read(root, 'src/renderer_host/scene_host.cpp'))
    # the three DrawRows calls read `DrawRows(fb, *m.record, j, <w>, <h>, ...)` (review F3: the old regex needed `DrawRows(*m.record` and never matched); RowsMayDraw reads `RowsMayDraw(*m.record, <w>, <h>, ...)`
    calls = re.findall(r'(?:DrawRows\(fb,\s*|RowsMayDraw\()\*m\.record[^;]*', sc)
    need(len(calls) >= 3, 'scene_host.cpp: the wiring lint sees the 2D calls (%d found, >= 3 wanted: two DrawRows sites + RowsMayDraw)' % len(calls))
    f_size = [c for c in calls if 'out_w_' in c.replace('seam_.wide_pass_f_size ? out_w_ : game_w_', '').replace('seam_.wide_pass_f_size ? out_h_ : game_h_', '')]
    need('seam_.wide_pass_f_size ? out_w_ : game_w_' in ' '.join(calls), 'scene_host.cpp: the 2D calls choose the frame size only behind the planted-twin switch')
    twin = re.sub(r'seam_\.wide_pass_f_size \? out_w_ : game_w_', 'out_w_', sc, count=1)
    twin_calls = re.findall(r'(?:DrawRows\(fb,\s*|RowsMayDraw\()\*m\.record[^;]*', twin)
    need(any('out_w_' in c.replace('seam_.wide_pass_f_size ? out_w_ : game_w_', '') for c in twin_calls), 'wiring lint twin: a DrawRows / RowsMayDraw call passing the frame size (out_w_) IS caught')
    need(not f_size, 'scene_host.cpp: no 2D call passes F\'s size without the planted-twin switch (%d bad)' % len(f_size))
    need(re.search(r'NoteStageRows[^;]*', sc) is not None and 'FateOf(r, opts_.draw_unknown_pass, out_w_' not in sc, 'scene_host.cpp: NoteStageRows asks FateOf with the game\'s size')
    rf = strip_comments(read(root, 'src/renderer_scene/record_frame.cpp'))
    need('TallScale(aspect)' in rf, 'record_frame.cpp: BuildFrame applies the tall rule')
    cl = strip_comments(read(root, 'src/scene_live/crowd_latch_reader.h'))
    need(len(re.findall(r'ScaleScreenProjection\(pj(?:, true)?\)', cl)) == 2, 'crowd_latch_reader.h: both screen-test ports scale the projection')
    ss = strip_comments(read(root, 'src/start_settings.cpp'))
    need('g_screen_sx_bits' in ss and 'FromSwitch(' in ss, 'start_settings.cpp sets the screen-test scale from the crowd switch')
    print('wide_frame_unit.py: %s' % ('PASS' if not bad else 'FAIL (%d)' % len(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
