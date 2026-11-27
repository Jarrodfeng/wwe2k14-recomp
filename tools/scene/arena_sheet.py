#!/usr/bin/env python3
r"""WGS: every match arena drawn OFFLINE by our renderer from the player's own files, a test pattern on every stage screen, and every screen checked by number.
(docs\agents\arena_sheet.md; contract docs\research\harness\WGS_contract.md + round 2 + the review fix round.) 0 game runs: arena_rec.exe builds a record from the files (no capture),
frame_rig.exe --serve draws it through the game's own loader (GameFilesSource), appliers and front end. Nothing on screen: the rig has no window.

  py -I tools\scene\arena_sheet.py [--arenas 4,44] [--plant highlight | --both] [--strict] [--out DIR] [--sheets DIR] [--no-sheet]

THE CHECK PASS (the numbers; it does not depend on any look or on the beauty camera). Per arena, every screen key 800-809 with a mesh:
  arena_rec --screen-pieces splits each key into connected pieces (arena_cams.h SplitPieces: corners welded by position rounded to 1 unit) and prints the 32 largest (and the area share
  of any dropped), each with its cameras: `front` (on the side its geometric normal points to) and `back`; a weak-normal (closed / folded) piece four horizontal views x+ x- z+ z-.
  Every camera line carries expect_px: the pixels where the piece is DRAWN at the check size. WGS6: the key is rasterised as the game draws it (one mesh, triangles in index order, camera-facing ones,
  depth test + write, the game's alpha test GEQUAL 1/255, blended): a pixel an EARLIER, nearer piece of the same key covers is not this piece's (hidden_px counts them), and the planes carry the
  composite colour (per channel) and alpha the camera sees there. Occlusion is modelled within one key only (the screens-only render has no other keys, no arena).
  Per view: frame_rig --families 0x20 (the screens alone) --show K (the key alone) --look-entrance --camera / --eye, env WWE_RDR_RIG_SCREEN_ROWS=1 + WWE_RDR_RIG_SCREEN_FEED=pattern;
  key 800 IS the frame grab in every look (front_end.cpp, the game's channel -1; PAR1): its jobs add --prime-grab 800 (the grab = 800's test picture, sampled through the grab's own
  binding, no seed frame); every served job drops the grab first (a served job = a one-shot). A second job per view with --flat-feed judges brightness (DIM1). judge_piece:
    PASS / FAINT (PASS, a see-through floor copy) / BLACK / DIM / PARTIAL (drawn / expected < coverage) / UNCLEAR / NO PIXELS; the back camera is rendered only when the front
    draws nothing, and a piece drawn only from behind is PASS-BACK (a cull fault).
THE BEAUTY TILES (for Jarrod's eyes; no number is taken from them): per look, arena_rec --camera-screens entrance|match (BeautyCam), the whole frame with the look's legacy set (TOLD).
--plant highlight sets WWE_RDR_PLANT_SCREEN_HIGHLIGHT=1 (Plants::screen_highlight_kept: the TRN4 fix undone) in its own rig process. --both runs both arms side by side.
NOT JUDGED (never a pass, never a strict failure): TOO FAINT (a see-through copy too faint for the hue fit), EDGE-ON (no camera-facing pixel from any view).
EXIT: with --both or --strict, 1 on any unplanted finding NOT in the known list tools\scene\arena_sheet_known.txt (EXPECT_PLANT keys included: a deleted TRN4 fix fails here), on a
listed piece whose verdict changed (PASS included: remove it), always on BLACK / PASS-BACK / NO PIXELS, or a planted arm off its expected table; --plant alone compares with a FRESH
arenas_unplanted.json (same thresholds, newer than the tools) or refuses (2). --write-known regenerates the list (all 49 arenas). 2 = could not run. The report is written before the
sheets; the per-piece renders are deleted once judged unless --keep-renders.
Pictures of game assets stay under out\ and are never committed (hard rule 1).
"""
import argparse, json, os, re, subprocess, sys, threading, time
import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))

# The match arenas (db\ids\arenas.json): ids 0-9, 11-17, 21-23, 28-56.
MATCH_ARENAS = list(range(0, 10)) + list(range(11, 18)) + list(range(21, 24)) + list(range(28, 57))
KEYS = list(range(800, 810))
# The legacy show sets per look (stage_show.h LegacyShow), written as --show rows (TOLD) for the beauty tiles.
LOOK_SET = {'entrance': [802, 804, 805, 807, 809], 'match': [800, 801, 805, 806, 808]}
SCENE_WORD = {'entrance': 4, 'match': 1}
# The rig's pattern colours (frame_rig.cpp RigScreenFeed::PatternColour kCol: the unit checks the two tables are equal).
PATTERN = {800: (255, 0, 0), 801: (0, 255, 0), 802: (0, 64, 255), 803: (255, 255, 0), 804: (255, 0, 255), 805: (0, 255, 255), 806: (255, 128, 0), 807: (128, 0, 255), 808: (0, 255, 128), 809: (255, 0, 128)}
# WGS6 round 2: the FLAT feed's colour per key (frame_rig.cpp RigScreenFeed::FlatColour kFlat: the unit checks the two tables are equal): pastels with all three channels at 100 or more, distinct per key, |R - B| >= 70, and every key's largest channel 255 (the same measurability as the saturated pattern colours).
# The flat render is where the vertex colour's per-channel tint is read; a key colour with no blue (the pattern's 800, 801, 803, 806) could not show a blue tint and a red / blue swap changed nothing.
FLAT = {800: (255, 170, 150), 801: (200, 255, 130), 802: (150, 185, 255), 803: (255, 235, 140), 804: (255, 140, 170), 805: (140, 255, 235), 806: (255, 205, 120), 807: (170, 150, 255), 808: (130, 255, 200), 809: (255, 105, 175)}
CLEAR = (31, 36, 46)   # the front end's clear colour (frame_rig.py CLEAR)
# The check's thresholds; set from the full unplanted check pass (docs\agents\arena_sheet.md "Thresholds": the numbers they came from).
TH = {
    'min_px': 20,          # fewer non-clear pixels than this = NO PIXELS
    'dark_luma': 0.05,     # a pixel with luma below this is dark
    'black_dark': 0.50,    # BLACK = more than this share dark ...
    'black_pattern': 0.10, # ... and less than this share pattern
    'pass_pattern': 0.80,  # PASS = at least this share of the drawn pixels is the key's pattern colour
    'coverage': 0.80,      # PARTIAL = drawn / expected pixels below this (expected = the piece's camera-facing triangles rasterised by arena_rec)
    'dim_ratio': 0.60,     # DIM (DIM1, docs\research\harness\DIM1.md) = the FLAT render's median (largest channel / (255 x interpolated vertex colour)) below this, on a piece that is not see-through
    'dim_median': 0.60,    # the fallback without a flat render only (units, back views): the pattern render's median a + g below this (the first judge: it measured the pattern's digits)
    'faint_a': 0.50,       # FAINT (a PASS, named) = median a below this ...
    'faint_g': 0.50,       # ... and median g above this (a real see-through copy over the clear colour)
    'meas_min': 6.0,       # round 5: a footprint pixel is measured for brightness when its expected signal alpha x vc x (P[k] - C[k]) is at least this many levels (the fit's misfit tolerance)
    'too_faint_levels': 24.0,   # round 5: TOO FAINT only when the median expected signal is below this (24 levels = a vertex alpha of about 0.1 on a white vertex colour) or nothing is measurable
    'too_faint_a': 0.20,   # (the fallback without a flat render only) TOO FAINT = a see-through copy that fails the hue test with median a below this. From the round-3 full run: the 163 blended
                           # UNCLEAR views had median a 0.02-0.16 (p95 0.10); the 436 blended FAINT (passing) views 0.02-0.25 (median 0.04): at these strengths the pattern adds a few
                           # levels over the clear colour and the hue fit is noise, either way
    # a pixel is the key's pattern colour when px = a P + g C (pattern_fit) with:
    'pat_min': 0.02,       # a >= this
    'sum_min': 0.30,       # a + g >= this (a near-black pixel with a slight hue is NOT the pattern: the titantron fault's own form)
    'g_lo': -0.15,         # g (the clear colour's share behind it) in [g_lo, g_hi]
    'g_hi': 1.15,
    'res_abs': 6.0,        # misfit at most max(res_abs, res_rel x |px|) (0..255 units)
    'res_rel': 0.10,
    'hidden_share': 0.5,   # WGS6 round 2: a view with more than this share of its pixels hidden by an earlier nearer piece of the key is HIDDEN (not judged)
    'dark_patch': 0.01,    # WGS6 round 2: PASS needs at most this share of the measured pixels drawn dark on the flat render where the expectation is lit (a black patch). From the full run: the dark share of all 2,860 PASS / FAINT views is 0.000 (max 0.000); 0.01 leaves room for rounding and catches a patch above 1 % of a piece's measured pixels
}
CHECK_SIZE, BEAUTY_SIZE = '640x360', '960x540'
OK = ('PASS', 'FAINT')
NOT_JUDGED = ('TOO FAINT', 'EDGE-ON', 'HIDDEN')   # contract rule 5: could not be judged, says so, never a pass, never a strict failure
NEVER_KNOWN = ('BLACK', 'PASS-BACK', 'NO PIXELS')   # may never sit in the known list: always a strict failure
SEVERITY = ['BLACK', 'NO PIXELS', 'DIM', 'PARTIAL', 'UNCLEAR', 'PASS-BACK', 'TOO FAINT', 'EDGE-ON', 'HIDDEN', 'FAINT', 'PASS']   # worst first
VERDICT_COLOUR = {'PASS': (90, 220, 90), 'FAINT': (150, 230, 200), 'BLACK': (255, 70, 70), 'DIM': (255, 110, 40), 'PARTIAL': (255, 210, 60), 'UNCLEAR': (255, 120, 220),
                  'NO PIXELS': (255, 190, 60), 'PASS-BACK': (120, 160, 255), 'TOO FAINT': (160, 160, 160), 'EDGE-ON': (160, 160, 160), 'HIDDEN': (160, 160, 160), 'no mesh': (150, 150, 150), 'not checked': (150, 150, 150)}
KNOWN_FILE = os.path.join(HERE, 'arena_sheet_known.txt')
KNOWN_OWNER = {'DIM': r'docs\research\harness\DIM1.md[L]','PARTIAL': r'docs\research\harness\PAR1.md', 'UNCLEAR': 'unowned'}
RIG_INPUT_TEXT = "without the rig's values (no stage look)"   # stage_look::MissingFor (rf_arena_live.cpp (rdr_scene::rf::c::ApplyArenaLive)): offline the record has no stage-look rig rows; it always fires
# The planted arm's expected keys: the file census in TRN4.md lines 48-51 (the 22 highlighted screen-key meshes): BLACK on every piece; a53 801 / 802 vanish instead of going black
# ([U] why): "not PASS" there (BLACK or NO PIXELS). Every other key of every arena = the unplanted arm's verdict.
EXPECT_PLANT = {30: {800, 803, 804}, 33: {800, 803, 804}, 35: {800, 803, 804}, 44: {800, 803, 804}, 50: {800, 803, 804, 806}, 53: {800, 801, 802, 803, 804}, 54: {806}}
EXPECT_NOT_PASS = {53: {801, 802}}


def luma(px):
    """px: (..., 3) uint8 or float 0..255 -> luma 0..1 (Rec.709 weights, on the picture as written: no curve)"""
    p = np.asarray(px, dtype=np.float64) / 255.0
    return p[..., 0] * 0.2126 + p[..., 1] * 0.7152 + p[..., 2] * 0.0722


def vc_rgb(vc):
    """a vertex-colour plane as floats 0..1 with a trailing channel axis (R, G, B): an (H, W, 3) plane (arena_rec's `.vc`, WGS6: per channel) as is; a grey (H, W) plane repeated to three channels"""
    v = np.asarray(vc, dtype=np.float64) / 255.0
    return v if (v.ndim == 3 and v.shape[-1] == 3) else np.repeat(v[..., None], 3, axis=-1)


def pattern_fit(px, key, th=TH, scale=None):
    """per pixel of px (N, 3): px = a P + g C by least squares (P = key's colour, C = the clear colour). Returns (is_pattern bool N, a N, g N). A pixel is the pattern when a >= pat_min,
    a + g >= sum_min, g in [g_lo, g_hi] and the misfit <= max(res_abs, res_rel |px|): the pattern at any brightness down to 0.3 and a blend over the clear colour; not a near-black pixel
    with a slight hue, not another key's colour, white or grey.
    WGS6: `scale` (per pixel, (N, 3) R G B 0..1, or (N,) grey) = the interpolated vertex colour. The texel is multiplied by it PER CHANNEL (the game's look and ours), so a tinted vertex colour
    (R 255 G 216 B 159) tints the picture: the fit's basis column is P x vc (the key's colour as that pixel should draw it), and `a` is reported as a x max(vc), the scalar the first judge used
    (identical to it for a grey vertex colour: same span of colours, same g, same misfit). The floor a + g >= sum_min scales with max(vc), clamped to [0.25, 1] (DIM1)."""
    p = np.asarray(px, dtype=np.float64).reshape(-1, 3)
    P = np.asarray(PATTERN[key], dtype=np.float64)
    C = np.asarray(CLEAR, dtype=np.float64)
    if scale is None:
        A = np.stack([P, C], axis=1)   # 3 x 2
        coef = p @ np.linalg.pinv(A).T
        a, g = coef[:, 0], coef[:, 1]
        res = np.linalg.norm(p - coef @ A.T, axis=1)
        sm = th['sum_min']
    else:
        sc = np.asarray(scale, dtype=np.float64)
        v = np.repeat(sc.reshape(-1, 1), 3, axis=1) if sc.ndim <= 1 else sc.reshape(-1, 3)
        smax = v.max(axis=1)
        u = np.where((smax >= 1.0 / 255.0)[:, None], P[None, :] * v, P[None, :])   # an all-black vertex colour: the plain key colour (the old behaviour)
        uu, uc, up = (u * u).sum(1), (u @ C), (u * p).sum(1)
        cc, cp = float(C @ C), p @ C
        det = uu * cc - uc * uc
        good = det > 1e-9 * np.maximum(uu * cc, 1e-12)
        dd = np.where(good, det, 1.0)
        au = np.where(good, (cc * up - uc * cp) / dd, 0.0)
        g = np.where(good, (uu * cp - uc * up) / dd, cp / cc)
        res = np.linalg.norm(p - au[:, None] * u - g[:, None] * C[None, :], axis=1)
        a = au * np.where(smax >= 1.0 / 255.0, smax, 1.0)
        sm = th['sum_min'] * np.clip(smax, 0.25, 1.0)
    ok = (a >= th['pat_min']) & (a + g >= sm) & (g >= th['g_lo']) & (g <= th['g_hi']) & (res <= np.maximum(th['res_abs'], th['res_rel'] * np.linalg.norm(p, axis=1)))
    return ok, a, g


def pattern_mask(px, key, th=TH):
    return pattern_fit(px, key, th)[0]


def mask_of(img):
    """the non-clear pixels of a screens-only render (RGB uint8 H x W x 3) -> bool H x W"""
    d = np.abs(img.astype(np.int16) - np.asarray(CLEAR, dtype=np.int16))
    return (d > 2).any(axis=2)


def read_footprint(path, w, h):
    """arena_rec's footprint of one view (packed bits, MSB first, row 0 = NDC y -1) as bool H x W in the PICTURE's orientation: the rig's picture has NDC y +1 at row 0, so the rows are
    flipped (checked on arena 4's 801 pieces 4 / 5 / 13 and 805 piece 0: flipped, the footprint lies wholly inside the drawn key; unflipped it does not)."""
    bits = np.unpackbits(np.fromfile(path, dtype=np.uint8))[:w * h]
    return bits.reshape(h, w).astype(bool)[::-1]


def read_vc(path, w, h):
    """arena_rec's vertex-colour plane of one view (WGS6: 3 bytes R G B a pixel, the footprint's rows): the composite colour the texel is multiplied by, per channel, 0..255; flipped like read_footprint"""
    return np.fromfile(path, dtype=np.uint8)[:w * h * 3].reshape(h, w, 3)[::-1]


def read_va(path, w, h):
    """arena_rec's alpha plane of one view (1 byte a pixel, the footprint's rows): the composite alpha 0..255, flipped like read_footprint"""
    return np.fromfile(path, dtype=np.uint8)[:w * h].reshape(h, w)[::-1]


def flat_ratio(flat, vc, footprint, key=800, va=None, th=TH):
    """DIM1 + review round 5 + WGS6: the brightness of the flat render against what it should be, per footprint pixel, on the key colour's largest channel k:
    expected = alpha x vc[k] x P[k] + (1 - alpha) x C[k] (the texel x the vertex colour's channel k, blended over the clear colour at the composite alpha; alpha = 1 on an opaque piece;
    vc = (H, W, 3) per channel, or a grey (H, W) plane);
    ratio = (drawn[k] - (1 - alpha) x C[k]) / (alpha x vc x P[k]). Only pixels whose expected signal alpha x vc x (P[k] - C[k]) is at least meas_min levels are measured.
    Returns (median ratio, p5, p95, n measured, median expected signal in levels) or None when fewer than min_px pixels are measurable."""
    P = np.asarray(FLAT[key], dtype=np.float64)   # WGS6 round 2: the flat feed's own colour
    k = int(np.argmax(P))
    C = float(CLEAR[k])
    al = (va.astype(np.float64) / 255.0) if va is not None else np.ones(footprint.shape)
    v = vc_rgb(vc)[..., k]
    signal = al * v * (P[k] - C)
    m = footprint & (signal >= th['meas_min'])
    n = int(m.sum())
    if n < th['min_px']:
        return None
    d = flat[..., k].astype(np.float64)
    r = (d[m] - (1.0 - al[m]) * C) / np.maximum(al[m] * v[m] * P[k], 1e-6)
    return float(np.median(r)), float(np.percentile(r, 5)), float(np.percentile(r, 95)), n, float(np.median(signal[m]))


def flat_hue(flat, vc, footprint, key=800, va=None, th=TH):
    """WGS6 + round 2: the COLOUR of the flat render against what it should be on all three channels, per measured footprint pixel (the pixels flat_ratio measures): expected = alpha x vc x F + (1 - alpha) x C
    (F = the flat feed's colour FLAT[key], vc per channel). Per pixel misfit = |drawn - expected| / max(res_abs, res_rel |expected|) (the pattern test's own tolerance). Returns a dict, or None when fewer
    than min_px pixels are measurable: 'median' the median misfit (a median, like the DIM ratio: a thin sliver the renderer draws differently does not decide it; above 1 when most pixels are off),
    'p90' its 90th percentile (a patch of more than a tenth of the pixels pushes it up), 'dark' the share of the measured pixels the flat render draws DARK (luma below dark_luma) where the expectation is
    lit (luma at least twice dark_luma): a black patch. flat_ratio sees only the key colour's largest channel: a vertex colour ignored (or a tint not applied) reads 1.18 there on a 0.85 tint."""
    P = np.asarray(FLAT[key], dtype=np.float64)
    C = np.asarray(CLEAR, dtype=np.float64)
    k = int(np.argmax(P))
    al = (va.astype(np.float64) / 255.0) if va is not None else np.ones(footprint.shape)
    v = vc_rgb(vc)
    m = footprint & (al * v[..., k] * (P[k] - C[k]) >= th['meas_min'])
    if int(m.sum()) < th['min_px']:
        return None
    E = al[..., None] * v * P[None, None, :] + (1.0 - al[..., None]) * C[None, None, :]
    d = flat.astype(np.float64)[m]
    err = np.linalg.norm(d - E[m], axis=1)
    tol = np.maximum(th['res_abs'], th['res_rel'] * np.linalg.norm(E[m], axis=1))
    mis = err / tol
    dark = (luma(d) < th['dark_luma']) & (luma(E[m]) >= 2.0 * th['dark_luma'])
    return {'median': float(np.median(mis)), 'p90': float(np.percentile(mis, 90)), 'dark': float(dark.mean()), 'n': int(m.sum())}


def expected_visible(footprint, vc, va, key=800, th=TH):
    """WGS6: how many footprint pixels the piece SHOULD draw at a measurable signal (some channel of alpha x vc x F + (1 - alpha) x C at least meas_min levels off the clear colour: the same
    measurability rule flat_ratio uses). A piece mostly hidden by an earlier nearer piece, or one whose visible part is the invisible fringe of a fade (a54 808 / 809 piece 13: alpha 1-4 of 255,
    a signal of 0.9-3.4 levels), has fewer than min_px: nothing to judge (not NO PIXELS)."""
    P = np.asarray(FLAT[key], dtype=np.float64)
    C = np.asarray(CLEAR, dtype=np.float64)
    al = (va.astype(np.float64) / 255.0) if va is not None else np.ones(footprint.shape)
    E = al[..., None] * vc_rgb(vc) * P[None, None, :] + (1.0 - al[..., None]) * C[None, None, :]
    return int((footprint & (np.abs(E - C[None, None, :]).max(axis=2) >= th['meas_min'])).sum())


# entry: judge_piece
def judge_piece(img, key, expect_px=None, footprint=None, th=TH, blended=False, flat=None, vc=None, va=None):
    """the check of one view of one piece: img = the screens-only, key-alone render (H x W x 3 uint8). footprint (bool H x W, arena_rec's rasterised camera-facing triangles of THIS
    piece): only its pixels are judged (other pieces of the key in view are not this piece) and coverage = its pixels that differ from the clear colour at all / its size. Without a
    footprint, expect_px (a count) gives the coverage of every drawn pixel. Returns (verdict, stats). A see-through piece (median g > faint_g: a vertex-alpha copy whose alpha fades to 0
    leaves parts of its quad exactly the clear colour by design) gets its coverage reported, never PARTIAL."""
    m = mask_of(img)
    if footprint is not None:
        m = m & footprint
        expect_px = int(footprint.sum())
        drawn_cov = int(((np.abs(img.astype(np.int16) - np.asarray(CLEAR, dtype=np.int16)).max(axis=2) > 0) & footprint).sum())
    else:
        drawn_cov = int(m.sum())
    n = int(m.sum())
    st = {'px': n, 'expect': expect_px, 'coverage': (drawn_cov / expect_px) if expect_px else None, 'pattern': 0.0, 'dark': 0.0, 'luma': 0.0, 'a_med': 0.0, 'g_med': 0.0, 'ag_med': 0.0,
          'other_rgb': None, 'other_share': 0.0}
    if n < th['min_px']:
        # WGS6: NO PIXELS is a fault only where the expectation itself has min_px visible pixels; a piece the key's earlier, nearer pieces hide (or whose visible part is an invisible fade) is not judged
        if footprint is not None and vc is not None and expected_visible(footprint, vc, va, key, th) < th['min_px']:
            return ('TOO FAINT' if blended else 'EDGE-ON'), st
        return 'NO PIXELS', st
    px = img[m].astype(np.float64)
    L = luma(px)
    vscale = vc_rgb(vc)[m] if (vc is not None and footprint is not None) else None   # DIM1 + WGS6: the vertex colour (R, G, B) the texel is multiplied by, per pixel
    ok, a, g = pattern_fit(px, key, th, vscale)
    st['luma'] = float(L.mean())
    st['dark'] = float((L < th['dark_luma']).mean())
    st['pattern'] = float(ok.mean())
    st['a_med'], st['g_med'], st['ag_med'] = float(np.median(a)), float(np.median(g)), float(np.median(a + g))
    if (~ok).any():
        st['other_rgb'] = tuple(int(v) for v in np.median(px[~ok], axis=0))
        st['other_share'] = float((~ok).mean())
    if st['dark'] > th['black_dark'] and st['pattern'] < th['black_pattern']:
        return 'BLACK', st
    see_through = st['a_med'] < th['faint_a'] and st['g_med'] > th['faint_g']
    have_flat = flat is not None and vc is not None and footprint is not None
    fr = flat_ratio(flat, vc, footprint, key, va, th) if have_flat else None
    if fr is not None:
        st['flat_ratio'], st['flat_p5'], st['flat_p95'], st['flat_px'], st['signal'] = fr
    fh = flat_hue(flat, vc, footprint, key, va, th) if have_flat else None   # WGS6: the flat render's colour on all three channels
    if fh is not None:
        st['flat_hue'], st['flat_hue_p90'], st['flat_dark'] = fh['median'], fh['p90'], fh['dark']
    if st['coverage'] is not None and st['coverage'] < th['coverage'] and st['g_med'] <= th['faint_g']:
        return 'PARTIAL', st   # the coverage (a geometric fact) before the brightness
    if have_flat:   # DIM1 + round 5: brightness on the FLAT render against alpha x vc x P + (1 - alpha) x C, for every piece, blended ones included
        st['dim_from'] = 'flat vs alpha x vertex colour'
        if fr is not None and fr[0] < th['dim_ratio']:
            return 'DIM', st   # a vertex-alpha screen whose texel went black reads ~0 here: never TOO FAINT
    else:   # no flat render (a unit without one, a back view): the pattern render's a + g, the first judge
        st['dim_from'] = 'pattern a + g (no flat render)'
        if st['ag_med'] < th['dim_median']:
            return 'DIM', st
    # WGS6: and the flat render's colour is what the vertex colour makes it (a tint ignored is not a PASS); round 2: and no black patch (a share of the measured pixels dark where the expectation is lit)
    if st['pattern'] >= th['pass_pattern'] and (fh is None or (fh['median'] <= 1.0 and fh['dark'] <= th['dark_patch'])):
        return ('FAINT' if see_through else 'PASS'), st
    # TOO FAINT (not judged) only where the EXPECTED contribution alpha x vc x (P - C) is itself too small for the hue fit (median below too_faint_levels, or nothing
    # measurable), and the brightness matched (no DIM above): never because the drawn colour is missing
    small = (fr is None) or (fr[4] < th['too_faint_levels'])
    if blended and see_through and st['g_med'] <= th['g_hi'] and small and (have_flat or st['a_med'] < th['too_faint_a']):
        return 'TOO FAINT', st
    return 'UNCLEAR', st


def apply_hidden(v, s, view, th=TH):
    """WGS6 round 2: a view with more than th['hidden_share'] of its pixels hidden by an earlier nearer piece of the key (view = arena_rec's camera line: 'expect' visible px, 'hidden' hidden px) is HIDDEN: not
    judged, never a pass. Only a verdict that would have been a pass or a not-judged one becomes HIDDEN: a fault found on the visible part (BLACK, DIM, ...) stays, never hidden behind a grey.
    Records the counts in the stats (`hidden`, `hidden_share`, and `hidden_was` when it converted)."""
    hid, ex = view.get('hidden', 0), view.get('expect', 0)
    s['hidden'] = hid
    s['hidden_share'] = hid / float(hid + ex) if hid + ex else 0.0
    if s['hidden_share'] > th['hidden_share'] and v in OK + ('TOO FAINT', 'EDGE-ON'):
        s['hidden_was'] = v
        return 'HIDDEN'
    return v


def worst(verdicts):
    vs = list(verdicts)
    for w in SEVERITY:
        if w in vs:
            return w
    return 'no mesh'


def piece_verdict(views):
    """one piece's verdict from its judged views [(view, verdict, stats)]. A normal piece: the front view; when the front draws nothing and the back view was judged and drew, the piece
    is PASS-BACK (its pattern drawn only from behind: a cull / winding fault) or the back's own fault. A weak piece (x+ x- z+ z-): the worst of its four views."""
    d = {v: (vv, s) for v, vv, s in views}
    if 'front' in d:
        fv = d['front'][0]
        if fv == 'HIDDEN' and 'alt' in d:   # WGS6 round 2: the front is mostly hidden by the key's earlier nearer pieces: the piece is judged from its alternative camera
            return d['alt'][0]
        if fv == 'NO PIXELS' and 'back' in d and d['back'][0] != 'NO PIXELS':
            return 'PASS-BACK' if d['back'][0] in OK else d['back'][0]
        return fv
    judged = [vv for vv, _ in d.values() if vv != 'HIDDEN']   # a HIDDEN view is not judged: the piece's verdict is the worst of the others; HIDDEN only when no view is judged
    return worst(judged) if judged else 'HIDDEN'



def grab_repeat_result(v1, s1, v2, s2):
    """PAR1: the same key-800 view judged twice in one serve process (other jobs between): (same?, text). A kept grab nested earlier frames and moved the pixel count."""
    same = v1 == v2 and s1.get('px') == s2.get('px')
    return same, 'first %s px %s, again %s px %s' % (v1, s1.get('px'), v2, s2.get('px'))


def edge_on(views):
    """a piece's camera lines {view: {'expect': n, 'hidden': n, ...}}: True when no view has a single camera-facing pixel, drawn or hidden (an edge-on / degenerate piece: EDGE-ON, not judged). A piece
    whose every pixel is hidden by earlier nearer pieces (expect 0, hidden > 0) is not edge-on: it is HIDDEN (WGS6 round 2)."""
    return bool(views) and all(v.get('expect', 0) == 0 and v.get('hidden', 0) == 0 for v in views.values())


def key_verdict(pieces):
    """one key's verdict from its pieces' verdicts {piece: verdict}: the worst"""
    return worst(pieces.values())


KNOWN_METRIC = {'DIM': 'flat_ratio', 'PARTIAL': 'coverage', 'UNCLEAR': 'pattern'}   # the number that made each kind of verdict (stats key)
KNOWN_MARGIN = 0.15   # --write-known writes the band as the measured number +/- this (absolute)


def load_known(path=KNOWN_FILE):
    """the committed list of OPEN findings: {(arena, key, piece): (verdict, class, owner, view, metric, lo, hi)}. Lines `arena key piece verdict class owner view metric lo hi` (WGS6 round 2: the VIEW
    that made the verdict and a band for the number that made it; a row with only the first six fields has view / metric / band None and strict mode refuses it); '#' starts a comment."""
    known = {}
    try:
        with open(path, encoding='utf-8') as f:
            for ln in f:
                ln = ln.split('#')[0].strip()
                if not ln:
                    continue
                v = ln.split()
                if len(v) >= 10:
                    known[(int(v[0]), int(v[1]), int(v[2]))] = (v[3], v[4], v[5], v[6], v[7], float(v[8]), float(v[9]))
                elif len(v) >= 6:
                    known[(int(v[0]), int(v[1]), int(v[2]))] = (v[3], v[4], v[5], None, None, None, None)
    except OSError:
        pass
    return known


def write_known(unplanted, path=KNOWN_FILE, grows='', views=None):
    """--write-known: the list from an unplanted arm {arena: {key: {piece: verdict}}} and its views {(arena, key, piece): [(view, verdict, stats)]}. BLACK, PASS-BACK and NO PIXELS (an EDGE-ON is its own
    verdict) may never be listed: they are refused (not written; returned with the reason, and they stay strict failures); every other open finding is written with the first view that has the
    piece's verdict and the number that made it (flat ratio for DIM, coverage for PARTIAL, pattern share for UNCLEAR) as a band: measured +/- KNOWN_MARGIN. A finding with no such view is refused."""
    old = load_known(path)
    views = views or {}
    refused = ['a%d %d piece %d: %s may never be listed' % (a, k, i, v) for a in sorted(unplanted) for k in sorted(unplanted[a]) for i, v in sorted(unplanted[a][k].items()) if v in NEVER_KNOWN]
    rows = []
    for a in sorted(unplanted):
        for k in sorted(unplanted[a]):
            for i, v in sorted(unplanted[a][k].items()):
                if v in OK or v in NOT_JUDGED or v in NEVER_KNOWN:
                    continue
                metric = KNOWN_METRIC.get(v)
                pick = next(((vn, st) for vn, vv, st in views.get((a, k, i), []) if vv == v and metric and st.get(metric) is not None), None)
                if pick is None:
                    refused.append('a%d %d piece %d: %s has no view with a %s to pin (not written)' % (a, k, i, v, metric))
                    continue
                x = pick[1][metric]
                rows.append((a, k, i, v, pick[0], metric, round(max(0.0, x - KNOWN_MARGIN), 2), round(x + KNOWN_MARGIN, 2)))
    counts = {}
    for r in rows:
        counts[r[3]] = counts.get(r[3], 0) + 1
    L = ['# tools\\scene\\arena_sheet_known.txt: the OPEN findings of the arena sheet\'s screen check (docs\\agents\\arena_sheet.md "The known list"). Our own text.',
         '# A list of OPEN findings: it must shrink to zero. Strict mode (--both / --strict) passes a piece listed here only while its verdict is the same AND the listed view still has that verdict with its number',
         '# inside the band AND every other view of the piece is judged as usual (a new non-PASS view fails). A changed piece (PASS included: then remove it) fails, an unlisted finding fails, and BLACK / PASS-BACK /',
         '# NO PIXELS are never listed. A row without a view / band (the old six-field format) is refused. Regenerate: arena_sheet.py --both --write-known (band = measured +/- %.2f).' % KNOWN_MARGIN,
         '# Owners: DIM -> %s; PARTIAL -> %s; UNCLEAR (opaque) -> unowned; a row keeps the owner written here.' % (KNOWN_OWNER['DIM'], KNOWN_OWNER['PARTIAL']),
         '# Written %s: %s.' % (time.strftime('%Y-%m-%d %H:%M'), ', '.join('%s %d' % kv for kv in sorted(counts.items())) or 'none'),
         '# NOT listable (strict failures until fixed): %s' % ('; '.join(r.replace(' may never be listed', '') for r in refused) or 'none')]
    if grows:
        L.append('# grows: %s' % grows)   # tools\\scene\\known_growth_lint.py: a longer list than main's needs this line
    L.append('# arena key piece verdict class owner view metric lo hi')
    for a, k, i, v, vn, metric, lo, hi in rows:
        prev = old.get((a, k, i))   # a row already listed with the same verdict keeps its owner (a row can be owned by a renderer contract site, not only by its class)
        L.append('%d %d %d %s %s %s %s %s %.2f %.2f' % (a, k, i, v, v, prev[2] if prev and prev[0] == v else KNOWN_OWNER.get(v, 'unowned'), vn, metric, lo, hi))
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(L) + '\n')
    return refused


def strict_failures(unplanted, known=None, arenas=None, views=None):
    """--both / --strict on the unplanted arm {arena: {key: {piece: verdict}}} against the known list. Fails: BLACK / PASS-BACK / NO PIXELS always; any other finding not in the list
    (an EXPECT_PLANT key going BLACK in BOTH arms, a deleted TRN4 fix, fails here); a listed piece whose verdict changed, PASS included ("remove it from the list"); a listed piece of an
    arena that ran but has no such piece. PASS / FAINT / TOO FAINT / EDGE-ON / HIDDEN never fail. `arenas` = the arenas this run judged (list entries of other arenas are not checked).
    WGS6 round 2: a listed row pins ONE VIEW and a band for its number: the view must still have the listed verdict with the number inside the band, every other view of the piece must be PASS / FAINT /
    not judged (a new non-PASS view on a listed piece fails), and a row without a view / band (the old format) is refused. `views` = {(arena, key, piece): [(view, verdict, stats)]} (None: the
    row's view / band are not checked: units of the verdict logic only)."""
    known = known or {}
    ran = set(arenas) if arenas is not None else set(unplanted)
    out = []
    for a in sorted(unplanted):
        for k in sorted(unplanted[a]):
            for i, v in sorted(unplanted[a][k].items()):
                tag = ' (an EXPECT_PLANT key: must PASS without the plant)' if k in EXPECT_PLANT.get(a, set()) else ''
                kv = known.get((a, k, i))
                if v in NEVER_KNOWN:
                    out.append('a%d %d piece %s: %s without the plant%s (never accepted as known)' % (a, k, i, v, tag))
                elif kv is not None and kv[0] != v:
                    out.append('a%d %d piece %s: listed as %s, now %s%s' % (a, k, i, kv[0], v, ': remove it from the list' if v in OK or v in NOT_JUDGED else ': update the list or fix it'))
                elif kv is not None:
                    if kv[3] is None:
                        out.append('a%d %d piece %s: the known row has no view / band (the old format): refused, regenerate it with --write-known' % (a, k, i))
                    elif views is not None:
                        vs = views.get((a, k, i), [])
                        d = {vn: (vv, st) for vn, vv, st in vs}
                        if kv[3] not in d:
                            out.append('a%d %d piece %s: the listed view %s was not judged' % (a, k, i, kv[3]))
                        else:
                            vv, st = d[kv[3]]
                            x = st.get(kv[4])
                            if vv != kv[0]:
                                out.append('a%d %d piece %s: listed view %s is %s, listed as %s' % (a, k, i, kv[3], vv, kv[0]))
                            elif x is None or not (kv[5] <= x <= kv[6]):
                                out.append('a%d %d piece %s: view %s %s %s is outside the listed band %.2f-%.2f (a different cause, or a change)' % (a, k, i, kv[3], kv[4], 'missing' if x is None else '%.2f' % x, kv[5], kv[6]))
                        for vn, vv, st in vs:
                            if vn != kv[3] and vv not in OK and vv not in NOT_JUDGED:
                                out.append('a%d %d piece %s: a NEW non-PASS view on a listed piece: %s is %s' % (a, k, i, vn, vv))
                elif v in OK or v in NOT_JUDGED:
                    continue
                else:
                    out.append('a%d %d piece %s: %s without the plant: a NEW finding (not in the known list)%s' % (a, k, i, v, tag))
    for (a, k, i), kv in sorted(known.items()):
        if a in ran and i not in unplanted.get(a, {}).get(k, {}):
            out.append('a%d %d piece %d: listed as %s but the run has no such piece: remove it from the list' % (a, k, i, kv[0]))
    return out


KEYS_FILE = os.path.join(HERE, 'arena_sheet_keys.txt')


def load_keys(path=KEYS_FILE):
    """the committed manifest of the screen keys each match arena's files hold (our own text: `arena keys` with keys `800,801,...` or `none`): {arena: [keys]}"""
    out = {}
    try:
        with open(path, encoding='utf-8') as f:
            for ln in f:
                ln = ln.split('#')[0].strip()
                if ln:
                    a, ks = ln.split()[:2]
                    out[int(a)] = [] if ks == 'none' else [int(x) for x in ks.split(',')]
    except (OSError, ValueError):
        pass
    return out


def write_keys(results, path=KEYS_FILE):
    L = ['# tools\\scene\\arena_sheet_keys.txt: the screen keys 800-809 with a mesh in each match arena\'s files (arena_rec\'s every-key model), our own text.',
         '# Strict mode fails an arena that errors, loads fewer keys than this, or is missing here. Regenerate: arena_sheet.py --both --write-keys (all 49 arenas).',
         '# Written %s.' % time.strftime('%Y-%m-%d %H:%M'), '# arena keys']
    for r in sorted(results, key=lambda r: r['arena']):
        L.append('%d %s' % (r['arena'], ','.join(str(k) for k in sorted(r.get('mesh_keys', []))) or 'none'))
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(L) + '\n')


def run_failures(results, manifest):
    """an arena that errored (arena_rec failed, a driver exception), is missing from the keys manifest, or loaded a key set other than the manifest's is a strict failure:
    an arena crashing in both arms used to compare empty with empty and pass"""
    out = []
    for r in results:
        a = r['arena']
        for e in r.get('errors', []):
            out.append('a%d: ERROR %s' % (a, e))
        if a not in manifest:
            out.append('a%d: not in %s (regenerate with --write-keys)' % (a, os.path.basename(KEYS_FILE)))
            continue
        have, want = sorted(r.get('mesh_keys', [])), sorted(manifest[a])
        if have != want:
            out.append('a%d: keys with a mesh %s, the manifest says %s' % (a, ','.join(map(str, have)) or 'none', ','.join(map(str, want)) or 'none'))
    return out


def known_counts(unplanted, known):
    """{class: n} of the listed findings this run still shows unchanged"""
    c = {}
    for (a, k, i), kv in known.items():
        if unplanted.get(a, {}).get(k, {}).get(i) == kv[0]:
            c[kv[1]] = c.get(kv[1], 0) + 1
    return c


def compare_planted(planted, unplanted):
    """planted / unplanted: {arena: {key: {piece: verdict}}}. Returns the differences from the expected planted result: every piece of an EXPECT_PLANT key BLACK (EXPECT_NOT_PASS keys:
    anything but PASS / FAINT), everything else as unplanted."""
    diffs = []
    for a in sorted(set(planted) | set(unplanted)):
        p, u = planted.get(a, {}), unplanted.get(a, {})
        exp = EXPECT_PLANT.get(a, set())
        for k in sorted(set(p) | set(u) | exp):
            pk, uk = p.get(k, {}), u.get(k, {})
            if k in exp:
                if not pk:
                    diffs.append('a%d %d: expected BLACK with the plant, key not judged' % (a, k))
                not_pass = k in EXPECT_NOT_PASS.get(a, set())
                for i in sorted(pk):
                    if (not_pass and pk[i] in OK) or (not not_pass and pk[i] != 'BLACK'):
                        diffs.append('a%d %d piece %s: expected %s with the plant, got %s' % (a, k, i, 'not PASS' if not_pass else 'BLACK', pk[i]))
            else:
                for i in sorted(set(pk) | set(uk)):
                    if pk.get(i) != uk.get(i):
                        diffs.append('a%d %d piece %s: %s with the plant, %s without (expected unchanged)' % (a, k, i, pk.get(i), uk.get(i)))
    return diffs


def stamp_now(tool_paths):
    return {'th': TH, 'time': time.time(), 'tools_mtime': max((os.path.getmtime(p) for p in tool_paths if os.path.exists(p)), default=0)}


def stale_reason(js, arenas, tool_paths):
    """why a saved unplanted arm cannot be compared with (None = fresh)"""
    s = js.get('stamp')
    if not s:
        return 'it has no stamp (written before the fix round)'
    if s.get('th') != TH:
        return 'its thresholds differ from this run\'s'
    newest = max((os.path.getmtime(p) for p in tool_paths if os.path.exists(p)), default=0)
    if s.get('time', 0) < newest:
        return 'a tool (arena_sheet.py / arena_rec.exe / frame_rig.exe) is newer than it'
    missing = [a for a in arenas if str(a) not in js.get('verdicts', {})]
    if missing:
        return 'it lacks arenas %s' % ','.join(str(a) for a in missing)
    return None


def arena_names():
    try:
        with open(os.path.join(ROOT, 'db', 'ids', 'arenas.json'), encoding='utf-8') as f:
            return {int(e['id']): e.get('name', '?') for e in json.load(f)['entries']}
    except (OSError, ValueError, KeyError):
        return {}


def main_checkout():
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return env
    common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
    return os.path.normpath(os.path.dirname(common)) if common else ROOT


def stage_info(game, arena):
    r"""the ring canvas top (entry 953's highest vertex, y up) and the stage rect's centre (entry 50003, light 200), read from the arena pac (tools\assets\spots.py readers, as
    arena_heights.py). Returns (canvas_top, (x, z) or None, note)."""
    try:
        sys.path.insert(0, os.path.join(ROOT, 'tools', 'assets'))
        import spots
        d = spots.pac.load(os.path.join(game, 'pac', 'bg', 'bg%02d.pac' % arena))
        top = 120.0
        for k, blob in spots.pac.entries(d, 8):
            if k == 'p0/953':
                ys = [p[1] for m in spots.meshes(blob) for p in m]
                if ys:
                    top = -min(ys)
        st = None
        for r in spots.arena_rects(d):
            if r['light'] == 200:
                st = (0.5 * (r['x0'] + r['x1']), 0.5 * (r['z0'] + r['z1']))
                break
        return top, st, ''
    except Exception as ex:   # the readers are research tools: a failure gives the defaults, said in the report
        return 120.0, None, 'stage_info failed: %s' % ex


def font(size, bold=False):
    for name in (('arialbd.ttf' if bold else 'arial.ttf'), 'DejaVuSans.ttf'):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


KEYLINE_RE = re.compile(r'\[arena_rec\] screen_key (\d+) pieces (\d+) kept (\d+) dropped_area_share (\S+)')
PIECE_RE = re.compile(r'\[arena_rec\] piece (\d+) (\d+) area (\S+) tris (\d+) centroid (\S+) normal (\S+) weak (\d) blend (\d) vcol (\S+) view (\S+) expect_px (\d+) vp (\S+) eye (\S+)(?: hidden_px (\d+))?')
BEAUTY_RE = re.compile(r'\[arena_rec\] screen_keys (\S+) camera beauty look (\S+) stage (.+?) eye (\S+) aim (\S+) fov (\S+) dist (\S+)')


def parse_arena_rec(text):
    info = {'keys': [], 'pieces': {}, 'dropped': {}, 'beauty': '', 'stage': '?'}
    m = BEAUTY_RE.search(text)
    if m:
        info['keys'] = [] if m.group(1) == 'none' else [int(x) for x in m.group(1).split(',')]
        info['stage'] = m.group(3)
        info['beauty'] = 'stage %s eye %s aim %s fov %s dist %s' % (m.group(3), m.group(4), m.group(5), m.group(6), m.group(7))
    for m in KEYLINE_RE.finditer(text):
        info['dropped'][int(m.group(1))] = (int(m.group(2)), int(m.group(3)), float(m.group(4)))
    for m in PIECE_RE.finditer(text):
        key = (int(m.group(1)), int(m.group(2)))
        p = info['pieces'].setdefault(key, {'key': key[0], 'index': key[1], 'area': float(m.group(3)), 'tris': int(m.group(4)), 'centroid': m.group(5), 'normal': m.group(6),
                                              'weak': int(m.group(7)), 'blend': int(m.group(8)), 'vcol': m.group(9), 'views': {}})
        p['views'][m.group(10)] = {'expect': int(m.group(11)), 'vp': m.group(12), 'eye': m.group(13), 'hidden': int(m.group(14) or 0)}
    return info


def run_arena_rec(exe, game, arena, look, out_rsc, canvas_top, stage_xz, fp_dir=None):
    cmd = [exe, '--game', game, '--arena', str(arena), '--from-provider', '--scene-word', str(SCENE_WORD[look]), '--camera-from', 'none', '--camera-screens', look,
           '--canvas-top', '%g' % canvas_top, '--out', out_rsc]
    if stage_xz:
        cmd += ['--stage-xz', '%g,%g' % stage_xz]
    if fp_dir:
        os.makedirs(fp_dir, exist_ok=True)
        cmd += ['--screen-pieces', '--check-size', CHECK_SIZE, '--footprints', fp_dir]
    t0 = time.time()
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=600)
    text = (p.stdout or '') + (p.stderr or '')
    info = parse_arena_rec(text)
    info.update({'rc': p.returncode, 'seconds': time.time() - t0, 'text': text})
    return info


def parse_report(text):
    """the counters read from frame_rig's report (frame_rig.cpp: the panel / family lines). Holes are split: rig_input = the offline rig's missing stage-look rows (RIG_INPUT_TEXT, a limit of
    the offline record, not a fault), screen_holes, other_holes."""
    r = {'empty': 'EMPTY: nothing was drawn' in text, 'validation': 0, 'covered': 0, 'holes': 0, 'screen_holes': 0, 'rig_input': 0, 'other_holes': 0, 'why': '', 'screens_objects': 0,
         'screens_drawn': 0, 'hole_why': [], 'families': {}}
    m = re.search(r'panel \S+ families \S+ objects_drawn \d+ front_end_objects \d+ covered_pixels (\d+) of \d+ colours \d+ validation_errors (\d+)', text)
    if m:
        r['covered'], r['validation'] = int(m.group(1)), int(m.group(2))
    for m in re.finditer(r'  family (\S+) objects (\d+) drawn (\d+) skipped (\d+) holes (\d+) why (.*)', text):
        fam, holes, why = m.group(1), int(m.group(5)), m.group(6).strip()
        r['families'][fam] = (int(m.group(2)), int(m.group(3)), holes)
        r['holes'] += holes
        if holes:
            r['hole_why'].append('%s %d: %s' % (fam, holes, why[:220]))
        if fam == 'screens':
            r['why'], r['screens_objects'], r['screens_drawn'], r['screen_holes'] = why, int(m.group(2)), int(m.group(3)), holes
        elif RIG_INPUT_TEXT in why:
            r['rig_input'] += holes
        else:
            r['other_holes'] += holes
    return r


def load_rgb(path):
    with Image.open(path) as im:
        return np.asarray(im.convert('RGB'))


class Arm:
    def __init__(self, args, plant):
        sys.path.insert(0, HERE)
        import frame_rig   # RigSession: load once, draw many (one serve process per environment)
        self.plant = plant
        self.args = args
        self.session = frame_rig.RigSession()
        self.env = dict(os.environ)
        for k in [k for k in self.env if k.startswith('WWE_RDR_PLANT_')]:
            self.env.pop(k)
        self.env['WWE_RDR_RIG_SCREEN_ROWS'] = '1'
        self.env['WWE_RDR_RIG_SCREEN_FEED'] = 'pattern'
        if plant:
            self.env['WWE_RDR_PLANT_SCREEN_HIGHLIGHT'] = '1'
        self.rig_exe = os.path.join(ROOT, 'out', 'tools', 'frame_rig.exe')
        self.memory = None

    def rig(self, record, prefix, size, show, entrance, families=None, camera=None, eye=None, flat=False):
        cmd = [self.rig_exe, '--record', record, '--game', self.args.game, '--size', size, '--prefix', prefix, '--show', ','.join(str(k) for k in show) if show else 'none']
        if entrance:
            cmd.append('--look-entrance')
        if flat:
            cmd.append('--flat-feed')   # DIM1: one solid colour per key, for the brightness judge
        if 800 in show and families == 0x20 and len(show) == 1:
            cmd += ['--prime-grab', '800']   # PAR1: key 800 IS the frame grab in every look (front_end.cpp; the game's channel -1): the check primes the grab with 800's test picture
        if families is not None:
            cmd += ['--families', hex(families)]
        if camera:
            cmd += ['--camera', camera, '--eye', eye]
        t0 = time.time()
        p = self.session.rig(cmd, env=self.env, timeout=900)
        text = ''
        if os.path.exists(prefix + '_report.txt'):
            with open(prefix + '_report.txt', encoding='utf-8', errors='replace') as f:
                text = f.read()
        return p.returncode, time.time() - t0, text, (p.stdout or '') + (p.stderr or '')

    def read_memory(self):
        """one nvidia-smi read while the first arena is resident (a read, no load)"""
        try:
            p = subprocess.run(['nvidia-smi', '--query-compute-apps=pid,used_memory', '--format=csv'], capture_output=True, text=True, timeout=30)
            pids = {str(s.pid) for s in self.session.servers.values()}
            lines = [ln for ln in (p.stdout or '').splitlines() if ln.split(',')[0].strip() in pids]
            self.memory = ('pid,used_memory: ' + lines[0].strip() + ' (rig serve process)') if lines else ('no line for the rig process; nvidia-smi said: ' + ' | '.join((p.stdout or '').splitlines()[:6]))
            if not lines or 'N/A' in lines[0]:   # Windows (WDDM) gives no per-process figure: the whole card's use, every process on the PC included
                g = subprocess.run(['nvidia-smi', '--query-gpu=memory.used,memory.total', '--format=csv,noheader'], capture_output=True, text=True, timeout=30)
                self.memory += '; per-process N/A on WDDM, whole card memory.used,total = %s (all processes on the PC)' % (g.stdout or '').strip()
        except (OSError, subprocess.SubprocessError) as ex:
            self.memory = 'nvidia-smi failed: %s' % ex

    def judge_view(self, rsc, prefix, key, view, fp_path, blended=False):
        rc, sec, _, out = self.rig(rsc, prefix, CHECK_SIZE, [key], True, families=0x20, camera=view['vp'], eye=view['eye'])
        png = prefix + '_mask32.png'
        if rc != 0 or not os.path.exists(png):
            return 'NO PIXELS', {'px': 0, 'expect': view['expect'], 'coverage': 0.0, 'error': 'rig rc %d %s' % (rc, (out.strip().splitlines() or ['?'])[-1][:160])}, None, sec
        img = load_rgb(png)
        fp = read_footprint(fp_path, img.shape[1], img.shape[0]) if os.path.exists(fp_path) else None
        flat = vc = va = None
        if fp is not None:   # DIM1: the same view with the flat feed, and the interpolated vertex colour and alpha, for the brightness judge
            rc2, sec2, _, _ = self.rig(rsc, prefix + '_flat', CHECK_SIZE, [key], True, families=0x20, camera=view['vp'], eye=view['eye'], flat=True)
            sec += sec2
            vpath, apath = fp_path[:-3] + '.vc', fp_path[:-3] + '.va'
            if rc2 == 0 and os.path.exists(prefix + '_flat_mask32.png') and os.path.exists(vpath) and os.path.exists(apath):
                flat = load_rgb(prefix + '_flat_mask32.png')
                vc = read_vc(vpath, img.shape[1], img.shape[0])
                va = read_va(apath, img.shape[1], img.shape[0])
        v, s = judge_piece(img, key, view['expect'] or None, fp, blended=blended, flat=flat, vc=vc, va=va)
        v = apply_hidden(v, s, view)
        if fp is None and fp_path:
            s['error'] = 'no footprint file: every drawn pixel judged'
        elif fp is not None and flat is None:
            s['error'] = 'no flat render / vertex colour: brightness judged on the pattern (a + g)'
        return v, s, png, sec

    def do(self, arena, work, looks):
        res = {'arena': arena, 'pieces': {}, 'views': {}, 'images': {}, 'notes': [], 'times': {'checks': []}, 'beauty': {}, 'errors': [], 'rig_keys': {}, 'mesh_keys': [],
               'piece_info': {}, 'dropped': {}}
        top, st, note = stage_info(self.args.game, arena)
        res['canvas_top'], res['stage_xz'] = top, st
        if note:
            res['notes'].append(note)
        exe = os.path.join(ROOT, 'out', 'tools', 'arena_rec.exe')
        recs = {}
        for look in ('entrance', 'match'):
            rsc = os.path.join(work, 'a%02d_%s.rsc' % (arena, look))
            info = run_arena_rec(exe, self.args.game, arena, look, rsc, top, st, os.path.join(work, 'fp_a%02d' % arena) if look == 'entrance' else None)
            res['times']['arena_rec_' + look] = info['seconds']
            if info['rc'] != 0:
                res['errors'].append('arena_rec %s rc %d: %s' % (look, info['rc'], (info['text'].strip().splitlines() or ['?'])[-1]))
                continue
            recs[look] = (rsc, info)
        if 'entrance' not in recs:
            return res
        info = recs['entrance'][1]
        res['mesh_keys'], res['piece_info'], res['dropped'] = info['keys'], info['pieces'], info['dropped']
        rsc_e = recs['entrance'][0]
        # the CHECK pass: every piece, every view, front-on, screens alone, the key alone, the entrance stage look
        fp_dir = os.path.join(work, 'fp_a%02d' % arena)

        def vfile(k, i, vn):
            return 'k%d_p%d_%s' % (k, i, vn.replace('+', 'p').replace('-', 'm'))
        for (k, i), p in sorted(info['pieces'].items()):
            judged = []
            if edge_on(p['views']):   # no camera-facing pixel from any view: EDGE-ON (not judged, never a pass)
                res['pieces'].setdefault(k, {})[i] = 'EDGE-ON'
                res['views'][(k, i)] = [('all', 'EDGE-ON', {'px': 0, 'expect': 0, 'coverage': None})]
                continue
            names = ['x+', 'x-', 'z+', 'z-'] if p['weak'] else ['front']
            for vn in names:
                if vn not in p['views']:
                    continue
                pv = p['views'][vn]
                if pv['expect'] == 0 and pv.get('hidden', 0) > 0:   # WGS6 round 2: every camera-facing pixel of this view is hidden by earlier nearer pieces: HIDDEN, nothing to render
                    judged.append((vn, 'HIDDEN', {'px': 0, 'expect': 0, 'hidden': pv['hidden'], 'hidden_share': 1.0, 'coverage': None}))
                    continue
                if p['weak'] and pv['expect'] == 0:   # none of a closed piece's triangles face this side: nothing to judge from it
                    continue
                v, s, png, sec = self.judge_view(rsc_e, os.path.join(work, 'a%02d_%s' % (arena, vfile(k, i, vn))), k, p['views'][vn], os.path.join(fp_dir, vfile(k, i, vn) + '.fp'), bool(p.get('blend')))
                res['times']['checks'].append(sec)
                judged.append((vn, v, s))
                if png and (k, i) not in res['images']:
                    res['images'][(k, i)] = png
            if judged and all(v == 'HIDDEN' for _, v, _ in judged) and 'alt' in p['views']:   # WGS6 round 2: hidden from every camera: one more camera (arena_rec's best of a ring of directions)
                v, s, png, sec = self.judge_view(rsc_e, os.path.join(work, 'a%02d_%s' % (arena, vfile(k, i, 'alt'))), k, p['views']['alt'], os.path.join(fp_dir, vfile(k, i, 'alt') + '.fp'), bool(p.get('blend')))
                res['times']['checks'].append(sec)
                judged.append(('alt', v, s))
                if png and (k, i) not in res['images']:
                    res['images'][(k, i)] = png
            if not p['weak'] and judged and judged[0][1] == 'NO PIXELS' and 'back' in p['views']:   # the front drew nothing: once from behind (a drawn back side = PASS-BACK)
                # the back view's footprint holds the triangles facing THAT camera (none, for a one-sided screen): judge every drawn pixel there instead
                v, s, png, sec = self.judge_view(rsc_e, os.path.join(work, 'a%02d_%s' % (arena, vfile(k, i, 'back'))), k, dict(p['views']['back'], expect=p['views']['front']['expect']), '', bool(p.get('blend')))
                res['times']['checks'].append(sec)
                judged.append(('back', v, s))
                if png and v != 'NO PIXELS':
                    res['images'][(k, i)] = png
            res['pieces'].setdefault(k, {})[i] = piece_verdict(judged)
            res['views'][(k, i)] = judged
            if self.memory is None:
                self.read_memory()
        # PAR1: a kept grab must change nothing: key 800's largest piece judged again in the same serve process (after other jobs) gives the same verdict and pixel count
        p0 = info['pieces'].get((800, 0))
        if p0 and not p0['weak'] and 'front' in p0['views'] and res['views'].get((800, 0)):
            v2, s2, _, sec = self.judge_view(rsc_e, os.path.join(work, 'a%02d_%s_again' % (arena, vfile(800, 0, 'front'))), 800, p0['views']['front'],
                                             os.path.join(fp_dir, vfile(800, 0, 'front') + '.fp'), bool(p0.get('blend')))
            res['times']['checks'].append(sec)
            first = [x for x in res['views'][(800, 0)] if x[0] == 'front']
            if first:
                res['grab_repeat'] = grab_repeat_result(first[0][1], first[0][2], v2, s2)
        # E: which keys the rig's own (live loader's) model has: every key shown, screens alone, rows for every key
        rc, sec, text, out = self.rig(rsc_e, os.path.join(work, 'a%02d_allkeys' % arena), CHECK_SIZE, KEYS, True, families=0x20)
        c = parse_report(text)
        res['rig_keys'] = {'objects': c['screens_objects'], 'drawn': c['screens_drawn'], 'why': c['why'], 'covered': c['covered'], 'rc': rc}
        # the BEAUTY tiles
        for look in looks:
            if look not in recs:
                continue
            rsc, li = recs[look]
            show = [k for k in LOOK_SET[look] if k in li['keys']]
            art = [303] if (801 in li['keys'] or 802 in li['keys']) else [301, 302, 303, 304]   # LegacyShow's art rule
            prefix = os.path.join(work, 'a%02d_%s' % (arena, look))
            rc, sec, text, out = self.rig(rsc, prefix, BEAUTY_SIZE, show + art, look == 'entrance')
            res['times']['beauty_' + look] = sec
            b = {'show': show, 'camera': li['beauty'], 'stage': li.get('stage', '?'), 'counters': parse_report(text)}
            if rc == 0 and os.path.exists(prefix + '_whole.png'):
                b['image'] = prefix + '_whole.png'
            else:
                b['error'] = 'frame_rig rc %d: %s' % (rc, (out.strip().splitlines() or ['?'])[-1][:200])
            res['beauty'][look] = b
        return res


def arena_fault(r):
    return bool(r.get('errors')) or any(v not in OK and v not in NOT_JUDGED for ps in r.get('pieces', {}).values() for v in ps.values())


def key_known(r, k, known):
    """True when every failing piece of key k in arena r is in the known list with the same verdict (the sheets add a small 'known' mark; colour and label stay)"""
    bad = [(i, v) for i, v in r.get('pieces', {}).get(k, {}).items() if v not in OK and v not in NOT_JUDGED]
    return bool(bad) and all(known.get((r['arena'], k, i), ('',))[0] == v for i, v in bad)


def key_label(r, k, look):
    if k not in r.get('mesh_keys', []):
        return 'no mesh'
    return key_verdict(r.get('pieces', {}).get(k, {}))   # PAR1: key 800 is the frame grab in both looks; the check judges it through the primed grab, so both looks carry its verdict


def beauty_tile(r, look, names, plant, known=None, w=480, h=270):
    known = known or {}
    band = 118
    im = Image.new('RGB', (w, h + band), (18, 18, 22))
    b = r.get('beauty', {}).get(look, {})
    c = b.get('counters', {})
    if b.get('image'):
        with Image.open(b['image']) as src:
            im.paste(src.convert('RGB').resize((w, h), Image.LANCZOS), (0, 0))
    else:
        msg = b.get('error') or ('; '.join(r.get('errors', [])) or 'not drawn')
        ImageDraw.Draw(im).text((12, h // 2 - 10), msg[:60], fill=(255, 90, 90), font=font(15, True))
    d = ImageDraw.Draw(im)
    a = r['arena']
    d.text((8, h + 4), ('a%d  %s' % (a, names.get(a, '?')))[:40], fill=(255, 255, 255), font=font(19, True))
    stage = b.get('stage', '?')
    d.text((8, h + 28), '%s look%s; shows %s (TOLD)%s' % (look.upper(), ', PLANT' if plant else '', ' '.join(str(k) for k in b.get('show', [])) or 'none',
                                                        '; no stage found' if stage == 'no stage found' else ''), fill=(200, 200, 200), font=font(14))
    x, y = 8, h + 48
    fv = font(14, True)
    keys = r.get('mesh_keys', [])
    if not keys:
        d.text((x, y), "no screen key 800-809 in this arena's files" if not r.get('rig_keys', {}).get('objects') else 'screens in the rig but not in arena_rec: see report', fill=(200, 200, 200), font=fv)
    else:
        for k in LOOK_SET[look]:
            v = key_label(r, k, look)
            s = '%d %s' % (k, v + (' (match frame grab)' if v == 'not checked' else '') + (' (known)' if (not plant and v != 'not checked' and key_known(r, k, known)) else ''))
            tw = d.textlength(s, font=fv) + 14
            if x + tw > w - 4:
                x, y = 8, y + 20
            d.text((x, y), s, fill=VERDICT_COLOUR.get(v, (255, 255, 255)), font=fv)
            x += tw
    others = [k for k in keys if k not in LOOK_SET[look] and key_verdict(r.get('pieces', {}).get(k, {})) not in OK]
    if others:
        d.text((8, y + 20), 'other keys failing the check: ' + ' '.join(str(k) for k in others), fill=(255, 90, 90), font=font(13))
    badges = []
    if c.get('screen_holes'):
        badges.append(('screen holes %d' % c['screen_holes'], (255, 90, 90)))
    if c.get('other_holes'):
        badges.append(('other holes %d' % c['other_holes'], (255, 160, 40)))
    if c.get('rig_input'):
        badges.append(('rig input missing %d' % c['rig_input'], (150, 150, 150)))
    if c.get('validation'):
        badges.append(('validation %d' % c['validation'], (255, 90, 90)))
    bx = w - 8
    for t, col in reversed(badges):
        bx -= d.textlength(t, font=font(13, True)) + 10
        d.text((bx, h + 6), t, fill=col, font=font(13, True))
    if arena_fault(r) or c.get('screen_holes') or c.get('validation') or c.get('empty'):
        d.rectangle([0, 0, w - 1, h + band - 1], outline=(255, 40, 40), width=6)
    elif c.get('other_holes'):
        d.rectangle([0, 0, w - 1, h + band - 1], outline=(255, 160, 40), width=4)
    return im


def beauty_sheet(results, names, look, plant, path, known=None, cols=7):
    tiles = [beauty_tile(r, look, names, plant, known) for r in results]
    if not tiles:
        return
    tw, th = tiles[0].size
    head = 56
    rows = (len(tiles) + cols - 1) // cols
    sh = Image.new('RGB', (cols * tw + (cols + 1) * 6, head + rows * th + (rows + 1) * 6), (8, 8, 10))
    d = ImageDraw.Draw(sh)
    ok = sum(1 for r in results if not arena_fault(r) and r.get('mesh_keys'))
    d.text((10, 8), 'Every match arena, %s look%s: our renderer, offline, from the files; a test pattern on every screen (the view is for the eyes only)' % (look.upper(), ' - PLANTED (TRN4 fix undone)' if plant else ''), fill=(255, 255, 255), font=font(24, True))
    d.text((10, 34), 'Labels = the screen CHECK (front-on, screens alone; screens_check.png): %d of %d arenas with screens pass every key.  Red border = a key fails the check; orange = other holes; grey "rig input missing" = the offline record has no stage-look rows (not a fault).%s' % (
        ok, len(results), '  Key 800 = the frame grab (both looks): checked through the primed grab.'), fill=(200, 200, 200), font=font(15))
    for i, t in enumerate(tiles):
        r, c = divmod(i, cols)
        sh.paste(t, (6 + c * (tw + 6), head + 6 + r * (th + 6)))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    sh.save(path)


def check_sheet(results, names, plant, path, known=None, cw=168, ch=95):
    """rows = arenas, columns = keys 800-809; each cell the front-on render of that key's largest piece, a border in the key's (worst) verdict colour; grey = no mesh.
    A key whose every failing piece is in the known list is drawn exactly as a new finding, plus a small 'known' mark."""
    known = known or {}
    lw, head = 250, 70
    sh = Image.new('RGB', (lw + 10 * (cw + 4) + 8, head + len(results) * (ch + 4) + 8), (10, 10, 12))
    d = ImageDraw.Draw(sh)
    allv = [v for r in results for ps in r.get('pieces', {}).values() for v in ps.values()]
    d.text((8, 6), 'Screen check%s: every screen key of every match arena, front-on, screens alone, the test pattern fed' % (' - PLANTED (TRN4 fix undone)' if plant else ''), fill=(255, 255, 255), font=font(22, True))
    d.text((8, 32), '%d of %d pieces PASS (FAINT %d).  Cell = the key\'s largest piece (+N = more pieces, worst shown).  Green PASS, teal FAINT, red BLACK, dark orange DIM, yellow PARTIAL, pink UNCLEAR, orange NO PIXELS, blue PASS-BACK, grey TOO FAINT / EDGE-ON (not judged), grey cell = no mesh; "known" = in the open-findings list.' % (
        sum(1 for v in allv if v in OK), len(allv), sum(1 for v in allv if v == 'FAINT')), fill=(200, 200, 200), font=font(14))
    fk = font(15, True)
    for j, k in enumerate(KEYS):
        x = lw + j * (cw + 4)
        d.rectangle([x, head - 18, x + 14, head - 4], fill=PATTERN[k])
        d.text((x + 20, head - 20), str(k), fill=(255, 255, 255), font=fk)
    fs = font(12, True)
    for i, r in enumerate(results):
        y = head + i * (ch + 4)
        a = r['arena']
        d.text((8, y + ch // 2 - 18), 'a%d' % a, fill=(255, 255, 255), font=font(17, True))
        d.text((8, y + ch // 2 + 2), names.get(a, '?')[:28], fill=(190, 190, 190), font=font(13))
        for j, k in enumerate(KEYS):
            x = lw + j * (cw + 4)
            ps = r.get('pieces', {}).get(k)
            if not ps:
                d.rectangle([x, y, x + cw - 1, y + ch - 1], fill=(55, 55, 58))
                continue
            png = r.get('images', {}).get((k, 0))
            if png and os.path.exists(png):
                with Image.open(png) as src:
                    sh.paste(src.convert('RGB').resize((cw, ch), Image.LANCZOS), (x, y))
            v0, wv = ps.get(0, key_verdict(ps)), key_verdict(ps)
            col = VERDICT_COLOUR.get(wv, (255, 255, 255))
            d.rectangle([x, y, x + cw - 1, y + ch - 1], outline=col, width=4)
            t = v0 + ((' +%d' % (len(ps) - 1)) + ((' ' + wv) if wv != v0 else '') if len(ps) > 1 else '') + (' known' if (not plant and key_known(r, k, known)) else '')
            d.rectangle([x + 4, y + ch - 20, x + 8 + d.textlength(t, font=fs), y + ch - 5], fill=(0, 0, 0))
            d.text((x + 6, y + ch - 19), t, fill=col, font=fs)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    sh.save(path)


def fmt_stats(s):
    cov = ('%.2f' % s['coverage']) if s.get('coverage') is not None else '-'
    oth = (' other %.2f rgb %s' % (s.get('other_share', 0), s['other_rgb'])) if s.get('other_rgb') else ''
    fl = (' flat %.2f (p5 %.2f p95 %.2f, signal %.0f lv)' % (s['flat_ratio'], s['flat_p5'], s['flat_p95'], s.get('signal', 0))) if s.get('flat_ratio') is not None else ''
    hid = (' hidden %d' % s['hidden']) if s.get('hidden') else ''
    if s.get('flat_hue') is not None:
        fl_hue = ' hue-misfit %.2f' % s['flat_hue']
    else:
        fl_hue = ''
    return 'px %d expect %s%s cov %s pattern %.3f dark %.3f a %.2f g %.2f a+g %.2f%s%s%s' % (s.get('px', 0), s.get('expect'), hid, cov, s.get('pattern', 0), s.get('dark', 0), s.get('a_med', 0), s.get('g_med', 0),
                                                                                              s.get('ag_med', 0), fl + fl_hue, oth, (' ' + s['error']) if s.get('error') else '')


def report_lines(results, names, plant, args, memory, total_s, compare, strict=None, others=None, known=None):
    """the report of one arm. `others` = the other arm's results (for the a23 / a22 line); never raises on a partial result (a crashed arena keeps its errors)."""
    L = []
    L.append('arena_sheet (WGS) %s arm, %s, check renders %s, beauty %s, game %s' % ('PLANTED (WWE_RDR_PLANT_SCREEN_HIGHLIGHT=1)' if plant else 'unplanted', time.strftime('%Y-%m-%d %H:%M'), CHECK_SIZE, BEAUTY_SIZE, args.game))
    L.append('thresholds: ' + ' '.join('%s=%g' % kv for kv in TH.items()))
    L.append('check pass: every piece (the 32 largest per key), front-on (weak pieces: 4 horizontal views), --families 0x20, --show K, --look-entrance; brightness on a --flat-feed render (DIM1).')
    L.append('  Key 800 is the frame grab in every look (PAR1): its jobs prime the grab with 800\'s test picture (--prime-grab 800) and draw no seed frame; every served job drops the grab first.')
    L.append('  Not covered: docs\\agents\\arena_sheet.md "Not covered".')
    L.append('')
    allp = [(r['arena'], k, i, v) for r in results for k, ps in sorted(r.get('pieces', {}).items()) for i, v in sorted(ps.items())]
    counts = {}
    for _, _, _, v in allp:
        counts[v] = counts.get(v, 0) + 1
    nkeys = sum(len(r.get('pieces', {})) for r in results)
    pass_low_a = sum(1 for r in results for (k, i), vs in r.get('views', {}).items() if r['pieces'].get(k, {}).get(i) in OK and any(s.get('a_med', 1) < 0.5 for _, vv, s in vs if vv in OK))
    L.append('CHECK PASS SUMMARY: %d arenas, %d keys with a mesh, %d pieces: %s' % (len(results), nkeys, len(allp), ', '.join('%s %d' % kv for kv in sorted(counts.items()))))
    L.append('  PASS / FAINT pieces with median a < 0.5 (a see-through copy or a dim picture: eyes there): %d; PASS-BACK (drawn only from behind): %d' % (pass_low_a, counts.get('PASS-BACK', 0)))
    L.append('  arenas whose every piece is PASS / FAINT: %d of %d with screens' % (sum(1 for r in results if r.get('pieces') and not arena_fault(r)), sum(1 for r in results if r.get('pieces'))))
    L.append('  NOT JUDGED (contract rule 5; never a pass, never a strict failure): TOO FAINT %d (a see-through copy below a = %.2f that fails the hue test), EDGE-ON %d (no camera-facing pixel from any view), HIDDEN %d (no view of the piece is judged: every camera has more than %d %% of its pixels hidden by earlier nearer pieces of the key)' % (
        counts.get('TOO FAINT', 0), TH['too_faint_a'], counts.get('EDGE-ON', 0), counts.get('HIDDEN', 0), int(100 * TH['hidden_share'])))
    hid_views = [(r['arena'], k, i, vn, vv, st) for r in results for (k, i), vs in sorted(r.get('views', {}).items()) for vn, vv, st in vs if st.get('hidden_share', 0) > TH['hidden_share']]
    hid_pieces = {(a, k, i) for a, k, i, _, _, _ in hid_views}
    no_view = [(a, k, i) for a, k, i in sorted(hid_pieces) if next(r for r in results if r['arena'] == a)['pieces'].get(k, {}).get(i) == 'HIDDEN']
    L.append('  HIDDEN VIEWS (WGS6 round 2: a view with more than %d %% of its pixels hidden by an earlier, nearer piece of its key is not judged; one line per view): %d views of %d pieces; %d pieces end HIDDEN with no judged view at all%s' % (
        int(100 * TH['hidden_share']), len(hid_views), len(hid_pieces), len(no_view), (': ' + ', '.join('a%d %d piece %d' % x for x in no_view)) if no_view else ' (the target is 0)'))
    for a, k, i, vn, vv, st in hid_views:
        L.append('    a%d %s %d piece %d %s: %s, %.1f %% hidden (%d visible px, %d hidden px)%s' % (a, names.get(a, '?'), k, i, vn, vv, 100 * st['hidden_share'], st.get('expect') or 0, st.get('hidden', 0),
                 (', would have been %s' % st['hidden_was']) if st.get('hidden_was') else ''))
    if strict is not None:
        kc = known_counts(as_ints(verdict_table(results)), known or {}) if not plant else {}
        nk = sum(kc.values())
        L.append('  KNOWN LIST (%s): %d open findings known (%s), %d new / changed' % (os.path.relpath(KNOWN_FILE, ROOT), nk, ', '.join('%s %d' % (c, kc.get(c, 0)) for c in ('DIM', 'PARTIAL', 'UNCLEAR')), len(strict)))
        L.append('  STRICT (--both / --strict): %s' % ('no new finding: pass' if not strict else '%d piece(s) fail: exit 1' % len(strict)))
        for s in strict:
            L.append('    ' + s)
    hv = [(r['arena'], k, i, vn, s['hidden']) for r in results for (k, i), vs in r.get('views', {}).items() for vn, _, s in vs if s.get('hidden')]
    L.append('  OCCLUSION (WGS6: a piece is judged where it is drawn; pixels an earlier, nearer piece of the same key covers are left out of its footprint): %d views of %d pieces have hidden pixels, %d px in all%s' % (
        len(hv), len({(a, k, i) for a, k, i, _, _ in hv}), sum(h for *_, h in hv), ('; most: ' + ', '.join('a%d %d piece %d %s %d px' % x for x in sorted(hv, key=lambda x: -x[4])[:5])) if hv else ''))
    gr = [(r['arena'], r['grab_repeat']) for r in results if r.get('grab_repeat')]
    L.append('  GRAB CARRY-OVER (PAR1: key 800 piece 0 judged again later in the same serve process): %d of %d the same%s' % (sum(1 for _, g in gr if g[0]), len(gr),
             ''.join('; a%d DIFFERS: %s' % (a, g[1]) for a, g in gr if not g[0])))
    opaque_unclear = [(r['arena'], k, i, r['views'].get((k, i), [])) for r in results for k, ps in sorted(r.get('pieces', {}).items()) for i, v in sorted(ps.items())
                      if v == 'UNCLEAR' and not r.get('piece_info', {}).get((k, i), {}).get('blend', 0)]
    L.append('  OPAQUE UNCLEAR (unowned findings, one by one): %d' % len(opaque_unclear))
    for a, k, i, vs in opaque_unclear:
        L.append('    a%d %s %d piece %d: %s' % (a, names.get(a, '?'), k, i, '; '.join('%s %s %s' % (vn, vv, fmt_stats(s)) for vn, vv, s in vs)))
    for r in results:
        bad = []
        for k, ps in sorted(r.get('pieces', {}).items()):
            for i, v in sorted(ps.items()):
                if v not in OK and v not in NOT_JUDGED:
                    vs = r['views'].get((k, i), [])
                    bad.append('%d piece %d %s [%s]' % (k, i, v, '; '.join('%s %s %s' % (vn, vv, fmt_stats(s)) for vn, vv, s in vs)))
        bad += ['ERROR ' + e for e in r.get('errors', [])]
        if bad:
            L.append('  FINDING a%d %s: %s' % (r['arena'], names.get(r['arena'], '?'), ' | '.join(bad)))
    L.append('')
    L.append('DROPPED PIECES (a key with more than 32 pieces: the 32 largest are judged)')
    nd = 0
    for r in results:
        for k, (n, kept, share) in sorted(r.get('dropped', {}).items()):
            if n > kept:
                nd += 1
                L.append('  a%d %d: %d pieces, %d judged, %d dropped = %.2f %% of the key\'s area' % (r['arena'], k, n, kept, n - kept, 100 * share))
    if not nd:
        L.append('  none')
    L.append('')
    L.append('THE CHECK AS A TABLE (largest piece; worst of the pieces in brackets when different)')
    L.append('  arena  ' + ' '.join('%-9d' % k for k in KEYS))
    ab = {'PASS': 'PASS', 'FAINT': 'FAINT', 'BLACK': 'BLACK', 'UNCLEAR': 'UNCL', 'NO PIXELS': 'NOPX', 'DIM': 'DIM', 'PARTIAL': 'PART', 'PASS-BACK': 'BACK', 'TOO FAINT': 'TFNT', 'EDGE-ON': 'EDGE'}
    for r in results:
        cells = []
        for k in KEYS:
            ps = r.get('pieces', {}).get(k)
            if not ps:
                cells.append('-')
                continue
            v0, w = ab.get(ps.get(0, '?'), '?'), ab.get(key_verdict(ps), '?')
            cells.append(v0 if v0 == w else '%s(%s)' % (v0, w))
        L.append('  a%-5d ' % r['arena'] + ' '.join('%-9s' % c for c in cells))
    L.append('')
    L.append('NON-PATTERN PIXELS IN PASS / FAINT VIEWS: median colour of the drawn pixels that are not the pattern (per view, the 12 with the largest share)')
    rows = []
    for r in results:
        for (k, i), vs in r.get('views', {}).items():
            for vn, vv, s in vs:
                if vv in OK and s.get('other_rgb'):
                    rows.append((s['other_share'], r['arena'], k, i, vn, s))
    rows.sort(reverse=True)
    for sh, a, k, i, vn, s in rows[:12]:
        L.append('  a%d %d piece %d %s: other %.2f median rgb %s (pattern %.3f, a %.2f, a+g %.2f)' % (a, k, i, vn, sh, s['other_rgb'], s['pattern'], s['a_med'], s['ag_med']))
    if rows:
        allrgb = np.asarray([s['other_rgb'] for _, _, _, _, _, s in rows], dtype=np.float64)
        L.append('  over all %d PASS / FAINT views with any: median of the medians rgb %s; the clear colour is %s' % (len(rows), tuple(int(v) for v in np.median(allrgb, axis=0)), CLEAR))
    L.append('')
    L.append('NO SCREEN KEYS / MODEL SOURCES (E): the keys arena_rec finds (the every-key viewer model) against the rig\'s own load (the live loader; every key shown, a row for every key).')
    L.append('  The rig\'s screens family counts one object per key it holds, 805 twice (it is in both screen models, stage_show.h ScreenCull); its why text names only the FIRST key with no mesh.')
    same = []
    for r in results:
        rk = r.get('rig_keys') or {}
        mine = r.get('mesh_keys', [])
        want = len(mine) + (1 if 805 in mine else 0)
        if not rk:
            L.append('  a%d %s: the rig was not asked (an earlier step failed)' % (r['arena'], names.get(r['arena'], '?')))
        elif not mine and rk.get('objects', 0) == 0 and 'has no stage screens' in rk.get('why', ''):
            L.append('  a%d %s: no screen keys: arena_rec none; the rig\'s own load says "%s" (both: the files hold no key 800-809)' % (r['arena'], names.get(r['arena'], '?'), rk.get('why', '')))
        elif rk.get('objects', 0) != want:
            L.append('  FINDING a%d %s: arena_rec keys %s (expects %d screen objects) but the rig holds %d (drawn %d); rig why: %s' % (
                r['arena'], names.get(r['arena'], '?'), ','.join(str(k) for k in mine) or 'none', want, rk.get('objects', 0), rk.get('drawn', 0), rk.get('why', '')[:300]))
        else:
            same.append(r['arena'])
    L.append('  the same key count in both models: %d arenas' % len(same))
    L.append('')
    L.append('THE BOWL BEHIND THE RING (a23 against a22): the rig\'s arena family on the beauty renders (objects / drawn / holes) and the covered pixels')
    byid = {r['arena']: r for r in results}
    for a in (22, 23):
        r = byid.get(a)
        if not r:
            continue
        for look, b in r.get('beauty', {}).items():
            c = b.get('counters', {})
            fam = c.get('families', {})
            L.append('  a%d %s %s: arena objects/drawn/holes %s, ring %s, props %s, covered %d of %d; camera %s' % (a, names.get(a, '?'), look, fam.get('arena'), fam.get('ring'), fam.get('props'), c.get('covered', 0),
                     960 * 540, b.get('camera', '')))
    L.append('')
    L.append('RIG HOLES AND WHY TEXT ON THE BEAUTY RENDERS (F): screens holes and other holes are FINDINGS; "rig input missing" (%s) is the offline record\'s limit' % RIG_INPUT_TEXT)
    for r in results:
        for look, b in r.get('beauty', {}).items():
            c = b.get('counters', {})
            for hw in c.get('hole_why', []):
                tag = 'rig input missing' if RIG_INPUT_TEXT in hw and not hw.startswith('screens') else 'FINDING'
                L.append('  %s a%d %s look: %s' % (tag, r['arena'], look, hw))
    if compare is not None:
        L.append('')
        L.append('PLANTED vs EXPECTED: BLACK on every piece of %s (%s: not PASS); every other key as the unplanted arm (%s)' % (
            '; '.join('a%d %s' % (a, ' '.join(str(k) for k in sorted(ks))) for a, ks in sorted(EXPECT_PLANT.items())),
            '; '.join('a%d %s' % (a, ' '.join(str(k) for k in sorted(ks))) for a, ks in sorted(EXPECT_NOT_PASS.items())), compare['from']))
        L.append('  %s' % ('as expected' if not compare['diffs'] else '%d difference(s)' % len(compare['diffs'])))
        for dd in compare['diffs']:
            L.append('    ' + dd)
    L.append('')
    L.append('THRESHOLD DATA: per view, by verdict: pattern share, coverage (drawn / expected), median a + g, median a')
    for v in SEVERITY:
        ss = [s for r in results for vs in r.get('views', {}).values() for _, vv, s in vs if vv == v and s.get('px', 0) >= TH['min_px']]
        if not ss:
            continue
        def q(name):
            xs = [s[name] for s in ss if s.get(name) is not None]
            return ('min %.3f p5 %.3f median %.3f max %.3f' % (min(xs), float(np.percentile(xs, 5)), float(np.median(xs)), max(xs))) if xs else '-'
        L.append('  %s (n %d): pattern %s | coverage %s | a+g %s | a %s | flat ratio %s' % (v, len(ss), q('pattern'), q('coverage'), q('ag_med'), q('a_med'), q('flat_ratio')))
    # WGS6 round 2 data for the black-patch bar: the pattern share, the flat render's p90 hue misfit and its dark share over the views that PASS / are FAINT
    okv = [s for r in results for vs in r.get('views', {}).values() for _, vv, s in vs if vv in OK and s.get('px', 0) >= TH['min_px']]
    for name, label in (('pattern', 'pattern share'), ('flat_hue', 'median hue misfit'), ('flat_hue_p90', 'p90 hue misfit'), ('flat_dark', 'dark share (a black patch)')):
        xs = [s[name] for s in okv if s.get(name) is not None]
        if xs:
            L.append('  PASS / FAINT views (n %d) %s: min %.3f p1 %.3f p5 %.3f median %.3f p95 %.3f p99 %.3f max %.3f; views above 0.01: %d, 0.02: %d, 0.05: %d, 0.10: %d' % (
                len(xs), label, min(xs), np.percentile(xs, 1), np.percentile(xs, 5), np.median(xs), np.percentile(xs, 95), np.percentile(xs, 99), max(xs), sum(x > 0.01 for x in xs), sum(x > 0.02 for x in xs), sum(x > 0.05 for x in xs), sum(x > 0.10 for x in xs)))
    fr = sorted(s['flat_ratio'] for r in results for vs in r.get('views', {}).values() for _, vv, s in vs if s.get('flat_ratio') is not None and vv not in ('FAINT', 'TOO FAINT'))
    if fr:
        hist = np.histogram(fr, bins=[0, 0.2, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.95, 1.05, 10])[0]
        L.append('  flat ratio of every opaque judged view (n %d): bins <0.2 %d | 0.2-0.4 %d | 0.4-0.5 %d | 0.5-0.6 %d | 0.6-0.7 %d | 0.7-0.8 %d | 0.8-0.9 %d | 0.9-0.95 %d | 0.95-1.05 %d | >1.05 %d' % ((len(fr),) + tuple(int(x) for x in hist)))
    L.append('')
    L.append('COST')
    ck = [s for r in results for s in r.get('times', {}).get('checks', [])]
    ar = [r['times'].get(k, 0) for r in results for k in ('arena_rec_entrance', 'arena_rec_match') if k in r.get('times', {})]
    bt = [r['times'].get(k, 0) for r in results for k in ('beauty_entrance', 'beauty_match') if k in r.get('times', {})]
    L.append('  total %.1f s for %d arenas; %d check renders mean %.3f s; %d arena_rec calls mean %.2f s; %d beauty renders mean %.2f s max %.2f s (the first includes the renderer start)' % (
        total_s, len(results), len(ck), float(np.mean(ck)) if ck else 0, len(ar), float(np.mean(ar)) if ar else 0, len(bt), float(np.mean(bt)) if bt else 0, max(bt) if bt else 0))
    L.append('  gpu memory (one read during the first arena): %s' % memory)
    L.append('')
    L.append('DETAIL')
    for r in results:
        L.append('a%d %s: canvas top %.1f, stage rect centre %s; keys with a mesh %s' % (r['arena'], names.get(r['arena'], '?'), r.get('canvas_top', 0), r.get('stage_xz'), ','.join(str(k) for k in r.get('mesh_keys', [])) or 'none'))
        for (k, i), p in sorted(r.get('piece_info', {}).items()):
            L.append('  key %d piece %d %s  area %.0f tris %d centroid %s normal %s weak %d blend %d vcol %s' % (k, i, r.get('pieces', {}).get(k, {}).get(i, '?'), p['area'], p['tris'], p['centroid'], p['normal'],
                     p['weak'], p.get('blend', 0), p.get('vcol', '?')))
            for vn, vv, s in r.get('views', {}).get((k, i), []):
                L.append('    view %s %s: %s' % (vn, vv, fmt_stats(s)))
        for look, b in r.get('beauty', {}).items():
            c = b.get('counters', {})
            L.append('  beauty %s: %s; show %s; screens objects %d drawn %d holes %d (screens %d, rig input %d, other %d) validation %d why %s' % (look, b.get('camera', ''), ','.join(str(k) for k in b.get('show', [])) or 'none',
                     c.get('screens_objects', 0), c.get('screens_drawn', 0), c.get('holes', 0), c.get('screen_holes', 0), c.get('rig_input', 0), c.get('other_holes', 0), c.get('validation', 0), c.get('why', '')))
        t = r.get('times', {})
        L.append('  time: arena_rec %.2f + %.2f s, %d check renders %.2f s, beauty %.2f + %.2f s' % (t.get('arena_rec_entrance', 0), t.get('arena_rec_match', 0), len(t.get('checks', [])), sum(t.get('checks', [])),
                 t.get('beauty_entrance', 0), t.get('beauty_match', 0)))
        for n in r.get('notes', []):
            L.append('  note: ' + n)
        for e in r.get('errors', []):
            L.append('  ERROR: ' + e)
    return L


def verdict_table(results):
    return {str(r['arena']): {str(k): {str(i): v for i, v in ps.items()} for k, ps in r.get('pieces', {}).items()} for r in results}


def views_of(results):
    """{(arena, key, piece): [(view, verdict, stats)]} of a run (the known list pins a view and a band)"""
    return {(r['arena'], k, i): vs for r in results for (k, i), vs in r.get('views', {}).items()}


def as_ints(vt):
    return {int(a): {int(k): {int(i): v for i, v in ps.items()} for k, ps in ks.items()} for a, ks in vt.items()}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--arenas', default='', help='comma list (default: the 49 match arenas)')
    ap.add_argument('--looks', default='entrance,match', help='the beauty tiles to draw')
    ap.add_argument('--plant', default='', choices=['', 'highlight'])
    ap.add_argument('--both', action='store_true', help='the unplanted and the planted arm side by side (two rig processes), one report; implies --strict')
    ap.add_argument('--strict', action='store_true', help='exit 1 on any unplanted piece that is not PASS / FAINT')
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'wgs_sheet'))
    ap.add_argument('--sheets', default='', help=r"where the sheets and the report go (default: the MAIN checkout's out\sheets)")
    ap.add_argument('--game', default='')
    ap.add_argument('--no-sheet', action='store_true')
    ap.add_argument('--keep-renders', action='store_true', help='keep the per-piece renders, footprints and records under --out (deleted once judged by default)')
    ap.add_argument('--write-known', action='store_true', help='regenerate the known-findings list (--known) from this run\'s unplanted arm; refused when it holds BLACK / PASS-BACK / NO PIXELS')
    ap.add_argument('--known', default=KNOWN_FILE, help='the list of OPEN findings strict mode accepts (default tools\\scene\\arena_sheet_known.txt)')
    ap.add_argument('--known-grows', default='', help='with --write-known: the "# grows: REASON" line the growth lint needs when the list gets longer')
    ap.add_argument('--write-keys', action='store_true', help='regenerate the keys manifest (--keys) from this run\'s unplanted arm (all 49 arenas, no errors)')
    ap.add_argument('--keys', default=KEYS_FILE, help='the manifest of each arena\'s screen keys (default tools\\scene\\arena_sheet_keys.txt)')
    args = ap.parse_args(argv)
    plant = args.plant == 'highlight'
    strict_on = args.both or args.strict
    main_root = main_checkout()
    args.game = args.game or os.path.join(main_root, 'game_data', 'extracted')
    sheets = args.sheets or os.path.join(main_root, 'out', 'sheets')
    tools = [os.path.abspath(__file__), os.path.join(ROOT, 'out', 'tools', 'arena_rec.exe'), os.path.join(ROOT, 'out', 'tools', 'frame_rig.exe')]
    for exe in tools[1:]:
        if not os.path.exists(exe):
            print('arena_sheet: BLOCKED %s is not built (tools\\scene\\build_arena_rec.ps1 / build_rig.ps1)' % exe)
            return 2
    if not os.path.isdir(args.game):
        print('arena_sheet: BLOCKED no game files at %s' % args.game)
        return 2
    arenas = [int(x) for x in args.arenas.split(',') if x.strip()] if args.arenas else MATCH_ARENAS
    looks = [x for x in args.looks.split(',') if x in LOOK_SET]
    names = arena_names()
    arms = [False, True] if args.both else [plant]
    up_json = os.path.join(args.out, 'arenas_unplanted.json')
    saved_unplanted = None
    if plant and not args.both:   # the planted arm alone compares with a FRESH unplanted arm, or refuses
        try:
            with open(up_json, encoding='utf-8') as f:
                js = json.load(f)
        except (OSError, ValueError):
            js = None
        why = 'it is missing or unreadable' if js is None else stale_reason(js, arenas, tools)
        if why:
            print('arena_sheet: REFUSED --plant highlight alone: %s: %s. Run --both (or the unplanted arm first, with the same tools and thresholds).' % (up_json, why))
            return 2
        saved_unplanted = as_ints(js['verdicts'])
    done = {}

    def run_arm(p):
        arm_name = 'planted' if p else 'unplanted'
        work = os.path.join(args.out, arm_name)
        os.makedirs(work, exist_ok=True)
        arm = Arm(args, p)
        t0 = time.time()
        results = []
        try:
            for a in arenas:
                try:
                    r = arm.do(a, work, looks)
                except Exception as ex:   # one arena's crash never loses the others or the report
                    r = {'arena': a, 'pieces': {}, 'errors': ['driver: %s: %s' % (type(ex).__name__, ex)]}
                results.append(r)
                print('%s a%d: %s%s' % (arm_name, a, ' '.join('%d=%s' % (k, '/'.join(v for _, v in sorted(ps.items()))) for k, ps in sorted(r.get('pieces', {}).items())) or 'no screen keys',
                                        ('  ERROR ' + '; '.join(r['errors'])) if r.get('errors') else ''), flush=True)
        finally:
            arm.session.close()
        total = time.time() - t0
        with open(os.path.join(args.out, 'arenas_%s.json' % arm_name), 'w', encoding='utf-8') as f:
            json.dump({'verdicts': verdict_table(results), 'arenas': arenas, 'total_s': total, 'memory': arm.memory, 'stamp': stamp_now(tools)}, f, indent=1)
        done[p] = (results, arm.memory, total)

    threads = [threading.Thread(target=run_arm, args=(p,)) for p in arms]   # --both: two rig processes side by side (the cap is 2)
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    if set(done) != set(arms):
        print('arena_sheet: an arm did not finish')
        return 2
    rc, lines = 0, []
    unplanted_vt = as_ints(verdict_table(done[False][0])) if False in done else saved_unplanted
    if args.write_known:   # regenerate the committed list of open findings from this run's unplanted arm (refused when it holds a never-listable verdict)
        if unplanted_vt is None or sorted(arenas) != sorted(MATCH_ARENAS):
            print('arena_sheet: REFUSED --write-known: it needs an unplanted arm over all %d match arenas (a subset would drop the others\' entries)' % len(MATCH_ARENAS))
            return 2
        refused = write_known(unplanted_vt, args.known, args.known_grows, views_of(done[False][0]))
        print('arena_sheet: wrote %s%s' % (args.known, ('; NOT listed (never listable, they stay strict failures): ' + '; '.join(refused[:20]) + (' ...' if len(refused) > 20 else '')) if refused else ''))
    if args.write_keys:   # the committed manifest of each arena's screen keys (from the unplanted arm, all 49 arenas)
        if False not in done or sorted(arenas) != sorted(MATCH_ARENAS) or any(r.get('errors') for r in done[False][0]):
            print('arena_sheet: REFUSED --write-keys: it needs an unplanted arm over all %d match arenas with no errors' % len(MATCH_ARENAS))
            return 2
        write_keys(done[False][0], args.keys)
        print('arena_sheet: wrote %s' % args.keys)
    known = load_known(args.known)
    manifest = load_keys(args.keys)
    for p in arms:
        results, memory, total = done[p]
        compare = strict = None
        if not p and strict_on:
            strict = strict_failures(as_ints(verdict_table(results)), known, arenas, views_of(results))
            strict += ['a%d 800 piece 0: judged twice in one serve process, not the same (a kept grab): %s' % (r['arena'], r['grab_repeat'][1]) for r in results if r.get('grab_repeat') and not r['grab_repeat'][0]]
            strict += run_failures(results, manifest)
            rc = 1 if strict else rc
        if p and unplanted_vt is not None:
            pl = as_ints(verdict_table(results))
            un = {a: v for a, v in unplanted_vt.items() if a in pl}
            compare = {'from': 'this run' if args.both else up_json, 'diffs': compare_planted(pl, un) + run_failures(results, manifest)}
            rc = 1 if compare['diffs'] else rc
        lines += report_lines(results, names, p, args, memory, total, compare, strict, known=known) + ['', '=' * 100, '']
    rep = os.path.join(sheets, 'arenas_report.txt' if (args.both or not plant) else 'arenas_report_planted.txt')
    os.makedirs(sheets, exist_ok=True)
    with open(rep, 'w', encoding='utf-8') as f:   # the report FIRST: a sheet that fails never loses it
        f.write('\n'.join(lines) + '\n')
    if not args.no_sheet:
        for p in arms:
            results = done[p][0]
            suffix = '_planted' if p else ''
            check_sheet(results, names, p, os.path.join(sheets, 'screens_check%s.png' % suffix), known)
            for look in looks:
                beauty_sheet(results, names, look, p, os.path.join(sheets, 'arenas_%s%s.png' % (look, suffix)), known)
    if not args.keep_renders:   # the per-piece renders, footprints, records (GBs per run) go once judged; the sheets, the report and the json stay
        import shutil
        out_root = os.path.normcase(os.path.realpath(os.path.join(ROOT, 'out')))
        for p in arms:
            work = os.path.join(args.out, 'planted' if p else 'unplanted')
            real = os.path.normcase(os.path.realpath(work))
            if not real.startswith(out_root + os.sep):   # review note 6: never delete outside this checkout's out\ folder
                print('arena_sheet: renders kept: %s is not under %s' % (work, out_root))
                continue
            if os.path.basename(work) in ('planted', 'unplanted') and os.path.isdir(work):
                shutil.rmtree(work, ignore_errors=True)
    print('arena_sheet: done (exit %d); report %s' % (rc, rep))
    return rc


if __name__ == '__main__':
    sys.exit(main())
