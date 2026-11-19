#!/usr/bin/env python3
r"""Lane I3 offline proof (Session 103, landing 2; 0 game runs; docs\renderer\REMNANT_MAP.md section 12): the 2D layer's GLYPHS (the text) drawn from OUR rows.

  py -I tools\scene\i3_check.py [--root DIR] [--no-build] [--quick]

Two captures of the main checkout (both Brock's entrance, both carry real text):
  L  out\captures\brock_losswin (220 frames): frames 140-210 hold the lower-third card "BROCK LESNAR / @BrockLesnar", 24 glyph draws a frame (the name-card font: no fontHD font agrees with it, ui_text keeps all of them)
  D  out\post_cover_cache\20261111_174957_s100_owner_s89_brock\capture (1063 frames): frames 930-1062 hold "Press START button" (18 glyph draws, the fontHD font)
fview replays the real frame through the replace seam in two arms and compares the pictures:
  stream   the game's own 2D pass (the reference, no hook)
  rows     --ui-ours --ui-quads --movie-ours --quads-2d-rows: every glyph (and sprite, rect, movie quad) the hooks took over is a Quad2D ROW built from the stream draw (the truth: four vertices with uv, c0 / c1 / c4 / c28, the
           sheet KEY, the primitive word), written and parsed through record section 58, and drawn by the drawer's pure function BlocksOf with the sheet chosen by KEY from the key table (UiOurs' join) instead of the draw's own slot
Bars (each line PASS / FAIL; a missing input FAILS, never skips):
  picture      rows vs stream: max channel difference <= 1 level and 0 pixels over 1 (owned_check's tolerance for a 2D pass) at every frame tried
  census       --ops: every glyph draw of the shown frame (call site 0x82BC3784) is an own quad (fate ui_quads_ours, geom_ours 2); over the WHOLE capture-L replay: glyph rows == the glyph draws the hook took over == drawn,
               refused 0, sheets from the key table + the stream's own == rows, state mismatch (blend, alpha test, key table vs the join) 0, section parser refused 0; every glyph draw is a strip (the primitive the row carries)
  validation   a 220-frame replay of L with the Vulkan validation layer on, rows on: errors 0
  plants       each planted twin on the same frames must be CAUGHT, the clean twin being within 1 level on them:
                 glyph_uv_shift      the picture moves (the neighbouring glyph is drawn)
                 glyph_wrong_sheet   the picture moves (a sheet key answered with another key's picture; capture L only: D's glyph draws carry no key)
                 glyph_lost_colour   the picture moves (the outline / shadow passes and the tints are gone: every glyph opaque white)
                 glyph_drop          the picture moves (every fifth glyph is not drawn) and the dropped counter rises
INFO lines: the stream's glyph blend class, alpha tests and primitive topology (REMNANT_MAP U6 for text).
Pictures and logs go under the main checkout's out\s103_i3\ ; nothing from the game is written elsewhere."""
import argparse
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
GLYPH = 0x82BC3784
CAPS = {
    'L': ('out/captures/brock_losswin', (140, 180, 210), 220),
    'D': ('out/post_cover_cache/20261111_174957_s100_owner_s89_brock/capture', (1000, 1062), 1063),
}
QUICK = {'L': (180,), 'D': (1000,)}
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
GLY_RE = re.compile(r'\[rdr\]\[2d\] glyphs: taken (\d+), rows (\d+), drawn (\d+), refused-by-prim (\d+), pictures: from the key table (\d+), the stream\'s own (\d+); state: pic (\d+); plants applied: uv (\d+) colour (\d+) dropped (\d+) sheet (\d+); shadow: rows (\d+) mismatch (\d+) incomplete (\d+); '
                    r'stream blend: off (\d+) alpha (\d+) add (\d+) other (\d+); topology \(vulkan 3 list / 4 strip / 5 fan\): 3 (\d+) 4 (\d+) 5 (\d+) other (\d+); alpha tests \(function@reference: draws\):([^\n]*)')
GKEYS = ['taken', 'rows', 'drawn', 'prim', 'pic_key', 'pic_stream', 'pic_bad', 'uv', 'colour', 'dropped', 'sheet', 'sh_rows', 'sh_mis', 'sh_inc', 'b_off', 'b_alpha', 'b_add', 'b_other', 't3', 't4', 't5', 'tother', 'alpha_tests']


def parse(text):
    r = ROWS_RE.findall(text)
    g = GLY_RE.findall(text)
    rows = dict(zip(['rect', 'movie', 'drawn', 'refused', 'section_refused', 'blend_mismatch', 'alpha_mismatch'], [int(x) for x in r[-1]])) if r else None
    gl = dict(zip(GKEYS, [int(x) if i < 22 else x.strip() for i, x in enumerate(g[-1])])) if g else None
    return rows, gl


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
    work = os.path.join(main_co, 'out', 's103_i3')
    cache = os.path.join(main_co, 'out', 's103_i1', 'idx')   # (the index cache lane I1 made; a missing one is rebuilt by fview)
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
    for cap, f in plan:
        s, _, sd = fview(cap, 'stream', f, [], ops=True)
        o, text, od = fview(cap, 'rows', f, rows_args, ops=True)
        mx, over1, npx = diff(o, s)
        want = [d for d in sd if d['origin_op'] == 19 and d['call_site'] == GLYPH]
        own = [d for d in od if d['origin_op'] == 19 and d['call_site'] == GLYPH]
        own_ok = [d for d in own if d['fate'] == 'ui_quads_ours' and d.get('geom_ours') == 2]
        rep.line(mx <= PIX and over1 == 0 and 'ERRORS' not in text, '%s frame %d: rows vs the stream\'s own 2D pass, max channel diff %d (%d pixels over %d, %d at 1), %d glyph draws' % (cap, f, mx, over1, PIX, npx - over1, len(want)))
        rep.line(bool(want) and len(own_ok) == len(want), '%s frame %d census (--ops): every glyph draw of the frame (%d) is an own quad (fate ui_quads_ours, geom_ours 2): %d' % (cap, f, len(want), len(own_ok)))

    # ---- the whole replay of L: counts, state cross-checks -------------------------------------------------------------------------------------------------------------------
    rc, text = run([fexe, '--capture', caps['L'], '--frames', str(CAPS['L'][2]), '--replay', '--root', root] + rows_args, 1800, env)
    rows, gl = parse(text)
    qm = re.search(r'quads: decisions \d+, sprites \d+, rects \d+;.*?glyphs (\d+)', text)
    hook_glyphs = int(qm.group(1)) if qm else -1   # (the UI quads hook's own count of the glyph draws it took over)
    ok = (rc == 0 and gl is not None and gl['rows'] == hook_glyphs and rows is not None and gl is not None and gl['rows'] == gl['taken'] and gl['drawn'] == gl['rows'] and gl['rows'] > 0 and gl['prim'] == 0 and gl['pic_key'] + gl['pic_stream'] == gl['rows'] and gl['pic_bad'] == 0 and
          rows['refused'] == 0 and rows['section_refused'] == 0 and rows['blend_mismatch'] == 0 and rows['alpha_mismatch'] == 0 and gl['t4'] == gl['taken'] and gl['sh_rows'] == gl['taken'] and gl['sh_mis'] == 0 and gl['sh_inc'] == 0)
    rep.line(ok, 'whole replay of L (%d frames) census: glyph rows %s == taken by the hook %s (the hook count %d) == drawn %s, refused-by-primitive %s, section-refused %s, state mismatch blend %s alpha %s picture %s; sheets from the key table %s, the stream\'s own %s; every glyph draw a strip: %s of %s; rows built through the TAP path (register shadow + op-19 core over the stream numbers) %s, differing from the stream numbers %s, incomplete %s' % (
        CAPS['L'][2], gl and gl['rows'], gl and gl['taken'], hook_glyphs, gl and gl['drawn'], gl and gl['prim'], rows and rows['section_refused'], rows and rows['blend_mismatch'], rows and rows['alpha_mismatch'], gl and gl['pic_bad'], gl and gl['pic_key'],
        gl and gl['pic_stream'], gl and gl['t4'], gl and gl['taken'], gl and gl['sh_rows'], gl and gl['sh_mis'], gl and gl['sh_inc']))
    if gl:
        print('INFO  glyph stream blend (U6 for text): off %d alpha %d add %d other %d; alpha tests (function@reference: draws) %s; topology 3 %d 4 %d 5 %d other %d' % (
            gl['b_off'], gl['b_alpha'], gl['b_add'], gl['b_other'], gl['alpha_tests'], gl['t3'], gl['t4'], gl['t5'], gl['tother']), flush=True)

    rc, vt = run([fexe, '--capture', caps['L'], '--frames', str(CAPS['L'][2]), '--validation', '1', '--replay', '--root', root] + rows_args, 1800, env)
    rep.line(rc == 0 and 'validation errors 0' in vt and 'validation active: yes' in vt, '%d-frame replay of L (the lower-third frames in it) with the validation layer on and rows on, errors 0 (exit %d)' % (CAPS['L'][2], rc))

    # ---- plants --------------------------------------------------------------------------------------------------------------------------------------------------------------------
    clean_ok = True
    for cap, f in plan:
        s, _, _ = fview(cap, 'stream', f, [])
        o, _, _ = fview(cap, 'rows', f, rows_args)
        mx, over1, _ = diff(o, s)
        clean_ok = clean_ok and mx <= PIX and over1 == 0
    rep.line(clean_ok, 'plants.clean_twin: the rows arm is within %d level of the stream at every plant frame (%s)' % (PIX, ', '.join('%s%d' % p for p in plan)))
    for name in ('glyph_uv_shift', 'glyph_wrong_sheet', 'glyph_lost_colour', 'glyph_drop'):
        per = []
        for cap, f in ([p for p in plan if p[0] == 'L'] if name == 'glyph_wrong_sheet' else plan):   # (D's glyph draws carry no sheet key in that older capture: a key answer cannot be planted there)
            s, _, _ = fview(cap, 'stream', f, [])
            p, text, _ = fview(cap, 'plant_' + name, f, rows_args + ['--quads-2d-fault', name])
            mx, over1, _ = diff(p, s)
            _, g = parse(text)
            per.append((cap + str(f), over1, mx, g[{'glyph_uv_shift': 'uv', 'glyph_wrong_sheet': 'sheet', 'glyph_lost_colour': 'colour', 'glyph_drop': 'dropped'}[name]] if g else -1))
        moved = [t for t in per if t[1] >= FAULT_PX]
        rep.line(len(moved) == len(per) and all(t[3] > 0 for t in per), 'plant %s: the picture moves (>= %d pixels over %d level) at %d of %d frames and the plant was applied: %s' % (name, FAULT_PX, PIX, len(moved), len(per), ', '.join('%s %d px (max %d, applied %d)' % t for t in per)))
    # review twin: register uploads stamped with the previous frame -> every glyph row is incomplete: refused (counted), the stream's draw stays, the picture is the stream's
    per = []
    for cap, f in plan:
        s, _, _ = fview(cap, 'stream', f, [])
        p, text, _ = fview(cap, 'plant_glyph_stale_regs', f, rows_args + ['--quads-2d-fault', 'glyph_stale_regs'])
        mx, over1, _ = diff(p, s)
        _, g = parse(text)
        per.append((cap + str(f), over1, mx, g['sh_inc'] if g else -1, g['drawn'] if g else -1, g['taken'] if g else -1))
    rep.line(all(t[3] == t[5] and t[3] > 0 and t[4] == 0 and t[1] == 0 for t in per), 'plant glyph_stale_regs: every glyph row is incomplete (stale register stamp), none is drawn (the stream keeps them), the picture is unchanged: %s' % ', '.join('%s incomplete %d of %d drawn %d (pic max %d)' % (t[0], t[3], t[5], t[4], t[2]) for t in per))
    print('%s  lane I3 glyph rows: %d failing check(s), %.0f s' % ('PASS' if not rep.fails else 'FAIL', rep.fails, time.time() - t0))
    return 1 if rep.fails else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except RuntimeError as e:
        print('FAIL  ' + str(e))
        sys.exit(1)
