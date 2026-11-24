#!/usr/bin/env python3
r"""The rig-vs-live parity check (Session 103, lane T3; docs\renderer\S103_FABLE_REVIEW_2.md Q4).

  py -I tools\scene\rig_parity.py --case out\s103\run28_case.zip --live out\s103\run28_live [--frames 15000] [--variant live]
        [--out DIR] [--expect-fail TOKEN] [--ratio-tol 0.10] [--diff-tol 12] [--curve-file F]

WHY: five lanes chased a 1.4x "too bright stands" that existed only between the offline rig (frame_rig.exe) and the game's picture; the LIVE mirror frame of the same
record (live_scene_ours_f<N>.png, drawn by LiveSceneDraw::DrawToPng) already matched the game. A rig proof is a live proof only while rig(record j) == live_scene_ours_j.
Before opening a lane on a picture gap, show it on the live pair, then run this.

WHAT: for a kept mirror run's case zip (capture\scene\frame_*.rsc) and its live pictures, per live picture N: find the record whose header guest_frame is N, draw it in the
rig at the live picture's size, put the live path's settings on it, and compare with live_scene_ours_fN.png per region. Settings the live path has and the bare rig lacks
(each one switched in the rig, each its own variant):
  curve  the game's display table (live.cpp applies shown_curve to BOTH pictures it writes; frame_rig writes linear). The table is not in a case: the S100 five-point curve
         (present_fetch_plants.h; 16->6 32->16 64->48 128->115 192->185) is used, or --curve-file F (1024 bytes, 256 x RGBA8).
  post   the record's post row (DoF / fog ...): frame_rig does it by default now; WWE_RDR_NO_SCENE_POST=1 = the old rig.
  scale  the wrestlers' body scale (live reads it from the skin latch; a case has none: the rig's --body-scale-one fixture tells 1.0 for every engine object [U that 1.0 is
         right for all; the live log says "body_scale latch==stream" and the picture shows it].
  hooks  the maps (wrestler shadow / rim / plane) + particle hooks and the game's see-through order (AttachFrameHooks): --no-hooks leaves them off.
NOT switchable in the rig (listed in the report, not tested): the frame clock (the header carries time_s 0), a tier / quality setting (the rig has none), the live-only GPU
state between frames (previous-frame screens).

REGIONS: from the rig's own per-family panels (the family drawn alone; pixels that differ from the front-end clear colour), painted by a front-to-back priority
(wrestlers, ring, props, screens, crowd, arena), each eroded so the post blur at an edge does not count. Per region: mean luminance ratio rig / live, mean abs
difference (0-255, max channel), p99 and max of the difference. A region FAILS when |ratio - 1| > --ratio-tol or the mean abs difference > --diff-tol (regions under
0.2 % of the picture are reported, not judged). Exit 1 on any FAIL of the judged --variant (default live = the full live settings).
--expect-fail TOKEN: the planted-twin form: exit 0 only when the judged variant FAILED and its output holds TOKEN ("PARITY crowd" = the stands).
"""
import argparse
import os
import re
import subprocess
import sys
import zipfile

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CLEAR = (31, 36, 46)   # the front-end's clear colour (frame_rig.py CLEAR)
GAME_CLEAR = (0, 0, 0)   # S103 CLRB: the game's clear colour, which a live record clears to when the switch is on (frame_rig.py --game-clear): neither background counts as drawn


def drawn(img):
    """pixels that are neither clear colour (the slate or the game's black)."""
    return np.any(img != np.array(CLEAR, np.uint8), axis=2) & np.any(img != np.array(GAME_CLEAR, np.uint8), axis=2)
FAMILIES = ['arena', 'ring', 'props', 'wrestlers', 'crowd', 'screens', 'flashes', 'particles', 'other']
PRIORITY = ['wrestlers', 'ring', 'props', 'screens', 'flashes', 'particles', 'crowd', 'arena']   # front to back
JUDGED = ['whole', 'crowd', 'arena', 'ring', 'wrestlers']   # the regions a verdict is made on; props / screens are reported
S100_XS = [0, 16, 32, 64, 128, 192, 255]
S100_YS = [0, 6, 16, 48, 115, 185, 255]
MIN_FRAC = 0.002

# a variant = which live settings the rig has: curve, post, scale (body scale), hooks
VARIANTS = {
    'live':     dict(curve=True,  post=True,  scale=True,  hooks=True),
    'no_curve': dict(curve=False, post=True,  scale=True,  hooks=True),
    'no_post':  dict(curve=True,  post=False, scale=True,  hooks=True),
    'no_scale': dict(curve=True,  post=True,  scale=False, hooks=True),
    'no_hooks': dict(curve=True,  post=True,  scale=True,  hooks=False),
    'bare':     dict(curve=False, post=False, scale=False, hooks=False),
}


def main_checkout():
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return env
    common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
    return os.path.dirname(common) if common else ROOT


def find_exe(name, main):
    for base in (ROOT, main):
        p = os.path.join(base, 'out', 'tools', name)
        if os.path.exists(p):
            return p
    return None


def run(cmd, env=None, timeout=900):
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=timeout, env=env)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def curve_table(path):
    """256 x 3 lookup (uint8). Default: the S100 five-point curve (present_fetch_plants.h); a file = 1024 bytes RGBA8."""
    if path:
        b = np.fromfile(path, dtype=np.uint8)
        if b.size != 1024:
            raise SystemExit('FAIL  --curve-file %s: %d bytes, want 1024' % (path, b.size))
        return b.reshape(256, 4)[:, :3]
    v = np.floor(np.interp(np.arange(256), S100_XS, S100_YS) + 0.5).astype(np.uint8)   # (the plants' own rounding)
    return np.stack([v, v, v], axis=1)


def guest_frame_of(rsc_dump, path):
    rc, text = run([rsc_dump, path])
    m = re.search(r'^\s*version \d+\s+source \d+\s+sections \d+\s+guest_frame (\d+)', text, re.M)
    return int(m.group(1)) if m else None


def find_record(zf, names, rsc_dump, work, guest):
    """The scene record whose header guest_frame == guest (records are written in guest order: a binary search, one extraction per probe). -> path or None."""
    cache = {}

    def gf(i):
        if i not in cache:
            p = zf.extract(names[i], work)
            cache[i] = (guest_frame_of(rsc_dump, p), p)
        return cache[i]
    lo, hi = 0, len(names) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        g, p = gf(mid)
        if g is None:
            return None
        if g == guest:
            return p
        if g < guest:
            lo = mid + 1
        else:
            hi = mid - 1
    return None


def erode(mask, k):
    """k-pixel box erosion (numpy only)."""
    if k <= 0:
        return mask
    out = mask.copy()
    for ax in (0, 1):
        acc = out.copy()
        for s in range(1, k + 1):
            a = np.zeros_like(out)
            b = np.zeros_like(out)
            if ax == 0:
                a[s:] = out[:-s]
                b[:-s] = out[s:]
            else:
                a[:, s:] = out[:, :-s]
                b[:, :-s] = out[:, s:]
            acc &= a & b
        out = acc
    return out


def load(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.uint8)


def lum(a):
    a = a.astype(np.float32)
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]


def region_masks(prefix, shape, erode_px):
    """Visible regions from the per-family panels (priority painted). -> {name: bool mask}, and the fractions."""
    h, w = shape[:2]
    taken = np.zeros((h, w), bool)
    masks = {}
    for fam in PRIORITY:
        p = '%s_%s.png' % (prefix, fam)
        if not os.path.exists(p):
            masks[fam] = np.zeros((h, w), bool)
            continue
        img = load(p)
        cov = drawn(img)
        m = cov & ~taken
        taken |= cov
        masks[fam] = erode(m, erode_px)
    masks['whole'] = erode(np.ones((h, w), bool), erode_px)
    return masks


def block_mean(a, k):
    h, w = a.shape[:2]
    h2, w2 = h // k * k, w // k * k
    return a[:h2, :w2].reshape(h2 // k, k, w2 // k, k, -1).mean(axis=(1, 3))


def compare(rig_rgb, live_rgb, masks):
    d = np.abs(rig_rgb.astype(np.int16) - live_rgb.astype(np.int16)).max(axis=2).astype(np.float32)
    # the judged difference is the BLOCK difference (16 x 16 mean colour): a one-pixel registration shift of a high-frequency region (a wrestler's skin, a rope edge) is not a parity fault;
    # the per-pixel mean / p99 / max are reported beside it.
    k = 16
    bd = np.abs(block_mean(rig_rgb.astype(np.float32), k) - block_mean(live_rgb.astype(np.float32), k)).max(axis=2)
    bd_full = np.kron(bd, np.ones((k, k), np.float32))
    h, w = d.shape
    bd_full = np.pad(bd_full, ((0, h - bd_full.shape[0]), (0, w - bd_full.shape[1])), mode='edge')
    lr, ll = lum(rig_rgb), lum(live_rgb)
    out = {}
    for name, m in masks.items():
        n = int(m.sum())
        frac = n / float(m.size)
        if n == 0:
            out[name] = dict(n=0, frac=0.0)
            continue
        dm = d[m]
        out[name] = dict(n=n, frac=frac, ratio=float(lr[m].mean() / max(1e-6, ll[m].mean())), mad=float(bd_full[m].mean()), pmad=float(dm.mean()), p99=float(np.percentile(dm, 99)), maxd=float(dm.max()),
                         rig=float(lr[m].mean()), live=float(ll[m].mean()))
    return out, d


# --wrestlers-wide: a wider wrestlers bar for a run whose live pictures were drawn by an OLDER build than the rig (run 28: its mirror frame had flat wrestlers, no key light / maps live: the
# rig, drawn by the current code, is ~7 % brighter with a block difference of ~19). Run 29 (the build with the shading) passes the tight bar (0.987, block 3.8): a build difference, not a rig fault.
WRESTLER_TOL = (0.12, 25.0)


WIDE = False   # --wrestlers-wide


def tol_of(name, ratio_tol, diff_tol):
    return WRESTLER_TOL if (WIDE and name == 'wrestlers') else (ratio_tol, diff_tol)


def verdict(stats, ratio_tol0, diff_tol0):
    """-> list of (region, ok, line)"""
    res = []
    for name in JUDGED:
        ratio_tol, diff_tol = tol_of(name, ratio_tol0, diff_tol0)
        s = stats.get(name)
        if not s or s['n'] == 0 or s['frac'] < MIN_FRAC:
            frac = s['frac'] if s else 0.0
            # a region the live frame has and the rig lacks entirely is a fault of its own: the live picture is not judged here (no mask), so the whole-picture number carries it
            res.append((name, True, 'NOTE  PARITY %s: %.2f %% of the picture (< %.1f %%): reported, not judged' % (name, 100 * frac, 100 * MIN_FRAC)))
            continue
        ok = abs(s['ratio'] - 1.0) <= ratio_tol and s['mad'] <= diff_tol
        res.append((name, ok, '%s  PARITY %s: ratio rig/live %.3f (tol %.2f), block diff %.1f (tol %.0f), per-pixel mean %.1f p99 %.0f max %.0f, %.1f %% of the picture, lum rig %.1f live %.1f'
                    % ('PASS' if ok else 'FAIL', name, s['ratio'], ratio_tol, s['mad'], diff_tol, s['pmad'], s['p99'], s['maxd'], 100 * s['frac'], s['rig'], s['live'])))
    return res


def draw_variant(rig, rec, game, size, prefix, v, per_family, env0, scales=None):
    cmd = [rig, '--record', rec, '--game', game, '--size', '%dx%d' % size, '--prefix', prefix, '--see-order', 'game']
    if v['scale'] and not scales:
        cmd.append('--body-scale-one')
    if not v['hooks']:
        cmd.append('--no-hooks')
    if per_family:
        cmd.append('--per-family')
    env = dict(env0)
    env.pop('WWE_RDR_NO_SCENE_POST', None)
    env.pop('WWE_RIG_BODY_SCALES', None)
    if v['scale'] and scales:
        env['WWE_RIG_BODY_SCALES'] = scales   # record object ids: 'actor:slot:0=1.0105,...' (frame_rig.cpp)
    if not v['post']:
        env['WWE_RDR_NO_SCENE_POST'] = '1'
    rc, text = run(cmd, env=env)
    png = prefix + '_whole.png'
    if rc != 0 or not os.path.exists(png):
        return None, text[-600:]
    return png, text


def fit_scales(rig, rec, game, size, prefix, live, curve, env0, slots, lines):
    """--fit-scales: the body scale is the one live-only input a case cannot carry (live reads the skin latch). Coordinate descent over the told scale of each visible wrestler slot
    (coarse 0.02 grid on 0.88..1.06, then +-0.01 in 0.0025 steps) minimising the 16 x 16 block difference to the live picture inside the wrestlers' window. -> the told string.
    A fitted scale is a fit, not a measurement: [U] it equals the game's latch value; the residual says whether scale is all that differs."""
    cur = {sl: 1.0 for sl in slots}

    def told(c):
        return ','.join('actor:slot:%d=%.5f' % (sl, v) for sl, v in sorted(c.items()))
    png, text = draw_variant(rig, rec, game, size, prefix + '_fit0', VARIANTS['live'], True, env0, scales=told(cur))
    if png is None:
        return told(cur)
    wm = drawn(load(prefix + '_fit0_wrestlers.png'))
    ys, xs = np.nonzero(wm)
    if ys.size == 0:
        return told(cur)
    pad = 60
    win = (slice(max(0, ys.min() - pad), ys.max() + pad), slice(max(0, xs.min() - pad), xs.max() + pad))
    memo = {}

    def score(c):
        key = told(c)
        if key not in memo:
            pg, _ = draw_variant(rig, rec, game, size, prefix + '_fit', VARIANTS['live'], False, env0, scales=key)
            if pg is None:
                memo[key] = 1e9
            else:
                r = curve[load(pg), np.arange(3)[None, None, :]]
                memo[key] = float(np.abs(block_mean(r[win].astype(np.float32), 16) - block_mean(live[win].astype(np.float32), 16)).max(axis=2).mean())
        return memo[key]
    best = score(cur)
    for lo, hi, st in ((0.88, 1.06, 0.02), (None, None, None)):
        for sl in slots:
            if lo is None:   # refine around the current value
                grid = np.arange(cur[sl] - 0.01, cur[sl] + 0.0101, 0.0025)
            else:
                grid = np.arange(lo, hi + 1e-9, st)
            for v in grid:
                c = dict(cur)
                c[sl] = float(v)
                sc = score(c)
                if sc < best - 1e-6:
                    best, cur = sc, c
    lines.append('FIT   body scales (told, fitted to the live picture; [U] = the game latch): %s, wrestlers window block diff %.2f (all 1.0: %.2f)' % (told(cur), best, score({k: 1.0 for k in slots})))
    return told(cur)


def sheet(path, live, rig_c, d, title, stats):
    w = 640
    h = int(live.shape[0] * w / live.shape[1])
    tiles = [Image.fromarray(live).resize((w, h)), Image.fromarray(rig_c).resize((w, h))]
    heat = np.clip(d / 64.0, 0, 1)
    hv = np.stack([np.clip(heat * 2, 0, 1), np.clip(heat * 2 - 0.5, 0, 1), np.clip(1 - heat * 2, 0, 1) * 0.35], axis=2)
    tiles.append(Image.fromarray((hv * 255).astype(np.uint8)).resize((w, h)))
    im = Image.new('RGB', (3 * w, h + 44), (12, 12, 14))
    for i, t in enumerate(tiles):
        im.paste(t, (i * w, 44))
    dr = ImageDraw.Draw(im)
    dr.text((4, 3), title, fill=(255, 255, 255))
    dr.text((4, 16), 'LIVE live_scene_ours | RIG with this variant\'s settings | |difference| (64 = full heat)', fill=(200, 200, 120))
    dr.text((4, 29), '  '.join('%s %.2f' % (k, stats[k]['ratio']) for k in JUDGED if stats.get(k, {}).get('n')), fill=(160, 220, 160))
    im.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--case', required=True, help='a kept mirror run\'s case zip (capture\\scene\\frame_*.rsc)')
    ap.add_argument('--live', required=True, help='the run\'s picture folder (live_scene_ours_fN.png)')
    ap.add_argument('--frames', default=None, help='guest frames (default: every live picture whose record is in the case)')
    ap.add_argument('--variant', default='live', choices=sorted(VARIANTS), help='the settings judged (default live = the full live settings)')
    ap.add_argument('--out', default=None)
    ap.add_argument('--ratio-tol', type=float, default=0.10)
    ap.add_argument('--diff-tol', type=float, default=12.0, help='the 16 x 16 block mean colour difference (0-255, max channel)')
    ap.add_argument('--erode', type=int, default=8, help='pixels eroded from every region mask (at 2560 wide; scaled with the width)')
    ap.add_argument('--wrestlers-wide', action='store_true', help='the wrestlers on the wide bar (ratio 0.12, block diff 25): for a run whose live pictures predate the wrestler lighting of the rig (run 28)')
    ap.add_argument('--curve-file', default=None)
    ap.add_argument('--fit-scales', default=None, metavar='SLOTS', help='fit the told body scale of these record slots (e.g. 0,1) to the live picture first (about 40 rig draws a frame)')
    ap.add_argument('--scales', default=None, metavar='ID=S,..', help='TOLD body scales by record object id (replaces the all-1.0 fixture), e.g. actor:slot:0=1.0105')
    ap.add_argument('--expect-fail', default=None, metavar='TOKEN')
    ap.add_argument('--quick', action='store_true', help='only the judged variant and live (no explanation table)')
    a = ap.parse_args()
    global WIDE
    WIDE = a.wrestlers_wide

    main_root = main_checkout()
    game = os.path.join(main_root, 'game_data', 'extracted')
    rig = find_exe('frame_rig.exe', main_root)
    rsc_dump = find_exe('rsc_dump.exe', main_root)
    case = os.path.abspath(a.case if os.path.isabs(a.case) or os.path.exists(a.case) else os.path.join(main_root, a.case))
    live_dir = os.path.abspath(a.live if os.path.exists(a.live) else os.path.join(main_root, a.live))
    for need, nm in ((rig, 'out\\tools\\frame_rig.exe (tools\\scene\\build_rig.ps1)'), (rsc_dump, 'out\\tools\\rsc_dump.exe'), (game, 'game_data\\extracted'), (case, a.case), (live_dir, a.live)):
        if not need or not os.path.exists(need):
            print('FAIL  cannot read its input: ' + nm)
            return 1
    tag = os.path.splitext(os.path.basename(case))[0]
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import run_dirs  # S103 PSO: a private output folder per run (never a fixed path under the main checkout)
    out = run_dirs.run_out('T3_' + tag, a.out)
    os.makedirs(out, exist_ok=True)
    curve = curve_table(a.curve_file)
    zf = zipfile.ZipFile(case)
    names = sorted(n for n in zf.namelist() if re.search(r'capture/scene/frame_\d+\.rsc$', n))
    if not names:
        print('FAIL  %s holds no capture/scene records' % case)
        return 1
    live_frames = sorted(int(m.group(1)) for n in os.listdir(live_dir) for m in [re.fullmatch(r'live_scene_ours_f(\d+)\.png', n)] if m)
    if a.frames:
        live_frames = [f for f in live_frames if f in {int(x) for x in a.frames.split(',')}]
    work = os.path.join(out, 'work')
    os.makedirs(work, exist_ok=True)

    lines, judged_fail, compared, skipped = [], [], 0, []
    explain = {}   # setting -> set of regions it explains
    env0 = dict(os.environ)
    for g in live_frames:
        rec = find_record(zf, names, rsc_dump, work, g)
        if rec is None:
            skipped.append(g)
            continue
        live = load(os.path.join(live_dir, 'live_scene_ours_f%d.png' % g))
        H, W = live.shape[:2]
        er = max(1, int(round(a.erode * W / 2560.0)))
        names_run = ['live'] if a.variant == 'live' else ['live', a.variant]   # (the live variant makes the region masks: always first)
        if not a.quick:
            names_run = list(dict.fromkeys(['live', 'bare'] + [k for k in VARIANTS if k not in ('live', 'bare')] + [a.variant]))
        scales_now = a.scales
        if a.fit_scales and not a.scales:
            scales_now = fit_scales(rig, rec, game, (W, H), os.path.join(out, '%s_f%d' % (tag, g)), live, curve, env0, [int(x) for x in a.fit_scales.split(',')], lines)
        results = {}
        masks = None
        for vn in names_run:
            v = VARIANTS[vn]
            prefix = os.path.join(out, '%s_f%d_%s' % (tag, g, vn))
            png, text = draw_variant(rig, rec, game, (W, H), prefix, v, per_family=(vn == 'live'), env0=env0, scales=scales_now)
            if png is None:
                lines.append('FAIL  frame %d variant %s: the rig failed: %s' % (g, vn, text))
                judged_fail.append('rig %d' % g)
                results = None
                break
            if vn == 'live':
                masks = region_masks(prefix, live.shape, er)
            rgb = load(png)
            if rgb.shape != live.shape:
                rgb = np.asarray(Image.fromarray(rgb).resize((W, H)), dtype=np.uint8)
            if v['curve']:
                rgb = curve[rgb, np.arange(3)[None, None, :]]
            if masks is None:
                lines.append('FAIL  frame %d: the live variant must run first (it makes the region masks)' % g)
                results = None
                break
            stats, d = compare(rgb, live, masks)
            results[vn] = (stats, d, rgb)
        if results is None:
            continue
        compared += 1
        lines.append('')
        lines.append('== frame %d (record %s; picture %dx%d; erode %d px; regions of the picture: %s) ==' % (
            g, os.path.basename(rec), W, H, er, ', '.join('%s %.1f %%' % (k, 100 * masks[k].mean()) for k in PRIORITY if masks[k].any())))
        vres = verdict(results[a.variant][0], a.ratio_tol, a.diff_tol)
        lines.append('judged variant: %s (%s)' % (a.variant, ', '.join('%s %s' % (k, 'on' if x else 'OFF') for k, x in VARIANTS[a.variant].items())))
        for name, ok, line in vres:
            lines.append(line)
            if not ok:
                judged_fail.append('%s %d' % (name, g))
        if not a.quick:
            lines.append('the explanation table (mean luminance ratio rig / live per region; * = outside the tolerance):')
            lines.append('  %-9s ' % 'variant' + ' '.join('%9s' % k for k in JUDGED) + '   switched off from the live settings')
            for vn in names_run:
                st = results[vn][0]
                row, bad = [], []
                for k in JUDGED:
                    s = st.get(k)
                    if not s or s['n'] == 0 or s['frac'] < MIN_FRAC:
                        row.append('%9s' % '-')
                        continue
                    rt, dt = tol_of(k, a.ratio_tol, a.diff_tol)
                    ok = abs(s['ratio'] - 1.0) <= rt and s['mad'] <= dt
                    row.append('%8.3f%s' % (s['ratio'], ' ' if ok else '*'))
                    if not ok:
                        bad.append(k)
                off = [k for k, x in VARIANTS[vn].items() if not x]
                lines.append('  %-9s ' % vn + ' '.join(row) + '   ' + (', '.join(off) or 'nothing'))
                if vn.startswith('no_'):
                    explain.setdefault(vn[3:], set()).update(bad)
        stl, dl, rl = results[a.variant]
        sheet(os.path.join(out, '%s_f%d_sheet.png' % (tag, g)), live, rl, dl, '%s frame %d, variant %s' % (tag, g, a.variant), stl)
        lines.append('sheet: %s' % os.path.join(out, '%s_f%d_sheet.png' % (tag, g)))
    if not a.quick and explain:
        lines.append('')
        lines.append('WHAT MAKES THE RIG DIFFER (a setting explains a region when taking it out of the live settings breaks that region\'s parity):')
        for k in ('curve', 'post', 'scale', 'hooks'):
            lines.append('  %-6s: %s' % (k, ', '.join(sorted(explain.get(k, []))) or 'no judged region depends on it'))
        lines.append('  not switchable in the rig (so not tested): the frame clock (header time_s 0), a quality tier, previous-frame screens.')
    if skipped:
        lines.append('NOTE  %d live picture(s) have no record in the case (it keeps the last records only): %s' % (len(skipped), ' '.join(map(str, skipped))))
    if compared == 0:
        lines.append('FAIL  no live picture had a record in the case: nothing compared')
        judged_fail.append('nothing compared')
    verdict_line = ('FAIL' if judged_fail else 'PASS') + '  rig parity (%s, variant %s): %d frame(s) compared%s' % (tag, a.variant, compared, ('; failed: ' + ', '.join(judged_fail)) if judged_fail else '')
    lines.append(verdict_line)
    text = '\n'.join(lines)
    print(text)
    open(os.path.join(out, 'rig_parity_%s_%s.txt' % (tag, a.variant)), 'w', encoding='utf-8').write(text + '\n')
    if a.expect_fail:
        if judged_fail and any(a.expect_fail in ln for ln in lines if ln.startswith('FAIL')):
            print('PASS  planted twin: failed with "%s" as expected' % a.expect_fail)
            return 0
        print('FAIL  planted twin: wanted a FAIL line holding "%s"; the run %s' % (a.expect_fail, 'failed otherwise' if judged_fail else 'passed'))
        return 1
    return 1 if judged_fail else 0


if __name__ == '__main__':
    sys.exit(main())
