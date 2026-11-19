#!/usr/bin/env python3
r"""Lane I1 offline proof (Session 103, landing 2; 0 game runs; docs\renderer\REMNANT_MAP.md section 10): the 2D layer drawn from OUR rows.

  py -I tools\scene\i1_check.py [--root DIR] [--no-build] [--quick]

ONE capture: Brock's entrance, `post_cover_cache\20261111_174957_s100_owner_s89_brock\capture` of the main checkout (frames 0-416 carry 3 T_Rect fades / panels, 9-14 sprites and the movie YUV
quad every frame: the tron / nameplate). fview replays the real frame through the replace seam in two arms and compares the pictures:
  stream   the game's own 2D pass (the reference, no hook)
  rows     --ui-ours --ui-quads --movie-ours --quads-2d-rows: every T_Rect and the movie quad of the frame is a Quad2D ROW built from the stream draw (the truth), written and parsed through the
           record's section 58, and drawn by the drawer's pure function BlocksOf (quads2d_blocks.h) instead of the stream's constants. Sprites, glyphs and the planes' pictures stay the stream's
           (this piece covers rects and the movie only).
Bars (each line PASS / FAIL; a missing input FAILS, never skips):
  picture      rows vs stream: max channel difference <= 1 level and 0 pixels over 1 (owned_check's tolerance for a 2D pass; the same bar ui_quads_check / movie_ours_check hold), at every frame tried
  census       the [rdr][2d] line: rect rows == the stream's T_Rect draws and movie rows == the stream's movie draws (counted from `fview --ops`), drawn == rows, refused 0, the section parser refused 0,
               blend / alpha-test state mismatches 0; and `--ops`: every such draw is fate ui_quads_ours / movie_ours with an own quad (geom_ours 2)
  validation   an 8-frame replay with the Vulkan validation layer on, rows on: errors 0
  plants       each planted twin on the same frames must be CAUGHT, the clean twin (rows) being within 1 level on them:
                 drop_rect / drop_movie   the picture moves >= FAULT_PX pixels over 1 level at some frame (the fade / the video is not drawn)
                 swap_vertices            the picture moves (a bow-tie rect)
                 stale_plane              the picture moves at a frame where the video advanced (last frame's planes)
                 wrong_blend              state mismatch counter > 0 (the picture cannot move: the seam keeps the stream's state); picture still within 1 level
                 lost_constants           the rect rows are refused (constants), counted; picture still within 1 level (the stream's draw stays)
INFO lines: the stream's blend class per kind (REMNANT_MAP U6), how many frames' alpha test is on.
Pictures and logs go under the main checkout's out\s103_i1\ ; nothing from the game is written elsewhere."""
import argparse
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
RECT, MOVIE = 0x82BC3EE8, 0x82BC7E50
CAP = 'out/post_cover_cache/20261111_174957_s100_owner_s89_brock/capture'
FRAMES = (20, 160, 300, 419)
RECT_FRAMES = (845, 850, 900)   # frames where a rect is visible [C: probed]: 845 / 850 = the full-screen fade (max 11 levels, 921,600 pixels), 900 = the panels (max 255 levels); the tron frames (0-416) draw their 3 rects invisibly
QUICK = (160, 300)
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


ROWS_RE = re.compile(r'\[rdr\]\[2d\] rows: rect (\d+), movie (\d+), drawn (\d+), refused (\d+) \(layout (\d+), kind (\d+), verts (\d+), constants (\d+), projection (\d+), finite (\d+), not-rect (\d+), blend (\d+), planes (\d+)(?:, variant \d+, picture \d+)?\), '   # S103 I2: the breakdown also lists variant / picture
                     r'section-refused (\d+), state mismatch: blend (\d+) alpha (\d+); stream blend: rect off (\d+) alpha (\d+) add (\d+) other (\d+), movie off (\d+) alpha (\d+) add (\d+) other (\d+); alpha test on (\d+); plants applied: dropped (\d+) stale (\d+); alpha tests \(kind function@reference: draws\):([^\n]*)')
KEYS = ['rect', 'movie', 'drawn', 'refused', 'layout', 'kind', 'verts', 'constants', 'projection', 'finite', 'not_rect', 'blend_unknown', 'planes', 'section_refused', 'blend_mismatch', 'alpha_mismatch',
        'r_off', 'r_alpha', 'r_add', 'r_other', 'm_off', 'm_alpha', 'm_add', 'm_other', 'alpha_test_on', 'dropped', 'stale', 'alpha_tests']


def parse_rows(text):
    m = ROWS_RE.findall(text)
    return dict(zip(KEYS, [int(x) if i < len(KEYS) - 1 else x.strip() for i, x in enumerate(m[-1])])) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--quick', action='store_true')
    ap.add_argument('--frames', default='', help='a comma list replacing the picture / census frames (diagnosis)')
    ap.add_argument('--rect-frames', default='', help='a comma list replacing the frames the rect plants run on (diagnosis)')
    args = ap.parse_args()
    rep = Report()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    cap = os.path.join(main_co, CAP)
    work = os.path.join(main_co, 'out', 's103_i1')
    cache = os.path.join(work, 'idx')
    os.makedirs(cache, exist_ok=True)
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
    frames = tuple(int(x) for x in args.frames.split(',')) if args.frames else QUICK if args.quick else FRAMES
    rect_frames = tuple(int(x) for x in args.rect_frames.split(',')) if args.rect_frames else RECT_FRAMES

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
    seen_blend = {}
    seen_alpha = {}
    for f in frames:
        s, _, sd = fview('stream', f, [], ops=True)
        o, text, od = fview('rows', f, rows_args, ops=True)
        mx, over1, npx = diff(o, s)
        r = parse_rows(text)
        want_rect = len([d for d in sd if d['origin_op'] == 19 and d['call_site'] == RECT])
        want_movie = len([d for d in sd if d['origin_op'] == 19 and d['call_site'] == MOVIE])
        own = [d for d in od if d['origin_op'] == 19 and d['call_site'] in (RECT, MOVIE)]
        own_ok = [d for d in own if d['fate'] in ('ui_quads_ours', 'movie_ours') and d.get('geom_ours') == 2]
        rep.line(mx <= PIX and over1 == 0 and 'ERRORS' not in text, 'frame %d: rows vs the stream\'s own 2D pass, max channel diff %d (%d pixels over %d, %d at 1)' % (f, mx, over1, PIX, npx - over1))
        # (fview replays the frames before the one asked for, so the rows line counts every frame it bound: it is compared with the hooks' own counts of the stream draws they took over, and the one frame the
        # --ops dump shows is compared draw by draw)
        qm = re.search(r'quads: decisions \d+, sprites \d+, rects (\d+)', text)
        mm = re.search(r'\[rdr\]\[movie\] decisions \d+, replaced (\d+)', text)
        hook_rect, hook_movie = (int(qm.group(1)) if qm else -1), (int(mm.group(1)) if mm else -1)
        ok = (r is not None and want_rect > 0 and want_movie > 0 and r['rect'] == hook_rect and r['movie'] == hook_movie and r['rect'] > 0 and r['drawn'] == r['rect'] + r['movie'] and r['refused'] == 0 and
              r['section_refused'] == 0 and r['blend_mismatch'] == 0 and r['alpha_mismatch'] == 0 and len(own_ok) == want_rect + want_movie)
        rep.line(ok, 'frame %d census: the frame\'s stream draws rect %d movie %d, all %d own (fate ours, own quad); rows over the replay: %s; the hooks took rect %d movie %d' % (
            f, want_rect, want_movie, len(own_ok), ('rect %d movie %d drawn %d refused %d section-refused %d state mismatch blend %d alpha %d' % (r['rect'], r['movie'], r['drawn'], r['refused'], r['section_refused'], r['blend_mismatch'], r['alpha_mismatch'])) if r else 'NO [rdr][2d] LINE',
            hook_rect, hook_movie))
        if r:
            seen_alpha[f] = r['alpha_tests']
            seen_blend[f] = (r['r_off'], r['r_alpha'], r['r_add'], r['r_other'], r['m_off'], r['m_alpha'], r['m_add'], r['m_other'], r['alpha_test_on'])
    if seen_alpha:
        print('INFO  alpha tests the stream\'s rect / movie draws carry (frame %d, whole replay) %s' % (max(seen_alpha), seen_alpha[max(seen_alpha)]), flush=True)
    for f, b in seen_blend.items():
        print('INFO  frame %d stream blend (REMNANT_MAP U6): rect off %d alpha %d add %d other %d; movie off %d alpha %d add %d other %d; alpha test on %d' % ((f,) + b), flush=True)

    rc, vt = run([fexe, '--capture', cap, '--frames', '8', '--validation', '1', '--replay', '--root', root] + rows_args, 1200, env)
    rep.line(rc == 0 and 'validation errors 0' in vt and 'validation active: yes' in vt, '8-frame replay with the validation layer on and rows on, errors 0 (exit %d)' % rc)

    # ---- plants ------------------------------------------------------------------------------------------------------------------------------------------------------------------
    def planted(name, f):
        return fview('plant_' + name, f, rows_args + ['--quads-2d-fault', name])

    clean_ok = True
    for f in tuple(frames) + tuple(x for x in rect_frames if x not in frames):
        s, _, _ = fview('stream', f, [])
        o, _, _ = fview('rows', f, rows_args)
        mx, over1, _ = diff(o, s)
        clean_ok = clean_ok and mx <= PIX and over1 == 0
    rep.line(clean_ok, 'plants.clean_twin: the rows arm is within %d level of the stream at every plant frame (%s)' % (PIX, ', '.join(map(str, tuple(frames) + tuple(x for x in rect_frames if x not in frames)))))
    for name in ('drop_rect', 'swap_vertices', 'drop_movie', 'stale_plane'):
        on = rect_frames if name in ('drop_rect', 'swap_vertices') else frames
        per = []
        for f in on:
            s, _, _ = fview('stream', f, [])
            p, text, _ = planted(name, f)
            mx, over1, npx = diff(p, s)
            per.append((f, over1, mx))
        moved = [t for t in per if t[1] >= FAULT_PX]
        # the video advances every frame of this capture, so last frame's planes differ from this frame's at every frame past the first few; a rect is visible on some frames only
        need = 2 if name == 'stale_plane' else 1
        ok = len(moved) >= need
        rep.line(ok, 'plant %s: the picture moves (>= %d pixels over %d level) at %d of %d frames (needs %d): %s' % (name, FAULT_PX, PIX, len(moved), len(on), need, ', '.join('f%d %d px (max %d)' % t for t in per)))
    for name, field in (('wrong_blend', 'blend_mismatch'), ('lost_constants', 'constants')):
        res = []
        for f in frames:
            s, _, _ = fview('stream', f, [])
            p, text, _ = planted(name, f)
            r = parse_rows(text)
            mx, over1, _ = diff(p, s)
            res.append((f, r[field] if r else -1, mx, over1))
        ok = all(t[1] > 0 for t in res) and all(t[2] <= PIX and t[3] == 0 for t in res)
        rep.line(ok, 'plant %s: caught by the %s counter at every frame, the picture itself unchanged (the seam keeps the stream\'s state): %s' % (name, field, ', '.join('f%d count %d (pic max %d)' % (t[0], t[1], t[2]) for t in res)))
    print('%s  lane I1 2D rows: %d failing check(s), %.0f s' % ('PASS' if not rep.fails else 'FAIL', rep.fails, time.time() - t0))
    return 1 if rep.fails else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except RuntimeError as e:
        print('FAIL  ' + str(e))
        sys.exit(1)
