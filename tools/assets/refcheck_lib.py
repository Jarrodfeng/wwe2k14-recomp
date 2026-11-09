r"""Session 98 step 15: the numbers behind tools\assets\refcheck.py (game frame vs Arena Viewer frame at the game's camera).
Pure numpy + PIL, no game data read here (the region geometry comes in as plain numbers from refcheck.py).

Image convention: float32 arrays H x W x 3, 0..255, the SAME size for both frames (refcheck resizes the game frame).
Cells: a COLS x ROWS grid (default 16 x 9). Flags are for eyes, not verdicts (LESSONS: an OVER is a flag, edge-only
differences go to the eye).
"""
import numpy as np
from PIL import Image, ImageDraw

BG_VIEWER = (31, 36, 46)  # the viewer's clear colour (renderer.cpp: 0.12 / 0.14 / 0.18 after the display curve)

DEFAULT_TOL = dict(
    dark_ratio=0.6, bright_ratio=1.6,  # viewer lum / game lum beyond these = DARK / BRIGHT ...
    min_diff=20.0,                      # ... and only when the lum difference is at least this (0..255)
    patch_dark=8.0, patch_flat=4.0, patch_other=40.0,  # BLACK PATCH: one side mean < dark and flat, the other mean > other
    white_min=235, white_frac=0.7, white_other=0.3,    # WHITE PATCH: share of pixels with every channel >= white_min
    bg_game=14.0, bg_game_std=6.0, missing_diff=25.0,  # MISSING: game cell flat and dark ("background-ish"), viewer covered
    extra_cov=0.25, extra_std=10.0,                    # EXTRA: viewer cell mostly void, game cell has structure
    ssim_flag=0.55,                                    # STRUCTURE: mean 1 - SSIM above this (only reported, never alone a verdict)
)


# ---------------------------------------------------------------------------------------------------------- helpers
def lum(a):
    return a[..., 0] * 0.2126 + a[..., 1] * 0.7152 + a[..., 2] * 0.0722


def box(a, r):
    """mean over a (2r+1)^2 window (edge-clamped), via a summed-area table"""
    k = 2 * r + 1
    p = np.pad(a, r, mode='edge')
    c = np.cumsum(np.cumsum(p, 0), 1)
    c = np.pad(c, ((1, 0), (1, 0)))
    s = c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]
    return s / (k * k)


def ssim_map(a, b, r=3):
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    ma, mb = box(a, r), box(b, r)
    va, vb = box(a * a, r) - ma * ma, box(b * b, r) - mb * mb
    cov = box(a * b, r) - ma * mb
    return ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2))


def grad_mag(l):
    l = box(l, 1)
    gy, gx = np.gradient(l)
    return np.hypot(gx, gy)


def best_shift(g, v, cx, cy, half=24, rad=8):
    """Shift (dx, dy) that moves the viewer onto the game around (cx, cy): the offset with the best normalised
    correlation of the gradient images (a ring edge is an edge in both). Returns (dx, dy, score) or None."""
    H, W = g.shape
    if cx - half - rad < 0 or cy - half - rad < 0 or cx + half + rad >= W or cy + half + rad >= H:
        return None
    pg = g[cy - half:cy + half, cx - half:cx + half]
    if pg.std() < 1.0:
        return None
    pg = pg - pg.mean()
    best = (-2.0, 0, 0)
    for dy in range(-rad, rad + 1):
        for dx in range(-rad, rad + 1):
            pv = v[cy - half + dy:cy + half + dy, cx - half + dx:cx + half + dx]
            s = pv.std()
            if s < 1e-6:
                continue
            sc = float((pg * (pv - pv.mean())).mean() / (pg.std() * s))
            if sc > best[0]:
                best = (sc, dx, dy)
    return (best[1], best[2], best[0]) if best[0] > -1 else None


def align_report(game, view, corners_px):
    """Global alignment (median shift over a patch grid) + the shift at each projected ring corner that is in frame."""
    gg, gv = grad_mag(lum(game)), grad_mag(lum(view))
    H, W = gg.shape
    pts = []
    for gy in range(1, 5):
        for gx in range(1, 7):
            r = best_shift(gg, gv, int(W * gx / 7), int(H * gy / 5))
            if r and r[2] > 0.35:
                pts.append(r)
    out = {'grid_patches': len(pts)}
    if pts:
        out['median_dx'] = float(np.median([p[0] for p in pts]))
        out['median_dy'] = float(np.median([p[1] for p in pts]))
    cs = []
    for name, (x, y) in corners_px:
        r = best_shift(gg, gv, int(round(x)), int(round(y)), half=20, rad=8)
        if r is not None and r[2] < 0.5:
            r = None  # no clear edge match: unmeasurable, never a made-up shift
        cs.append({'corner': name, 'px': [round(x, 1), round(y, 1)],
                   'dx': None if r is None else r[0], 'dy': None if r is None else r[1],
                   'score': None if r is None else round(r[2], 2)})
    out['corners'] = cs
    return out


def shift_image(a, dx, dy):
    """out[y, x] = a[y + dy, x + dx] (edge-clamped): moves the viewer frame onto the game frame by best_shift's (dx, dy)"""
    H, W = a.shape[:2]
    ys = np.clip(np.arange(H) + dy, 0, H - 1)
    xs = np.clip(np.arange(W) + dx, 0, W - 1)
    return a[ys][:, xs]


# --------------------------------------------------------------------------------------------------------- projection
def project(vp, pts):
    """vp = 16 floats (c56..c59 rows, file space); pts = [(x, y, z)] -> [(px, py, w)] in a unit square (0..1); w <= 0 = behind."""
    out = []
    for x, y, z in pts:
        c = [vp[4 * i] * x + vp[4 * i + 1] * y + vp[4 * i + 2] * z + vp[4 * i + 3] for i in range(4)]
        out.append(c)
    return out


def clip_poly(cl, eps=1.0):
    """clip-space points (x, y, z, w) -> polygon in unit coordinates, clipped to w > eps (Sutherland-Hodgman, one plane)"""
    res = []
    n = len(cl)
    for i in range(n):
        a, b = cl[i], cl[(i + 1) % n]
        ina, inb = a[3] > eps, b[3] > eps
        if ina:
            res.append(a)
        if ina != inb:
            t = (eps - a[3]) / (b[3] - a[3])
            res.append([a[k] + (b[k] - a[k]) * t for k in range(4)])
    return [((c[0] / c[3]) * 0.5 + 0.5, 1.0 - ((c[1] / c[3]) * 0.5 + 0.5)) for c in res]


def to_px(p, W, H):
    return (p[0] * W, p[1] * H)


def poly_mask(polys, W, H):
    im = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(im)
    for p in polys:
        if len(p) >= 3:
            d.polygon([to_px(q, W, H) for q in p], fill=255)
    return np.asarray(im) > 0


def quad_mask(vp, quads, W, H):
    return poly_mask([clip_poly(project(vp, q)) for q in quads], W, H)


def horizon_row(vp, eye, W, H):
    """pixel row of the horizon (a far point straight ahead at eye height) or None; f = the view-depth gradient (row 3)"""
    f = [vp[12], vp[13], vp[14]]
    f[1] = 0.0
    n = (f[0] ** 2 + f[2] ** 2) ** 0.5
    if n < 1e-6:
        return None
    far = [eye[0] + f[0] / n * 1e6, eye[1], eye[2] + f[2] / n * 1e6]
    c = project(vp, [tuple(far)])[0]
    if c[3] <= 0:
        return None
    return (1.0 - ((c[1] / c[3]) * 0.5 + 0.5)) * H


def build_regions(vp, eye, geo, W, H):
    """geo: {'canvas': half, 'canvas_y': y_file, 'apron_half', 'apron_y0', 'apron_y1', 'floor_y': y_file, 'floor_half',
    'stage': (x0, z0, x1, z1, y_file) | None, 'ramp': (..) | None, 'backdrop_h'} -> ordered {name: bool mask} (later wins)
    Regions that cannot be built (behind the camera, no data) come back empty; 'above horizon' and 'stands / rest' fill the rest."""
    regs = {}
    fh = geo.get('floor_half', 1600)
    fy = geo.get('floor_y', 0.0)
    regs['ringside floor'] = quad_mask(vp, [[(-fh, fy, -fh), (fh, fy, -fh), (fh, fy, fh), (-fh, fy, fh)]], W, H)
    for nm in ('ramp', 'stage'):
        r = geo.get(nm)
        if r:
            x0, z0, x1, z1, y = r
            regs[nm] = quad_mask(vp, [[(x0, y, z0), (x1, y, z0), (x1, y, z1), (x0, y, z1)]], W, H)
    st = geo.get('stage')
    if st:
        x0, z0, x1, z1, y = st
        zb = max(z0, z1)
        h = geo.get('backdrop_h', 650.0)
        regs['stage backdrop / screens'] = quad_mask(vp, [[(x0, y, zb), (x1, y, zb), (x1, y - h, zb), (x0, y - h, zb)]], W, H)
    a, y0, y1 = geo['apron_half'], geo['apron_y0'], geo['apron_y1']
    sides = [[(-a, y0, -a), (a, y0, -a), (a, y1, -a), (-a, y1, -a)], [(-a, y0, a), (a, y0, a), (a, y1, a), (-a, y1, a)],
             [(-a, y0, -a), (-a, y0, a), (-a, y1, a), (-a, y1, -a)], [(a, y0, -a), (a, y0, a), (a, y1, a), (a, y1, -a)]]
    regs['ring apron'] = quad_mask(vp, sides, W, H)
    c, cy = geo['canvas'], geo['canvas_y']
    regs['canvas'] = quad_mask(vp, [[(-c, cy, -c), (c, cy, -c), (c, cy, c), (-c, cy, c)]], W, H)
    taken = np.zeros((H, W), bool)
    for m in regs.values():
        taken |= m
    hr = horizon_row(vp, eye, W, H)
    up = np.zeros((H, W), bool)
    if hr is not None:
        up[:max(0, min(H, int(hr))), :] = True
    regs['above horizon (ceiling / rig / upper stands)'] = up & ~taken
    taken |= up
    regs['stands / crowd / barricade (rest)'] = ~taken
    # priority: the list order, later wins -> make them disjoint
    seen = np.zeros((H, W), bool)
    out = {}
    for k in reversed(list(regs)):
        out[k] = regs[k] & ~seen
        seen |= regs[k]
    return {k: out[k] for k in regs}


# ------------------------------------------------------------------------------------------------------------ compare
def compare(game, view, regions, ignore, cols=16, rows=9, tol=None):
    """-> dict with 'cells' (rows x cols list of dicts), 'regions' (name -> stats), 'flags' (list), 'maps' (numpy arrays for the
    heat map). `ignore` = bool mask of pixels to leave out (wrestlers, HUD)."""
    t = dict(DEFAULT_TOL)
    t.update(tol or {})
    H, W = game.shape[:2]
    lg, lv = lum(game), lum(view)
    ss = ssim_map(lg, lv)
    gm = grad_mag(lg)
    vm = grad_mag(lv)
    bg = np.all(np.abs(view - np.array(BG_VIEW(), np.float32)) <= 4, axis=2)
    diff = np.abs(game - view).mean(axis=2) / 255.0
    names = list(regions)
    reg_id = np.full((H, W), -1, int)
    for i, n in enumerate(names):
        reg_id[regions[n]] = i
    cells = []
    flags = []
    ch, cw = H // rows, W // cols
    for r in range(rows):
        row = []
        for c in range(cols):
            ys, xs = slice(r * ch, (r + 1) * ch if r < rows - 1 else H), slice(c * cw, (c + 1) * cw if c < cols - 1 else W)
            ig = ignore[ys, xs]
            ok = ~ig
            n_ok = int(ok.sum())
            cell = {'r': r, 'c': c, 'ignored': n_ok < 0.5 * ig.size}
            if cell['ignored']:
                row.append(cell)
                continue
            g, v = game[ys, xs][ok], view[ys, xs][ok]
            lgc, lvc = lg[ys, xs][ok], lv[ys, xs][ok]
            cov = float(1.0 - bg[ys, xs][ok].mean())
            gmean, vmean = lgc.mean(), lvc.mean()
            cell.update(game_rgb=[round(float(x), 1) for x in g.mean(0)], view_rgb=[round(float(x), 1) for x in v.mean(0)],
                        game_lum=round(float(gmean), 1), view_lum=round(float(vmean), 1),
                        ratio=round(float((vmean + 1) / (gmean + 1)), 2), colour_diff=round(float(diff[ys, xs][ok].mean()), 3),
                        structure=round(float(1.0 - ss[ys, xs][ok].mean()), 3),
                        edge_diff=round(float(abs(gm[ys, xs][ok].mean() - vm[ys, xs][ok].mean())), 2),
                        view_cov=round(cov, 2))
            ids = reg_id[ys, xs][ok]
            ids = ids[ids >= 0]
            cell['region'] = names[int(np.bincount(ids).argmax())] if ids.size else '-'
            gs, vs = float(lgc.std()), float(lvc.std())
            cell['flags'] = []
            ratio = (vmean + 1) / (gmean + 1)
            gwhite = float((g.min(axis=1) >= t['white_min']).mean())
            vwhite = float((v.min(axis=1) >= t['white_min']).mean())
            fl = None
            if vmean < t['patch_dark'] and vs < t['patch_flat'] and gmean > t['patch_other'] and gs > 8:
                fl = 'BLACK PATCH (viewer)'
            elif gmean < t['patch_dark'] and gs < t['patch_flat'] and vmean > t['patch_other'] and vs > 8:
                fl = 'BLACK PATCH (game)'
            elif vwhite > t['white_frac'] and gwhite < t['white_other']:
                fl = 'WHITE PATCH (viewer)'
            elif gwhite > t['white_frac'] and vwhite < t['white_other']:
                fl = 'WHITE PATCH (game)'
            elif cov > 0.5 and gmean < t['bg_game'] and gs < t['bg_game_std'] and vmean - gmean > t['missing_diff']:
                fl = 'MISSING (game lacks it)'
            elif cov < t['extra_cov'] and gs > t['extra_std']:
                fl = 'EXTRA (game has it, viewer void)'
            elif ratio < t['dark_ratio'] and gmean - vmean > t['min_diff']:
                fl = 'DARK (viewer)'
            elif ratio > t['bright_ratio'] and vmean - gmean > t['min_diff']:
                fl = 'BRIGHT (viewer)'
            if fl:
                cell['flags'].append(fl)
            if cell['structure'] > t['ssim_flag'] and not fl:
                cell['flags'].append('STRUCTURE')
            row.append(cell)
        cells.append(row)
    # region stats
    rstats = {}
    for i, n in enumerate(names):
        m = (reg_id == i) & ~ignore
        if not m.any():
            rstats[n] = {'pixels': 0}
            continue
        rstats[n] = {'pixels': int(m.sum()), 'share': round(float(m.sum()) / (H * W), 3),
                     'colour_diff': round(float(diff[m].mean()), 3),
                     'game_lum': round(float(lg[m].mean()), 1), 'view_lum': round(float(lv[m].mean()), 1),
                     'ratio': round(float((lv[m].mean() + 1) / (lg[m].mean() + 1)), 2),
                     'structure': round(float(1.0 - ss[m].mean()), 3),
                     'game_rgb': [round(float(x), 1) for x in game[m].mean(0)], 'view_rgb': [round(float(x), 1) for x in view[m].mean(0)]}
    # one line per (flag, region)
    grp = {}
    for row in cells:
        for cell in row:
            for f in cell.get('flags', []):
                grp.setdefault((f, cell['region']), []).append(cell)
    for (f, rg), cs in sorted(grp.items(), key=lambda kv: (kv[0][0] == 'STRUCTURE', -len(kv[1]))):
        gl = float(np.mean([c['game_lum'] for c in cs]))
        vl = float(np.mean([c['view_lum'] for c in cs]))
        flags.append({'flag': f, 'region': rg, 'cells': len(cs), 'frame_share': round(len(cs) / (cols * rows), 3),
                      'game_lum': round(gl, 1), 'view_lum': round(vl, 1), 'at': ['r%dc%d' % (c['r'] + 1, c['c'] + 1) for c in cs]})
    return {'cells': cells, 'regions': rstats, 'flags': flags, 'cols': cols, 'rows': rows, 'diff': diff, 'ssim': ss, 'cov': ~bg}


def BG_VIEW():
    return BG_VIEWER


# ------------------------------------------------------------------------------------------------------------- pictures
def heat(diff):
    """0..1 -> dark blue .. red (a small ramp, no matplotlib)"""
    d = np.clip(diff * 3.0, 0, 1)
    r = np.clip(d * 2.0, 0, 1)
    g = np.clip(2.0 - np.abs(d * 4.0 - 2.0), 0, 1) * 0.8
    b = np.clip(1.0 - d * 2.0, 0, 1) * 0.6
    return (np.stack([r, g, b], axis=2) * 255).astype(np.uint8)


def arr_img(a):
    return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))


FLAG_COL = {'BLACK PATCH': (255, 0, 255), 'WHITE PATCH': (255, 255, 0), 'MISSING': (255, 128, 0), 'EXTRA': (0, 200, 255),
            'DARK': (80, 120, 255), 'BRIGHT': (255, 60, 60), 'STRUCTURE': (160, 160, 160)}


def flag_color(f):
    for k, c in FLAG_COL.items():
        if f.startswith(k):
            return c
    return (255, 255, 255)


def outline_image(base, res, regions, corners_px):
    """the game frame with flagged cells outlined (colour per flag, label = short flag) and region borders in faint white"""
    im = arr_img(base).convert('RGB')
    H, W = base.shape[:2]
    d = ImageDraw.Draw(im, 'RGBA')
    ch, cw = H // res['rows'], W // res['cols']
    for row in res['cells']:
        for c in row:
            if c.get('ignored'):
                d.rectangle([c['c'] * cw, c['r'] * ch, (c['c'] + 1) * cw - 1, (c['r'] + 1) * ch - 1], fill=(0, 0, 0, 110))
                continue
            for k, f in enumerate(c.get('flags', [])):
                col = flag_color(f)
                x0, y0 = c['c'] * cw + 2 + k * 2, c['r'] * ch + 2 + k * 2
                d.rectangle([x0, y0, (c['c'] + 1) * cw - 3 - k * 2, (c['r'] + 1) * ch - 3 - k * 2], outline=col + (255,), width=3)
                if k == 0:
                    d.text((x0 + 4, y0 + 4), f.split(' ')[0], fill=col + (255,))
    for name, (x, y) in corners_px:
        d.ellipse([x - 5, y - 5, x + 5, y + 5], outline=(0, 255, 0, 255), width=2)
    return im


def region_border_image(base, regions):
    im = arr_img(base).convert('RGB')
    a = np.asarray(im).copy()
    for m in regions.values():
        e = m & ~(np.roll(m, 1, 0) & np.roll(m, -1, 0) & np.roll(m, 1, 1) & np.roll(m, -1, 1))
        a[e] = (255, 255, 255)
    return Image.fromarray(a)
