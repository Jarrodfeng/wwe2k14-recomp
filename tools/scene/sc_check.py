#!/usr/bin/env python3
r"""Lane SC offline proof (Session 103, landing 2: our 2D must clip like the game's; 0 game runs): the 2D rows' own scissor (Quad2D layout 4) against the stream's 2D pass.

  py -I tools\scene\sc_check.py [--root DIR] [--no-build] [--quick]

Arms (fview, offline): the stream's own 2D pass vs `--ui-ours --ui-quads --movie-ours --quads-2d-rows` on frames of out\captures\c2d_{menu,options,play,select} (main checkout). In the rows arm each own draw is clipped by THE ROW'S scissor
(rig: the row's box -> ReplaceOut::OwnDraw::scissor; a row with no scissor is drawn over everything), so a missing / wrong row scissor shows in the picture.
Bars (each a FAIL when missed):
  picture    rows arm vs stream: max channel difference <= 1 level and 0 pixels over 1 (the existing quads2d rig bar), on frames that had list clipping too
  clipping   the rows arm's scissor census line reports rows clipped smaller than 1280x720 on the clipping frames (c2d_menu 7 ...): the bar is not vacuous
  twins      scissor_ignored / scissor_y_flipped / scissor_prev_row each move the picture (> 1 level) on a clipping frame; scissor_ignored's pixel count is the 'before' number (no scissor)
Pictures go under the main checkout's out\s103\sc\ . Nothing from the game is committed."""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CAPS = {
    'menu': ('out/captures/c2d_menu', (7, 40, 80)),
    'options': ('out/captures/c2d_options', (5, 30)),
    'play': ('out/captures/c2d_play', (5, 30)),
    'select': ('out/captures/c2d_select', (7, 40)),
}
QUICK = {'menu': (7,), 'options': (5,), 'play': (5,), 'select': (7,)}
TWINS = ('scissor_ignored', 'scissor_y_flipped', 'scissor_prev_row')
PIX = 1
SC_RE = re.compile(r'\[rdr\]\[2d\] scissor: rows with a scissor (\d+), clipping \(smaller than 1280x720\) (\d+); target: frame (\d+) offscreen (\d+); plants applied: (\d+)(?:; target class by surface role: unknown (\d+), by size \(no role table\) (\d+))?')


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout=1200, env=None):
    e = dict(os.environ)
    if env:
        e.update(env)
    for attempt in range(8):
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=e, errors='replace')
        text = (p.stdout or '') + (p.stderr or '')
        if 'Not enough graphics memory for the frame ring' in text:
            time.sleep(15)
            continue
        return p.returncode, text
    return p.returncode, text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--quick', action='store_true')
    args = ap.parse_args()
    fails = 0

    def line(ok, text):
        nonlocal fails
        print(('PASS' if ok else 'FAIL') + '  ' + text, flush=True)
        fails += 0 if ok else 1
        return ok

    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    work = os.path.join(main_co, 'out', 's103', 'sc')
    cache = os.path.join(main_co, 'out', 's103_i1', 'idx')
    os.makedirs(cache, exist_ok=True)
    os.makedirs(work, exist_ok=True)
    caps = {}
    for k, (rel, frames) in CAPS.items():
        p = os.path.join(main_co, rel)
        if not os.path.isfile(os.path.join(p, 'images.bin')):
            line(False, 'input missing: the capture ' + p)
            return 1
        caps[k] = p
    if not os.path.isdir(os.path.join(game, 'pac', 'menu')):
        line(False, 'input missing: the game files ' + game)
        return 1
    import numpy as np
    from PIL import Image
    t0 = time.time()
    if not args.no_build:
        rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], 2400)
        if rc:
            print(text[-3000:])
            line(False, 'fview build (exit %d)' % rc)
            return 1
    sys.path.insert(0, os.path.join(HERE, '..', 'fview'))
    import fview_exe as _fe
    fexe = _fe.find_or_exit(root=root, kind='dev')
    env = {'WWE2K14_UI_INDEX_CACHE': cache}
    cached = {}

    def fview(cap, tag, frame, extra):
        key = (cap, tag, frame)
        if key in cached:
            return cached[key]
        d = os.path.join(work, 'arm_%s_%s_%d' % (cap, tag, frame))
        rc, text = run([fexe, '--capture', caps[cap], '--frame', str(frame), '--root', root, '--judge-out', d] + extra, 1200, env)
        png = os.path.join(d, 'picture.png')
        if rc or not os.path.isfile(png):
            print(text[-2500:])
            raise RuntimeError('fview arm %s %s frame %d failed (exit %d)' % (cap, tag, frame, rc))
        cached[key] = (np.asarray(Image.open(png).convert('RGB'), dtype=np.int16), text)
        return cached[key]

    def diff(a, b):
        d = np.abs(a - b).max(axis=2)
        return int(d.max()), int((d > PIX).sum()), int((d > 0).sum())

    rows_args = ['--ui-ours', '--ui-quads', '--movie-ours', '--quads-2d-rows', '--game', game]
    plan = [(k, f) for k, (rel, frames) in CAPS.items() for f in (QUICK[k] if args.quick else frames)]
    clipping_frames = []
    for cap, f in plan:
        s, _ = fview(cap, 'stream', f, [])
        o, text = fview(cap, 'rows', f, rows_args)
        mx, over1, npx = diff(o, s)
        m = SC_RE.findall(text)
        n_rows, n_clip, n_frame, n_off = (int(x) for x in m[-1][:4]) if m else (-1, -1, -1, -1)
        n_unk = int(m[-1][5] or 0) if m else -1   # review F3: rows whose target surface could not be read (holes)
        n_size = int(m[-1][6] or 0) if m else -1   # captures with no role table: the class fell back to the image's size
        if n_clip > 0:
            clipping_frames.append((cap, f))
        line(mx <= PIX and over1 == 0 and 'ERRORS' not in text and n_rows >= 0 and n_unk == 0,
             '%s frame %d: rows (row scissor) vs the stream, max channel diff %d (%d pixels over %d); rows with a scissor %d, clipping %d, target (by the surface) frame %d offscreen %d, unknown %d, class by size %d' % (cap, f, mx, over1, PIX, n_rows, n_clip, n_frame, n_off, n_unk, n_size))
    line(('menu', 7) in clipping_frames, 'c2d_menu frame 7 has rows clipped by the game scissor (the list clipping: the bar is not vacuous); clipping frames: %s' % clipping_frames)
    tcap, tf = ('menu', 7)
    for tw in TWINS:
        s, _ = fview(tcap, 'stream', tf, [])
        t, _ = fview(tcap, 'twin_' + tw, tf, rows_args + ['--quads-2d-fault', tw])
        mx, over1, npx = diff(t, s)
        line(mx > PIX, 'twin %s: caught on %s frame %d (max channel diff %d, %d pixels over %d%s)' % (tw, tcap, tf, mx, over1, PIX, '; this is the no-scissor "before" number' if tw == 'scissor_ignored' else ''))
    print('%s  lane SC scissor: %d failing check(s), %.0f s' % ('PASS' if not fails else 'FAIL', fails, time.time() - t0))
    return 1 if fails else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except RuntimeError as e:
        print('FAIL  ' + str(e))
        sys.exit(1)
