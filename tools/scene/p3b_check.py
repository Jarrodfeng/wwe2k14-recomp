#!/usr/bin/env python3
r"""Lane P3b offline proof (Session 103, landing 2; 0 game runs; docs\renderer\LANDING2_PLAN.md section 5 row P3b): the menu leaves drawn from OUR 2D rows.

  py -I tools\scene\p3b_check.py [--root DIR] [--no-build] [--quick]

Kinds proved (the captures of the main checkout that hold their draws; found by the census of 2026-11-17, see docs\renderer\P3B_MENU_LEAVES.md):
  M   sub_82BC8180's plain T_Sprite (technique 0x8206AA0C) (call site 0x82BC8BEC, a 4-vertex strip, yAudience_color_alpha): out\captures\c2d_menu (frames 7 ...), c2d_play, c2d_options, c2d_oneonone
  R   sub_82BC4450's flat T_Rect (call site 0x82BC484C, a 4-vertex strip, yDebugPrimitive): the same four captures + c2d_select
Kinds with NO capture holding a draw (they stay holes; the unit proof tools\scene\p3b_unit.cpp covers the tap side): sub_82BC4450's band loop 0x82BC47C8 (T_RectRGradation), sub_82BC8180's loop 0x82BC8B68,
sub_82BC3F30's T_RectColor 0x82BC4404 (c2d_load has none), sub_82BC3098's textured rect 0x82BC32B8 (in_ring: c2d_match_hud / pause have none).
RESULT OF THE CENSUS (2026-11-17): every draw of both sites in every capture is MATRIX-PLACED (the leaf's c28 fUseProjMatrix = 1: sub_82BC2A30 uploads c28 = [object + 280] != 0 and the object's 4x4 matrix as c29..c32). The pixel-unit
drawer (BlocksOf: `use_proj != 0` -> Refuse::Projection) keeps such a draw, so the rows arm equals the stream by construction. Bars:
  picture      rows arm vs stream: max channel difference <= 1 level and 0 pixels over 1 at every frame tried (the rows path must do no harm)
  census       every draw of the shown frame from 0x82BC8BEC / 0x82BC484C is kept for reason `projection` (UiQuads' counter, rig only) and none is an own quad
  info         no capture holds a pixel-unit strip-site draw: the drawn-row picture proof and its plants wait for the matrix variant of the drawer (docs\renderer\P3B_MENU_LEAVES.md)
Pictures and logs go under the main checkout's out\s103_p3b\ ; nothing from the game is written elsewhere."""
import argparse
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
SITE_M, SITE_R = 0x82BC8BEC, 0x82BC484C
# (capture, frames): every frame holds both kinds in play / options / oneonone (frame 5 on), the menu from frame 7; select holds only R
CAPS = {
    'menu': ('out/captures/c2d_menu', (7, 40, 80), 90),
    'play': ('out/captures/c2d_play', (5, 30), 60),
    'options': ('out/captures/c2d_options', (5, 30), 60),
    'select': ('out/captures/c2d_select', (7, 40), 90),
}
QUICK = {'menu': (7,), 'play': (5,), 'options': (), 'select': (7,)}
FAULT_PX = 20
PIX = 1


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
    for attempt in range(8):   # the GPU is shared: only the frame-ring refusal is retried
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=e, errors='replace')
        text = (p.stdout or '') + (p.stderr or '')
        if 'Not enough graphics memory for the frame ring' in text:
            time.sleep(15)
            continue
        return p.returncode, text
    return p.returncode, text


class Report:
    def __init__(self):
        self.fails = 0

    def line(self, ok, text):
        print(('PASS' if ok else 'FAIL') + '  ' + text, flush=True)
        self.fails += 0 if ok else 1
        return ok


ROWS_RE = re.compile(r'\[rdr\]\[2d\] rows: rect (\d+), movie (\d+), drawn (\d+), refused (\d+) \(.*?\), section-refused (\d+), state mismatch: blend (\d+) alpha (\d+);')
SPR_RE = re.compile(r'\[rdr\]\[2d\] sprites: rows (\d+), drawn (\d+), hue-shifted (\d+), drawn pictures: from the key table (\d+), the stream\'s own (\d+); state: hsl (\d+) blend-unknown (\d+) pic (\d+); plants applied: uv (\d+) hsl-dropped (\d+) pic-wrong (\d+);')
STRIP_RE = re.compile(r'\[rdr\]\[2d\] strips: rect rows (\d+), sprite-M rows (\d+), drawn (\d+); plants applied: strip-order (\d+)')


def parse(text):
    r, s, t = ROWS_RE.findall(text), SPR_RE.findall(text), STRIP_RE.findall(text)
    rows = dict(zip(['rect', 'movie', 'drawn', 'refused', 'section_refused', 'blend_mismatch', 'alpha_mismatch'], [int(x) for x in r[-1]])) if r else None
    spr = dict(zip(['rows', 'drawn', 'hsl', 'pic_key', 'pic_stream', 'hsl_bad', 'blend_unknown', 'pic_bad', 'uv', 'hsl_dropped', 'pic_wrong'], [int(x) for x in s[-1]])) if s else None
    strip = dict(zip(['rect', 'm', 'drawn', 'planted'], [int(x) for x in t[-1]])) if t else None
    return rows, spr, strip


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--quick', action='store_true')
    args = ap.parse_args()
    rep = Report()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    work = os.path.join(main_co, 'out', 's103_p3b')
    cache = os.path.join(main_co, 'out', 's103_i1', 'idx')
    os.makedirs(cache, exist_ok=True)
    os.makedirs(work, exist_ok=True)
    if not os.path.isdir(os.path.join(game, 'pac', 'menu')):
        rep.line(False, 'input missing: the game files ' + game)
        return 1
    caps = {}
    for k, (rel, frames, total) in CAPS.items():
        p = os.path.join(main_co, rel)
        if not os.path.isfile(os.path.join(p, 'images.bin')):
            rep.line(False, 'input missing: the capture ' + p)
            return 1
        caps[k] = p
    try:
        import numpy as np
        from PIL import Image
    except Exception as e:
        rep.line(False, 'numpy / Pillow are needed to compare pictures: %s' % e)
        return 1
    t0 = time.time()
    if not args.no_build:
        rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], 2400)
        if rc:
            print(text[-3000:])
            rep.line(False, 'fview build (exit %d)' % rc)
            return 1
    sys.path.insert(0, os.path.join(HERE, '..', 'fview'))
    import fview_exe as _fe   # one locator, refuses a stale exe
    fexe = _fe.find_or_exit(root=root, kind='dev')
    env = {'WWE2K14_UI_INDEX_CACHE': cache}
    cached = {}

    def fview(cap, tag, frame, extra, ops=False):
        key = (cap, tag, frame, ops)
        if key in cached:
            return cached[key]
        d = os.path.join(work, 'arm_%s_%s_%d' % (cap, tag, frame))
        rc, text = run([fexe, '--capture', caps[cap], '--frame', str(frame), '--root', root, '--judge-out', d] + extra, 1200, env)
        png = os.path.join(d, 'picture.png')
        if rc or not os.path.isfile(png):
            print(text[-2500:])
            raise RuntimeError('fview arm %s %s frame %d failed (exit %d)' % (cap, tag, frame, rc))
        img = np.asarray(Image.open(png).convert('RGB'), dtype=np.int16)
        draws = None
        if ops:
            oj = d + '_ops.json'
            rc2, text2 = run([fexe, '--capture', caps[cap], '--frame', str(frame), '--root', root, '--ops', '--json', oj] + extra, 1200, env)
            if rc2 or not os.path.isfile(oj):
                print(text2[-2000:])
                raise RuntimeError('fview ops arm %s %s frame %d failed (exit %d)' % (cap, tag, frame, rc2))
            draws = [o for o in json.load(open(oj, encoding='utf-8'))['ops'] if o.get('kind') == 'draw']
        cached[key] = (img, text, draws)
        return cached[key]

    def diff(a, b):
        d = np.abs(a - b).max(axis=2)
        return int(d.max()), int((d > PIX).sum()), int((d > 0).sum())

    rows_args = ['--ui-ours', '--ui-quads', '--movie-ours', '--quads-2d-rows', '--game', game]
    plan = [(k, f) for k, (rel, frames, total) in CAPS.items() for f in (QUICK[k] if args.quick else frames)]
    tot_m = tot_r = 0
    for cap, f in plan:
        s, _, sd = fview(cap, 'stream', f, [], ops=True)
        o, text, od = fview(cap, 'rows', f, rows_args, ops=True)
        mx, over1, npx = diff(o, s)
        want_m = [d for d in sd if d['origin_op'] == 19 and d['call_site'] == SITE_M]
        want_r = [d for d in sd if d['origin_op'] == 19 and d['call_site'] == SITE_R]
        own = [d for d in od if d['origin_op'] == 19 and d['call_site'] in (SITE_M, SITE_R) and d['fate'] == 'ui_quads_ours' and d.get('geom_ours') == 2]
        tot_m += len(want_m)
        tot_r += len(want_r)
        rep.line(mx <= PIX and over1 == 0 and 'ERRORS' not in text, '%s frame %d: rows vs the stream\'s own 2D pass, max channel diff %d (%d pixels over %d, %d at 1), %d M draws, %d R draws' % (cap, f, mx, over1, PIX, npx - over1, len(want_m), len(want_r)))
        pj = re.findall(r'projection \(c28 set, a subset of constants\) (\d+)', text)
        rep.line(len(own) == 0 and bool(pj) and int(pj[-1]) >= len(want_m) + len(want_r), '%s frame %d census (--ops): the %d draws of the frame from the two strip sites are all matrix-placed (c28 fUseProjMatrix set: kept by the pixel-unit drawer, reason projection %s >= them), none is an own quad (%d)' % (cap, f, len(want_m) + len(want_r), pj and pj[-1], len(own)))
    rep.line(tot_m > 0 and tot_r > 0, 'both kinds are present in the frames tried (M %d draws, R %d draws)' % (tot_m, tot_r))
    # the offline picture proof of a strip row DRAWN needs a capture with a pixel-unit (c28 = 0) draw from 0x82BC8BEC / 0x82BC484C: none of the 12 c2d_* captures holds one (every such draw is matrix-placed). It rides the landing-2 sweep.
    rep.line(True, 'INFO: no capture holds a pixel-unit (c28 = 0) strip-site draw: the drawn-row picture proof and its plants wait for one (the matrix variant of the drawer, docs/renderer/P3B_MENU_LEAVES.md); the tap / blocks side is tools/scene/p3b_unit.cpp')
    print('%s  lane P3b menu-leaf rows: %d failing check(s), %.0f s' % ('PASS' if not rep.fails else 'FAIL', rep.fails, time.time() - t0))
    return 1 if rep.fails else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except RuntimeError as e:
        print('FAIL  ' + str(e))
        sys.exit(1)
