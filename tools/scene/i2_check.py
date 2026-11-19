#!/usr/bin/env python3
r"""Lane I2 offline proof (Session 103, landing 2; 0 game runs; docs\renderer\REMNANT_MAP.md section 10): the 2D layer's SPRITES drawn from OUR rows.

  py -I tools\scene\i2_check.py [--root DIR] [--no-build] [--quick]

ONE capture: Brock's entrance, `post_cover_cache\20261111_174957_s100_owner_s89_brock\capture` of the main checkout. Frames 0-421 carry 9-14 sprites each (the tron / nameplate; hue-shifted ones at 200 / 350 / 416-421),
845-928 the fade and panels. fview replays the real frame through the replace seam in two arms and compares the pictures:
  stream   the game's own 2D pass (the reference, no hook)
  rows     --ui-ours --ui-quads --movie-ours --quads-2d-rows: every sprite (plain AND hue-shifted), every T_Rect and the movie quad is a Quad2D ROW built from the stream draw (the truth: four vertices with uv, c0 / c1, the
           picture KEY, the hue-shift triple read from the stream's pixel block), written and parsed through record section 58 (layout 2), and drawn by the drawer's pure function BlocksOf with the picture chosen by
           KEY from the key table (UiOurs' join) instead of the draw's own slot.
Bars (each line PASS / FAIL; a missing input FAILS, never skips):
  picture      rows vs stream: max channel difference <= 1 level and 0 pixels over 1 (owned_check's tolerance for a 2D pass) at every frame tried, at least one with a hue-shifted sprite in it
  census       over the WHOLE replay (1063 frames): sprite rows == the sprite draws the hook took over, hue-shifted rows > 0, drawn == rect + movie rows and every sprite row drawn, refused 0, section parser refused 0, state mismatch (blend,
               alpha test, hue program vs the row's hue constant, key table vs the join) 0; --ops: every sprite draw of the shown frame is an own quad (fate ui_quads_ours, geom_ours 2), the hue-shifted ones included
  validation   a 210-frame replay (it holds the hue-shifted frames) with the Vulkan validation layer on, rows on: errors 0
  plants       each planted twin on the same frames must be CAUGHT, the clean twin being within 1 level on them:
                 wrong_uv        the picture moves >= FAULT_PX pixels over 1 level (every picture zoomed)
                 drop_hsl        the picture moves at a frame with hue-shifted sprites (the plain program draws them)
                 wrong_picture   the picture moves (a key answered with another key's picture)
                 lost_blend      the blend-unknown counter rises at every frame (the drawer refuses a row it cannot blend: the stream's draw stays), the picture itself unchanged
INFO lines: the stream's sprite programs by name with their first frame (REMNANT_MAP U8), blend class and alpha tests of the sprite draws (U6).
Pictures and logs go under the main checkout's out\s103_i2\ ; nothing from the game is written elsewhere."""
import argparse
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
SPRITE = 0x82BC5688
CAP = 'out/post_cover_cache/20261111_174957_s100_owner_s89_brock/capture'
FRAMES = (20, 200, 350, 419, 900)
HSL_FRAMES = (200, 350, 419)
QUICK = (200, 419)
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
SPR_RE = re.compile(r'\[rdr\]\[2d\] sprites: rows (\d+), drawn (\d+), hue-shifted (\d+), drawn pictures: from the key table (\d+), the stream\'s own (\d+); state: hsl (\d+) blend-unknown (\d+) pic (\d+); plants applied: uv (\d+) hsl-dropped (\d+) pic-wrong (\d+); '
                    r'sprite-site programs: stream blend: off (\d+) alpha (\d+) add (\d+) other (\d+); alpha tests \(function@reference: draws\):([^\n]*?)((?: \w+_fragment \d+ \(first frame \d+\);)*)\s*$', re.M)
SKEYS = ['rows', 'drawn', 'hsl', 'pic_key', 'pic_stream', 'hsl_bad', 'blend_unknown', 'pic_bad', 'uv', 'hsl_dropped', 'pic_wrong', 'b_off', 'b_alpha', 'b_add', 'b_other', 'alpha_tests', 'programs']


def parse(text):
    r = ROWS_RE.findall(text)
    s = SPR_RE.findall(text)
    rows = dict(zip(['rect', 'movie', 'drawn', 'refused', 'section_refused', 'blend_mismatch', 'alpha_mismatch'], [int(x) for x in r[-1]])) if r else None
    spr = dict(zip(SKEYS, [int(x) if i < 15 else x.strip() for i, x in enumerate(s[-1])])) if s else None
    return rows, spr


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
    cap = os.path.join(main_co, CAP)
    work = os.path.join(main_co, 'out', 's103_i2')
    cache = os.path.join(main_co, 'out', 's103_i1', 'idx')   # (the index cache lane I1 made; a missing one is rebuilt by fview)
    os.makedirs(cache, exist_ok=True)
    os.makedirs(work, exist_ok=True)
    if not os.path.isdir(os.path.join(game, 'pac', 'menu')):
        rep.line(False, 'input missing: the game files ' + game)
        return 1
    if not os.path.isfile(os.path.join(cap, 'images.bin')):
        rep.line(False, 'input missing: the capture ' + cap)
        return 1
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
    frames = QUICK if args.quick else FRAMES

    def fview(tag, frame, extra, ops=False):
        key = (tag, frame, ops)
        if key in cached:
            return cached[key]
        d = os.path.join(work, 'arm_%s_%d' % (tag, frame))
        rc, text = run([fexe, '--capture', cap, '--frame', str(frame), '--root', root, '--judge-out', d] + extra, 1200, env)
        png = os.path.join(d, 'picture.png')
        if rc or not os.path.isfile(png):
            print(text[-2500:])
            raise RuntimeError('fview arm %s frame %d failed (exit %d)' % (tag, frame, rc))
        img = np.asarray(Image.open(png).convert('RGB'), dtype=np.int16)
        draws = None
        if ops:
            oj = d + '_ops.json'
            rc2, text2 = run([fexe, '--capture', cap, '--frame', str(frame), '--root', root, '--ops', '--json', oj] + extra, 1200, env)
            if rc2 or not os.path.isfile(oj):
                print(text2[-2000:])
                raise RuntimeError('fview ops arm %s frame %d failed (exit %d)' % (tag, frame, rc2))
            draws = [o for o in json.load(open(oj, encoding='utf-8'))['ops'] if o.get('kind') == 'draw']
        cached[key] = (img, text, draws)
        return cached[key]

    def diff(a, b):
        d = np.abs(a - b).max(axis=2)
        return int(d.max()), int((d > PIX).sum()), int((d > 0).sum())

    rows_args = ['--ui-ours', '--ui-quads', '--movie-ours', '--quads-2d-rows', '--game', game]
    saw_hsl_frame = False
    for f in frames:
        s, _, sd = fview('stream', f, [], ops=True)
        o, text, od = fview('rows', f, rows_args, ops=True)
        mx, over1, npx = diff(o, s)
        want_sprite = [d for d in sd if d['origin_op'] == 19 and d['call_site'] == SPRITE]
        hsl_draws = [d for d in want_sprite if d.get('ps_variant') == 'ySprite_hsl_fragment']
        own = [d for d in od if d['origin_op'] == 19 and d['call_site'] == SPRITE]
        own_ok = [d for d in own if d['fate'] == 'ui_quads_ours' and d.get('geom_ours') == 2]
        saw_hsl_frame = saw_hsl_frame or bool(hsl_draws)
        rep.line(mx <= PIX and over1 == 0 and 'ERRORS' not in text, 'frame %d: rows vs the stream\'s own 2D pass, max channel diff %d (%d pixels over %d, %d at 1), %d sprite draws of which %d hue-shifted' % (f, mx, over1, PIX, npx - over1, len(want_sprite), len(hsl_draws)))
        rep.line(bool(want_sprite) and len(own_ok) == len(want_sprite), 'frame %d census (--ops): every sprite draw of the frame (%d, %d hue-shifted) is an own quad (fate ui_quads_ours, geom_ours 2): %d' % (f, len(want_sprite), len(hsl_draws), len(own_ok)))
    rep.line(saw_hsl_frame, 'the frames tried include a hue-shifted sprite in the picture (frames %s)' % ', '.join(map(str, frames)))

    # ---- the whole replay: counts, state cross-checks ----------------------------------------------------------------------------------------------------------------------------
    rc, text = run([fexe, '--capture', cap, '--frames', '1063', '--replay', '--root', root] + rows_args, 1800, env)
    rows, spr = parse(text)
    qm = re.search(r'quads: decisions \d+, sprites (\d+), rects (\d+)', text)
    hook_sprites = int(qm.group(1)) if qm else -1
    ok = (rc == 0 and rows is not None and spr is not None and spr['rows'] == hook_sprites and spr['hsl'] > 0 and rows['drawn'] == rows['rect'] + rows['movie'] and spr['drawn'] == spr['rows'] and rows['refused'] == 0 and rows['section_refused'] == 0 and
          rows['blend_mismatch'] == 0 and rows['alpha_mismatch'] == 0 and spr['hsl_bad'] == 0 and spr['blend_unknown'] == 0 and spr['pic_bad'] == 0 and spr['pic_key'] + spr['pic_stream'] == spr['rows'])
    rep.line(ok, 'whole replay census: sprite rows %s (the hook took %d), hue-shifted %s, rect %s movie %s, drawn %s, refused %s, section-refused %s, state mismatch blend %s alpha %s hue-program %s picture %s; pictures from the key table %s, the stream\'s own %s (unjoined)' % (
        spr and spr['rows'], hook_sprites, spr and spr['hsl'], rows and rows['rect'], rows and rows['movie'], rows and rows['drawn'], rows and rows['refused'], rows and rows['section_refused'], rows and rows['blend_mismatch'],
        rows and rows['alpha_mismatch'], spr and spr['hsl_bad'], spr and spr['pic_bad'], spr and spr['pic_key'], spr and spr['pic_stream']))
    if spr:
        print('INFO  sprite-site programs (REMNANT_MAP U8): %s' % spr['programs'], flush=True)
        print('INFO  sprite stream blend (U6): off %d alpha %d add %d other %d; alpha tests (function@reference: draws) %s' % (spr['b_off'], spr['b_alpha'], spr['b_add'], spr['b_other'], spr['alpha_tests']), flush=True)

    rc, vt = run([fexe, '--capture', cap, '--frames', '210', '--validation', '1', '--replay', '--root', root] + rows_args, 1200, env)
    rep.line(rc == 0 and 'validation errors 0' in vt and 'validation active: yes' in vt, '210-frame replay (hue-shifted sprites in it) with the validation layer on and rows on, errors 0 (exit %d)' % rc)

    # ---- plants --------------------------------------------------------------------------------------------------------------------------------------------------------------------
    def planted(name, f):
        return fview('plant_' + name, f, rows_args + ['--quads-2d-fault', name])

    clean_ok = True
    for f in frames:
        s, _, _ = fview('stream', f, [])
        o, _, _ = fview('rows', f, rows_args)
        mx, over1, _ = diff(o, s)
        clean_ok = clean_ok and mx <= PIX and over1 == 0
    rep.line(clean_ok, 'plants.clean_twin: the rows arm is within %d level of the stream at every plant frame (%s)' % (PIX, ', '.join(map(str, frames))))
    for name, on, need in (('wrong_uv', frames, 1), ('wrong_picture', frames, 1), ('drop_hsl', tuple(f for f in HSL_FRAMES if f in frames) or HSL_FRAMES, 1)):
        per = []
        for f in on:
            s, _, _ = fview('stream', f, [])
            p, text, _ = planted(name, f)
            mx, over1, _ = diff(p, s)
            per.append((f, over1, mx))
        moved = [t for t in per if t[1] >= FAULT_PX]
        rep.line(len(moved) >= need, 'plant %s: the picture moves (>= %d pixels over %d level) at %d of %d frames (needs %d): %s' % (name, FAULT_PX, PIX, len(moved), len(on), need, ', '.join('f%d %d px (max %d)' % t for t in per)))
    res = []
    for f in frames:
        s, _, _ = fview('stream', f, [])
        p, text, _ = planted('lost_blend', f)
        _, sp = parse(text)
        mx, over1, _ = diff(p, s)
        res.append((f, sp['blend_unknown'] if sp else -1, mx, over1))
    rep.line(all(t[1] > 0 for t in res) and all(t[2] <= PIX and t[3] == 0 for t in res), 'plant lost_blend: the drawer refuses a row with no blend (counted at every frame), the stream\'s draw stays, the picture unchanged: %s' % ', '.join('f%d count %d (pic max %d)' % (t[0], t[1], t[2]) for t in res))
    print('%s  lane I2 sprite rows: %d failing check(s), %.0f s' % ('PASS' if not rep.fails else 'FAIL', rep.fails, time.time() - t0))
    return 1 if rep.fails else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except RuntimeError as e:
        print('FAIL  ' + str(e))
        sys.exit(1)
