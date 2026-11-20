#!/usr/bin/env python3
r"""Lane P3c offline proof (Session 103, landing 2; 0 game runs; docs\renderer\P3B_MENU_LEAVES.md section 5): the menu's MATRIX-PLACED 2D draws (c28 = 1: text glyphs, plain T_Sprite strips, flat T_Rect strips) drawn from OUR rows.

  py -I tools\scene\p3c_check.py [--root DIR] [--no-build] [--quick] [--sheet]

Arms (fview, offline, no game run): the stream's own 2D pass vs `--ui-ours --ui-quads --movie-ours --quads-2d-rows` on frames of out\captures\c2d_{menu,play,options,select,title,load} (main checkout).
Bars (every one a FAIL when missed):
  picture    rows arm vs stream: max channel difference <= 1 level and 0 pixels over 1 (the existing quads2d rig bar)
  whole      every row-site draw of the frame (sites 0x82BC3784 glyph, 0x82BC3EE8 rect, 0x82BC484C rect strip, 0x82BC5688 / 0x82BC8BEC sprite, 0x82BC7E50 movie) is replaced by our row (fate ui_quads_ours / movie_ours);
             `before` = a frame is 2D-whole without the matrix variant = it has no matrix-placed draw (the P3b state), reported next to `after`
  twins      each planted twin must move the picture: proj_identity (matrix ignored), proj_transposed, c28_ignored (the pixel-unit rule on a matrix-placed row): max diff > 1 on at least one frame
Pictures and logs go under the main checkout's out\s103\p3c\ ; the contact sheet (stream | ours | |diff| x 8) is out\s103\p3c\sheet.png. Nothing from the game is written elsewhere or committed."""
import argparse
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
ROW_SITES = {0x82BC3784, 0x82BC3EE8, 0x82BC484C, 0x82BC5688, 0x82BC8BEC, 0x82BC7E50, 0x82BC799C, 0x82BC4404}   # S105 A8: + T_RectColor
# the 2D leaf sites that make no row (quads2d_core.h kOp19Sites, SiteClass::Leaf2D): a frame with one of their draws is not 2D-whole whatever the rows do
LEAF_SITES = {0x82BC32B8, 0x82BC3758, 0x82BC47C8, 0x82BC5408, 0x82BC6748, 0x82BC69D0, 0x82BC6E78, 0x82BC6EFC, 0x82BC7208, 0x82BC723C, 0x82BC8B68}
OUR_FATES = ('ui_quads_ours', 'movie_ours')
# (capture, frames)
CAPS = {
    'menu': ('out/captures/c2d_menu', (7, 40, 80)),
    'play': ('out/captures/c2d_play', (5, 30)),
    'options': ('out/captures/c2d_options', (5, 30)),
    'select': ('out/captures/c2d_select', (7, 40)),
    'title': ('out/captures/c2d_title', (15, 40)),
    'load': ('out/captures/c2d_load', (20, 60)),
}
QUICK = {'menu': (7,), 'play': (5,), 'options': (5,), 'select': (7,), 'title': (15,), 'load': (20,)}
TWINS = ('proj_identity', 'proj_transposed', 'c28_ignored')
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


MAT_RE = re.compile(r'\[rdr\]\[2d\] matrix: rows rect (\d+) sprite (\d+) glyph (\d+), drawn rect (\d+) sprite (\d+) glyph (\d+); tap: strip rows (\d+) \(differ (\d+)\), glyph matrices through the register file (\d+); plants applied: (\d+)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--quick', action='store_true')
    ap.add_argument('--sheet', action='store_true')
    args = ap.parse_args()
    rep = Report()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    work = os.path.join(main_co, 'out', 's103', 'p3c')
    cache = os.path.join(main_co, 'out', 's103_i1', 'idx')
    os.makedirs(cache, exist_ok=True)
    os.makedirs(work, exist_ok=True)
    if not os.path.isdir(os.path.join(game, 'pac', 'menu')):
        rep.line(False, 'input missing: the game files ' + game)
        return 1
    caps = {}
    for k, (rel, frames) in CAPS.items():
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
    plan = [(k, f) for k, (rel, frames) in CAPS.items() for f in (QUICK[k] if args.quick else frames)]
    sheet_rows = []
    whole_before = whole_after = frames_with_2d = 0
    per_cap = {}
    for cap, f in plan:
        s, _, sd = fview(cap, 'stream', f, [], ops=True)
        o, text, od = fview(cap, 'rows', f, rows_args, ops=True)
        mx, over1, npx = diff(o, s)
        sites = [d for d in sd if d['origin_op'] == 19 and d['call_site'] in ROW_SITES]
        ours = [d for d in od if d['origin_op'] == 19 and d['call_site'] in ROW_SITES and d['fate'] in OUR_FATES]
        kept = [d for d in od if d['origin_op'] == 19 and d['call_site'] in ROW_SITES and d['fate'] not in OUR_FATES]
        m = MAT_RE.findall(text)
        mrows = sum(int(x) for x in m[-1][:3]) if m else -1
        mdrawn = sum(int(x) for x in m[-1][3:6]) if m else -1
        differ = int(m[-1][7]) if m else -1
        leaf = [d for d in sd if d['origin_op'] == 19 and d['call_site'] in LEAF_SITES]
        if (cap, f) in (('menu', 7), ('play', 5)):   # S103 P3c review: the matrix variant must actually DRAW rows on the two frames that carry matrix-placed draws (a 0 would pass a picture that simply drew nothing of ours)
            rep.line(mdrawn > 0, '%s frame %d: matrix-placed rows drawn by us %d (want > 0)' % (cap, f, mdrawn))
        if not sites:
            rep.line(mx <= PIX and over1 == 0, '%s frame %d: no row-site 2D draw in the frame (picture rows vs stream: max %d)' % (cap, f, mx))
            continue
        frames_with_2d += 1
        pc = per_cap.setdefault(cap, [0, 0, 0])
        pc[0] += 1
        # `before` (the P3b state): whole only when no draw of the frame is matrix-placed. The per-process counter `matrix rows` counts the frame replay's matrix-placed rows (a floor for this frame)
        was_whole = mrows == 0 and not kept and not leaf
        now_whole = not kept and not leaf
        whole_before += was_whole
        whole_after += now_whole
        pc[1] += was_whole
        pc[2] += now_whole
        rep.line(mx <= PIX and over1 == 0 and 'ERRORS' not in text and differ == 0,
                 '%s frame %d: rows vs the stream\'s own 2D pass, max channel diff %d (%d pixels over %d, %d at 1); %d row-site draws, %d ours (matrix-placed rows %d, drawn %d), %d kept, %d leaf-site draws (no row); tap rows differing %d; 2D-whole: before %s, after %s' %
                 (cap, f, mx, over1, PIX, npx - over1, len(sites), len(ours), mrows, mdrawn, len(kept), len(leaf), differ, 'yes' if was_whole else 'no', 'yes' if now_whole else 'no'))
        if kept:
            by = {}
            for d in kept:
                k = (hex(d['call_site']), d['fate'])
                by[k] = by.get(k, 0) + 1
            print('      kept draws by (site, fate): %s' % by)
        sheet_rows.append((cap, f, s, o))
    for cap, (n, b, a) in sorted(per_cap.items()):
        print('INFO  %-8s frames with row-site 2D draws %d: 2D-whole before %d, after %d' % (cap, n, b, a))
    rep.line(frames_with_2d > 0 and whole_after > whole_before, '2D-whole frames: before %d, after %d of %d frames with row-site draws (the rest hold a leaf-site draw that makes no row, listed above)' % (whole_before, whole_after, frames_with_2d))
    # planted twins: each must move the picture on some frame
    tcap, tf = ('play', 5) if args.quick else ('menu', 7)
    for tw in TWINS:
        worst = 0
        for cap, f in ((tcap, tf), ('play', 5)):
            s, _, _ = fview(cap, 'stream', f, [])
            t, text, _ = fview(cap, 'twin_' + tw, f, rows_args + ['--quads-2d-fault', tw])
            mx, over1, npx = diff(t, s)
            worst = max(worst, mx)
        rep.line(worst > PIX, 'twin %s: caught (max channel diff %d on %s / play frame 5; bar %d)' % (tw, worst, tcap, PIX))
    if args.sheet and sheet_rows:
        pick = sheet_rows[:: max(1, len(sheet_rows) // 8)][:8]
        h, w = pick[0][2].shape[:2]
        sc = 0.35
        tiles = []
        for cap, f, s, o in pick:
            d = np.clip(np.abs(s - o).max(axis=2) * 8, 0, 255).astype(np.uint8)
            row = np.concatenate([s.astype(np.uint8), o.astype(np.uint8), np.stack([d] * 3, axis=2)], axis=1)
            im = Image.fromarray(row).resize((int(row.shape[1] * sc), int(row.shape[0] * sc)))
            tiles.append(np.asarray(im))
        Image.fromarray(np.concatenate(tiles, axis=0)).save(os.path.join(work, 'sheet.png'))
        print('INFO  sheet: ' + os.path.join(work, 'sheet.png') + ' (rows: ' + ', '.join('%s %d' % (c, f) for c, f, _, _ in pick) + '; columns: stream | ours | |diff| x 8)')
    print('%s  lane P3c matrix rows: %d failing check(s), %.0f s' % ('PASS' if not rep.fails else 'FAIL', rep.fails, time.time() - t0))
    return 1 if rep.fails else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except RuntimeError as e:
        print('FAIL  ' + str(e))
        sys.exit(1)
