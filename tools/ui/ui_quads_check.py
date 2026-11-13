#!/usr/bin/env python3
r"""Lane 148 offline proof (0 game runs): the UI's sprites and flat rectangles (T_Sprite 0x82BC5688, T_Rect 0x82BC3EE8) are drawn with OUR quad and OUR 2D program
(docs\renderer\UI_QUADS_OURS.md). fview through the real replace seam, --ui-text-ours --ui-ours --ui-quads against the stream.

  py -I tools\ui\ui_quads_check.py [--root DIR] [--no-build] [--quick]

Every line PASS / FAIL, a missing input FAILS, exit 1 on any FAIL:
  1. picture: ours (quads on) vs the stream, max channel difference <= 1 and 0 pixels over 1, at frames of the menu, the editor (CAW), the Bryan entrance, the WM30
     entrance, the Brock entrance and Check B (frame 260 = the fade rectangle). The quads-off picture (lane 66 / 89 / 126) is byte-equal to the stream's, so the bar here is
     the same plus the 1-level slack of re-ordered float sums.
  2. census (fview --ops): every 2D sprite / rectangle draw at the two sites is OURS (fate ui_quads_ours, geom_ours 2 = our quad and our program), except the draws the
     check names as kept by reason (the hue-shifted sprite ySprite_hsl_fragment: a variant this reading does not take); mismatch 0, fallback 0.
  3. a scale-2 arm (render scale 2) and an 8-frame replay with the validation layer on, errors 0.
  4. plants (each moves the picture on a frame that has the draw kind, and the clean twin on that frame is within 1 level):
     wrong_uv_rect (sprite texture rectangle shrunk), no_blend (sprite alpha forced 1), fade_alpha_ignored (rectangle alpha forced 1), half_pixel (the placement's half unit dropped).
     R (a past bug of UI_OURS.md): --replace-fault unchecked_wrong_texture / unchecked_rb_swap, with the quads on, still move the picture; the checked twin keeps it within 1 level.
Pictures and logs go under the main checkout's out\s101\ui_quads\ ; nothing from the game is written elsewhere."""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
SPRITE, RECT = 0x82BC5688, 0x82BC3EE8
# kind -> (capture folder under the main checkout, frames)
CAPS = {
    'menu': ('out/s101/ui_ours/case_menu/capture', (5, 700, 2300)),   # (frame 300 draws no 2D sprite or rectangle)
    'editor': ('out/s101/ui_ours/case_editor/capture', (20, 120, 250)),
    'bryan': ('out/s101/ui_inmatch/case_bryan/capture', (90, 150)),
    'wm30': ('out/s101/ui_inmatch/case_wm30/capture', (127, 300)),
    'brock': ('out/s101/ui_inmatch/case_brock/capture', (100, 250)),
    'checkB': ('out/s101/lane92/case_20261111_215233_217_0_pc_run_exit/capture', (200, 260)),
}
KEPT_OK = {'ySprite_hsl_fragment'}   # the variants this reading does not take: kept by reason, counted (the hue-shifted sprite)


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout=900, env=None):
    e = dict(os.environ)
    if env:
        e.update(env)
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=e, errors='replace')
    return p.returncode, (p.stdout or '') + (p.stderr or '')


class Report:
    def __init__(self):
        self.fails = 0

    def line(self, ok, text):
        print(('PASS' if ok else 'FAIL') + '  ' + text, flush=True)
        self.fails += 0 if ok else 1
        return ok


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
    work = os.path.join(main_co, 'out', 's101', 'ui_quads')
    cache = os.path.join(main_co, 'out', 's101', 'ui_inmatch', 'idx')
    os.makedirs(work, exist_ok=True)
    os.makedirs(cache, exist_ok=True)
    if not os.path.isdir(os.path.join(game, 'pac', 'menu')):
        rep.line(False, 'input missing: the game files ' + game)
        return 1
    caps = {}
    for kind, (rel, _) in CAPS.items():
        cap = os.path.join(main_co, rel)
        if not os.path.isfile(os.path.join(cap, 'images.bin')):
            rep.line(False, 'input missing: the %s capture %s (extract the case zip there)' % (kind, cap))
            return 1
        caps[kind] = cap
    try:
        import json
        import numpy as np
        from PIL import Image
    except Exception as e:
        rep.line(False, 'numpy / Pillow are needed to compare pictures: %s' % e)
        return 1
    t0 = time.time()
    if not args.no_build:
        rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], 1800)
        if rc:
            print(text[-3000:])
            rep.line(False, 'fview build (exit %d)' % rc)
            return 1
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    fexe = _fe.find_or_exit(root=root, kind='dev')
    if not os.path.isfile(fexe):
        rep.line(False, 'no ' + fexe)
        return 1
    env = {'WWE2K14_UI_INDEX_CACHE': cache}
    cached = {}

    def fview(tag, kind, frame, extra, ops=False):
        key = (tag, kind, frame, ops)
        if key in cached:
            return cached[key]
        d = os.path.join(work, 'arm_%s_%s_%d' % (tag, kind, frame))
        cmd = [fexe, '--capture', caps[kind], '--frame', str(frame), '--root', root, '--judge-out', d] + extra
        ops_json = d + '_ops.json'
        for attempt in range(10):   # the GPU is shared: only the frame-ring refusal is retried
            rc, text = run(cmd, 1200, env)
            if 'Not enough graphics memory for the frame ring' in text:
                print('      (shared GPU: frame ring refused, attempt %d; waiting)' % (attempt + 1), flush=True)
                time.sleep(20)
                continue
            break
        png = os.path.join(d, 'picture.png')
        if rc or not os.path.isfile(png):
            print(text[-2500:])
            raise RuntimeError('fview arm %s %s frame %d failed (exit %d)' % (tag, kind, frame, rc))
        img = np.asarray(Image.open(png).convert('RGB'), dtype=np.int16)
        draws = None
        if ops:   # (a second process: --ops and --judge-out do not share a run)
            rc2, text2 = run([fexe, '--capture', caps[kind], '--frame', str(frame), '--root', root, '--ops', '--json', ops_json] + extra, 1200, env)
            if rc2 or not os.path.isfile(ops_json):
                print(text2[-2000:])
                raise RuntimeError('fview ops arm %s %s frame %d failed (exit %d)' % (tag, kind, frame, rc2))
            draws = [o for o in json.load(open(ops_json, encoding='utf-8'))['ops'] if o.get('kind') == 'draw']
        cached[key] = (img, text, draws)
        return cached[key]

    def diff(a, b):
        d = np.abs(a - b).max(axis=2)
        return int(d.max()), int((d > 1).sum()), int((d > 0).sum())

    census_re = re.compile(r'^\[rdr\]\[ui\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(not ready (\d+), unjoined (\d+), mismatch (\d+)', re.M)

    def census(text):
        m = census_re.search(text)
        return tuple(map(int, m.groups())) if m else None

    def quads_line(text):
        m = re.search(r'quads: decisions (\d+), sprites (\d+), rects (\d+); kept: variant (\d+), geometry (\d+), quad (\d+), constants (\d+), alpha-to-coverage (\d+), texture (\d+)', text)
        return tuple(map(int, m.groups())) if m else None

    def draws_2d(draws):
        return [o for o in draws if o['origin_op'] == 19 and o['call_site'] in (SPRITE, RECT)]

    ui = ['--ui-text-ours', '--ui-ours', '--ui-quads', '--game', game]
    ui1 = ['--ui-ours', '--ui-quads', '--game', game]   # (the plants name one hook: --replace-fault is parsed by every registered class)
    sel = {k: ((v[1][-1:] if k == 'checkB' else v[1][:1]) if args.quick else v[1]) for k, v in CAPS.items()}
    for kind, frames in sel.items():
        for f in frames:
            s, _, _ = fview('stream', kind, f, [])
            o, text, draws = fview('quads', kind, f, ui, ops=True)
            mx, over1, npx = diff(o, s)
            c = census(text)
            q = quads_line(text)
            d2 = draws_2d(draws)
            ours = [d for d in d2 if d['fate'] == 'ui_quads_ours' and d.get('geom_ours') == 2]
            kept = [d for d in d2 if d not in ours]
            bad_kept = [d for d in kept if d.get('ps_variant') not in KEPT_OK]
            ok = mx <= 1 and over1 == 0 and c is not None and c[1] == 0 and c[6] == 0 and len(ours) > 0 and not bad_kept and 'ERRORS' not in text and q is not None
            rep.line(ok, 'fview %s frame %d: quads vs stream max channel diff %d (%d pixels over 1, %d at 1); fallback %s, mismatch %s; 2D sprite / rect draws %d, ours %d (own quad + program), kept %d by variant (%s)' % (
                kind, f, mx, over1, npx - over1, c[1] if c else '?', c[6] if c else '?', len(d2), len(ours), len(kept), ','.join(sorted(set(d.get('ps_variant', '?') for d in kept))) or 'none'))
    # scale 2 (the render scale) on the menu: the quad's placement goes through the renderer's system block
    rplay = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    if not os.path.isfile(rplay):
        rep.line(False, 'no ' + rplay)
        return 1
    pics = {}
    for tag, extra in (('stream', []), ('quads', ['--ui-text-ours', '--ui-ours', '--ui-quads', '--game', game])):
        sd = os.path.join(work, 'rp_scale2_%s' % tag)
        os.makedirs(sd, exist_ok=True)
        for fn in os.listdir(sd):
            os.remove(os.path.join(sd, fn))
        rc, text = run([rplay, '--capture', caps['menu'], '--frames', '2301', '--no-vsync', '--shot-dir', sd, '--shots', '700,2300', '--scale', '2'] + extra, 1800, env)
        if rc:
            print(text[-2000:])
            raise RuntimeError('rplay scale 2 arm %s failed (exit %d)' % (tag, rc))
        pics[tag] = sd
    for f in (700, 2300):
        a = np.asarray(Image.open(os.path.join(pics['stream'], 'frame_%04d.png' % f)).convert('RGB'), dtype=np.int16)
        b = np.asarray(Image.open(os.path.join(pics['quads'], 'frame_%04d.png' % f)).convert('RGB'), dtype=np.int16)
        mx, over1, npx = diff(b, a)
        rep.line(mx <= 1 and over1 == 0 and a.shape == b.shape and a.shape[0] == 1440, 'rplay menu frame %d at --scale 2: quads vs stream max channel diff %d (%d pixels over 1), picture %dx%d' % (f, mx, over1, a.shape[1], a.shape[0]))
    for kind in ('menu', 'bryan', 'editor', 'checkB'):   # (review S3: the editor and Check B draw rects, so the flat program runs under validation; Bryan has none)
        rc, vt = run([fexe, '--capture', caps[kind], '--frames', '8', '--validation', '1', '--replay', '--root', root] + ui, 1200, env)
        rep.line(rc == 0 and 'validation errors 0' in vt and 'validation active: yes' in vt, 'fview %s: 8-frame replay with the validation layer on and the quads ours, errors 0 (exit %d)' % (kind, rc))
    # plants
    plant_frames = {'wrong_uv_rect': ('menu', 700), 'no_blend': ('menu', 700), 'half_pixel': ('menu', 700), 'fade_alpha_ignored': ('checkB', 260)}
    for plant, (kind, f) in plant_frames.items():
        s, _, _ = fview('stream', kind, f, [])
        clean, _, _ = fview('quads1', kind, f, ui1)
        planted, text, _ = fview('plant_' + plant, kind, f, ui1 + ['--replace-fault', plant])
        cm = diff(clean, s)
        pm = diff(planted, s)
        rep.line(cm[0] <= 1 and cm[1] == 0 and pm[2] > 100 and pm[0] > 1, 'fview %s frame %d plant %s: clean twin within 1 level (max %d), the planted picture moves %d pixels (max %d levels; bar > 100 pixels)' % (kind, f, plant, cm[0], pm[2], pm[0]))
    # review S3: plants that make a per-draw CHECK refuse (or that only the picture sees)
    def ql(text):   # the hook's final "quads:" line, with the kept-by-reason counts
        m = re.search(r'\[fview\] \[rdr\]\[ui\].*quads: decisions (\d+), sprites (\d+), rects (\d+); kept: variant (\d+), geometry (\d+), quad (\d+), constants (\d+)', text)
        return dict(zip(('decisions', 'sprites', 'rects', 'variant', 'geometry', 'quad', 'constants'), map(int, m.groups()))) if m else None
    for kind, f in (('menu', 700), ('checkB', 260)):
        s, _, _ = fview('stream', kind, f, [])
        for plant, field in (('proj_flag_forced', 'constants'), ('indices_permuted', 'quad')):
            pic, text, draws = fview('refuse_%s' % plant, kind, f, ui1 + ['--replace-fault', plant], ops=True)
            q = ql(text)
            mx, over1, _ = diff(pic, s)
            d2 = draws_2d(draws)
            own = [d for d in d2 if d['fate'] == 'ui_quads_ours']
            rects_stream = [d for d in d2 if d['call_site'] == RECT and d['fate'] == 'stream']
            nrect = len([d for d in d2 if d['call_site'] == RECT])
            ok = q is not None and q[field] > 0 and q['sprites'] == 0 and q['rects'] == 0 and mx <= 1 and over1 == 0 and not own and len(rects_stream) == nrect
            rep.line(ok, 'fview %s frame %d plant %s: every 2D draw refused as "%s" (%s), none ours, rect draws stay the stream draw (%d of %d), picture within 1 level (max %d)' % (
                kind, f, plant, field, q[field] if q else '?', len(rects_stream), nrect, mx))
    s, _, _ = fview('stream', 'menu', 700, [])
    chk, ctext, _ = fview('stride_chk', 'menu', 700, ui1 + ['--replace-fault', 'stride_plus_one'])
    unc, utext, _ = fview('stride_unc', 'menu', 700, ui1 + ['--replace-fault', 'unchecked_stride_plus_one'])
    q = ql(ctext)
    cm, um = diff(chk, s), diff(unc, s)
    rep.line(q is not None and q['geometry'] > 0 and q['sprites'] == 0 and cm[0] <= 1 and cm[1] == 0 and um[2] > 100, 'fview menu frame 700 plant stride_plus_one: checked, every draw refused as "geometry" (%s) and the picture within 1 level (max %d); unchecked_ twin moves %d pixels (max %d)' % (q['geometry'] if q else '?', cm[0], um[2], um[0]))
    for plant, kind, f in (('color_from_c2', 'menu', 700), ('alpha_test_ignored', 'menu', 700), ('alpha_test_ignored', 'checkB', 260)):
        s, _, _ = fview('stream', kind, f, [])
        pic, _, _ = fview('plant_' + plant, kind, f, ui1 + ['--replace-fault', plant])
        pm = diff(pic, s)
        if plant == 'color_from_c2':
            rep.line(pm[2] > 100, 'fview %s frame %d plant %s (the colour from the projection-flag row): caught by the PICTURE check, moves %d pixels (max %d); no per-draw check sees a wrong-but-finite colour' % (kind, f, plant, pm[2], pm[0]))
        else:   # informational: whether any captured 2D draw has the alpha test on (a plant that moves nothing = the branch is unproven there)
            print('INFO  fview %s frame %d plant %s: moves %d pixels (max %d)%s' % (kind, f, plant, pm[2], pm[0], '' if pm[2] else ' - no 2D draw of this frame has the alpha test on: the alpha-test branch is UNPROVEN here'), flush=True)
    for plant in ('wrong_texture', 'rb_swap'):   # R: past bugs of UI_OURS.md (lane 66 / 126), found again through the own path
        kind, f = 'menu', 700
        s, _, _ = fview('stream', kind, f, [])
        raw, _, _ = fview('raw_' + plant, kind, f, ui1 + ['--replace-fault', 'unchecked_' + plant])
        chk, ctext, _ = fview('chk_' + plant, kind, f, ui1 + ['--replace-fault', plant])
        rm, cm = diff(raw, s), diff(chk, s)
        m = re.search(r'^\[fview\] \[rdr\]\[ui\] replaced (\d+), kept (\d+) \(unjoined (\d+), mismatch (\d+)', ctext, re.M)   # the hook's own check counts (the join refuses the planted texture)
        refused = int(m.group(2)) if m else -1
        rep.line(rm[2] > 100 and cm[0] <= 1 and cm[1] == 0 and refused > 0, 'fview %s frame %d past bug %s through the own path: unchecked picture moves %d pixels (max %d); checked: the join refuses %d draws (kept), picture within 1 level (max %d)' % (
            kind, f, plant, rm[2], rm[0], refused, cm[0]))
    print('%s  lane 148 ui_quads: %d failing check(s), %.0f s' % ('PASS' if not rep.fails else 'FAIL', rep.fails, time.time() - t0))
    return 1 if rep.fails else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except RuntimeError as e:
        print('FAIL  ' + str(e))
        sys.exit(1)
