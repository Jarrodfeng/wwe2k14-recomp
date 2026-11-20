#!/usr/bin/env python3
r"""WGS: the unit of tools\scene\arena_sheet.py's check, on SYNTHETIC pictures only (no game data, no GPU, ~1 s).
  py -I tools\scene\arena_sheet_unit.py
Drives the production path the check pass runs: judge_piece(the screens-only render, key, footprint) per view -> piece_verdict over a piece's views -> key_verdict ->
strict_failures (the unplanted arm under --both) / compare_planted (the planted arm) -> report_lines; in that order, as main() does.
Rows (the review's): a solid pattern is PASS (its grid / number shades, lit at 0.8, a faint blend over the clear colour = FAINT); a dim screen (P x 0.05, P x 0.2, P x 0.3), black with
a faint tint (6,0,0), unlit navy (10,15,40) are not PASS; black is BLACK; nothing drawn is NO PIXELS; 51 % pattern + 49 % black is not PASS; drawn / expected < 0.8 is PARTIAL (not
for a see-through copy); only the piece's own footprint is judged; a piece drawn only from behind is PASS-BACK; an unplanted BLACK under --both fails; the planted table (BLACK,
not-PASS keys, unchanged keys); a result dict without rig_keys still gives a report; dropped pieces appear in it; PATTERN equals the rig's kCol table. Exit 0 = all PASS.
"""
# entry: judge_piece
import argparse, os, re, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import arena_sheet as A  # noqa: E402

fails = []


def check(name, got, want):
    ok = got == want
    print('%s %s: got %r want %r' % ('PASS' if ok else 'FAIL', name, got, want))
    if not ok:
        fails.append(name)


def render(parts, n=400):
    """a screens-only render: one row of pixels [(count, rgb)], the rest the clear colour"""
    row = []
    for c, rgb in parts:
        row += [tuple(int(round(x)) for x in rgb)] * c
    row += [A.CLEAR] * (n - len(row))
    return np.asarray([row], dtype=np.uint8)


def blend(rgb, a):
    return tuple(a * x + (1 - a) * c for x, c in zip(rgb, A.CLEAR))


def scale(rgb, s):
    return tuple(s * x for x in rgb)


J = lambda img, key, **kw: A.judge_piece(img, key, **kw)[0]   # noqa: E731

check('mask counts the drawn pixels', int(A.mask_of(render([(30, (200, 0, 0))])).sum()), 30)
check('mask ignores near-clear', int(A.mask_of(render([(30, (33, 38, 48))])).sum()), 0)

for key in A.KEYS:
    P = A.PATTERN[key]
    check('solid pattern %d' % key, J(render([(100, P)]), key), 'PASS')
    check('grid + number shades %d' % key, J(render([(70, P), (20, scale(P, 0.6)), (10, scale(P, 0.35))]), key), 'PASS')
    check('lit at 0.8 %d' % key, J(render([(100, scale(P, 0.8))]), key), 'PASS')
    check('faint blend (alpha 0.2) over the clear colour %d = FAINT' % key, J(render([(100, blend(P, 0.2))]), key), 'FAINT')
    for s in (0.05, 0.2, 0.3):
        check('dim screen %d x %.2f is not PASS' % (key, s), J(render([(100, scale(P, s))]), key) in A.OK, False)
check('black with a faint red tint (6,0,0) on 800 is not PASS', J(render([(100, (6, 0, 0))]), 800) in A.OK, False)
check('unlit navy (10,15,40) on 802 is not PASS', J(render([(100, (10, 15, 40))]), 802) in A.OK, False)
check('804 at 8 % is not PASS', J(render([(100, scale(A.PATTERN[804], 0.08))]), 804) in A.OK, False)
check('800 at 20 % is DIM or BLACK', J(render([(100, scale(A.PATTERN[800], 0.2))]), 800) in ('DIM', 'BLACK'), True)
check('801 (green) at 0.3 is DIM', J(render([(100, scale(A.PATTERN[801], 0.3))]), 801), 'DIM')
for a, b in ((804, 809), (804, 807), (800, 806), (801, 808), (805, 808), (802, 807)):
    check('colour of %d on key %d is not PASS' % (b, a), J(render([(100, A.PATTERN[b])]), a) in A.OK, False)
check('white is not PASS', J(render([(100, (255, 255, 255))]), 804), 'UNCLEAR')
check('grey is not PASS', J(render([(100, (128, 128, 128))]), 800) in A.OK, False)
check('black screen', J(render([(100, (0, 0, 0))]), 804), 'BLACK')
check('nothing drawn', J(render([]), 804), 'NO PIXELS')
check('under min_px (19 px)', J(render([(19, A.PATTERN[804])]), 804), 'NO PIXELS')
check('min_px (20 px)', J(render([(20, A.PATTERN[804])]), 804), 'PASS')
P, K, W = A.PATTERN[804], (0, 0, 0), (255, 255, 255)
check('51 % pattern + 49 % black is not PASS', J(render([(51, P), (49, K)], 100), 804) in A.OK, False)
check('dark 0.51 pattern 0.09 = BLACK', J(render([(51, K), (9, P), (40, W)], 100), 804), 'BLACK')
n_pass = int(round(A.TH['pass_pattern'] * 100))
check('pattern at pass_pattern = PASS', J(render([(n_pass, P), (100 - n_pass, W)], 100), 804), 'PASS')
check('pattern 1 below pass_pattern = UNCLEAR', J(render([(n_pass - 1, P), (101 - n_pass, W)], 100), 804), 'UNCLEAR')
# coverage: drawn / expected
check('drawn / expected 0.79 = PARTIAL', J(render([(79, P)], 100), 804, expect_px=100), 'PARTIAL')
check('drawn / expected 0.80 = PASS', J(render([(80, P)], 100), 804, expect_px=100), 'PASS')
check('a see-through copy at coverage 0.5 is FAINT, not PARTIAL', J(render([(50, blend(P, 0.1))], 100), 804, expect_px=100), 'FAINT')
# the footprint: only the piece's own pixels are judged; coverage counts every non-clear pixel there
img = render([(60, P), (40, scale(A.PATTERN[800], 0.2))], 100)
fp = np.zeros((1, 100), dtype=bool)
fp[0, :60] = True
check('another piece outside the footprint is not judged', J(img, 804, footprint=fp), 'PASS')
fp2 = np.zeros((1, 100), dtype=bool)
fp2[0, :80] = True
check('footprint 80, drawn 60 = PARTIAL', J(img, 804, footprint=fp2) in ('PARTIAL', 'UNCLEAR'), True)
v, st = A.judge_piece(img, 804, footprint=fp)
check('footprint stats: expect = its size', (st['expect'], round(st['coverage'], 2)), (60, 1.0))
# a piece's verdict over its views
check('front PASS', A.piece_verdict([('front', 'PASS', {})]), 'PASS')
check('front nothing, back PASS = PASS-BACK', A.piece_verdict([('front', 'NO PIXELS', {}), ('back', 'PASS', {})]), 'PASS-BACK')
check('front nothing, back BLACK = BLACK', A.piece_verdict([('front', 'NO PIXELS', {}), ('back', 'BLACK', {})]), 'BLACK')
check('front nothing, back nothing = NO PIXELS', A.piece_verdict([('front', 'NO PIXELS', {}), ('back', 'NO PIXELS', {})]), 'NO PIXELS')
check('weak piece: the worst of its views', A.piece_verdict([('x+', 'PASS', {}), ('x-', 'DIM', {}), ('z+', 'PASS', {})]), 'DIM')
check('key verdict worst', A.key_verdict({0: 'PASS', 1: 'BLACK', 2: 'UNCLEAR'}), 'BLACK')
check('key verdict FAINT + PASS', A.key_verdict({0: 'PASS', 1: 'FAINT'}), 'FAINT')
# DIM1: brightness is judged on the FLAT render against the key colour x the interpolated vertex colour, not on the pattern's shades
P8 = A.PATTERN[802]
F8 = A.FLAT[802]   # WGS6 round 2: the flat render of key 802 is its FLAT colour (the pastel feed), the pattern render keeps the saturated PATTERN
fp_all = np.ones((1, 100), dtype=bool)
digits = render([(60, scale(P8, 0.35)), (40, P8)], 100)   # a full-brightness piece that shows mostly digits (the pattern render)
check('without a flat render a mostly-digit piece reads DIM (the first judge\'s fault)', J(digits, 802, footprint=fp_all), 'DIM')
check('a full-brightness piece that shows mostly digits is NOT DIM (flat render at full)', J(digits, 802, footprint=fp_all, flat=render([(100, F8)], 100), vc=np.full((1, 100), 255, np.uint8)) != 'DIM', True)
check('a flat render at 0.3 of its expected vertex-colour value IS DIM', J(render([(100, scale(P8, 0.3 * 0.8))], 100), 802, footprint=fp_all, flat=render([(100, scale(F8, 0.3 * 0.8))], 100),
                                                                         vc=np.full((1, 100), int(0.8 * 255), np.uint8)), 'DIM')
check('vertex colour 0.7 drawing at 0.7 is NOT DIM', J(render([(100, scale(P8, 0.7))], 100), 802, footprint=fp_all, flat=render([(100, scale(F8, 0.7))], 100),
                                                         vc=np.full((1, 100), int(0.7 * 255), np.uint8)), 'PASS')
vc68 = np.full((1, 100), int(0.68 * 255), np.uint8)
check('digits (0.35) on a 0.68 vertex-colour slat are the pattern: PASS', J(render([(60, scale(P8, 0.35 * 0.68)), (40, scale(P8, 0.68))], 100), 802, footprint=fp_all,
                                                                         flat=render([(100, scale(F8, 0.68))], 100), vc=vc68), 'PASS')
check('a near-black (6,0,0) on a white vertex colour is still not the pattern', J(render([(100, (6, 0, 0))], 100), 800, footprint=fp_all, flat=render([(100, (6, 0, 0))], 100),
                                                                                  vc=np.full((1, 100), 255, np.uint8)) in A.OK, False)
check('a vertex-alpha piece drawing exactly at its alpha (0.25) is not DIM', J(render([(100, blend(P8, 0.25))], 100), 802, footprint=fp_all, blended=True,
                                                                                  flat=render([(100, blend(F8, 0.25))], 100), vc=np.full((1, 100), 255, np.uint8),
                                                                                  va=np.full((1, 100), int(0.25 * 255), np.uint8)) != 'DIM', True)
v_, s_ = A.judge_piece(render([(100, P8)], 100), 802, footprint=fp_all, flat=render([(100, F8)], 100), vc=np.full((1, 100), 255, np.uint8))
check('the flat ratio is reported', round(s_.get('flat_ratio', 0), 2), 1.0)
check('a black screen is BLACK on the pattern render (the flat one is black too)', J(render([(100, (0, 0, 0))], 100), 802, footprint=fp_all, flat=render([(100, (0, 0, 0))], 100),
                                                                                    vc=np.full((1, 100), 255, np.uint8)), 'BLACK')
# WGS6: the vertex colour is PER CHANNEL (a54 808 / 809 pieces 26 / 27: R 255, G 216, B 159 on part of the vertices; the game and ours multiply the texel by it channel by channel)
P808, F808 = A.PATTERN[808], A.FLAT[808]
tint = np.array([255, 216, 159], np.uint8)
vc_tint = np.tile(tint, (1, 100, 1))                                   # (1, 100, 3): the plane arena_rec writes
tinted = np.tile(np.round(np.asarray(P808, dtype=np.float64) * tint / 255.0).astype(np.uint8), (1, 100, 1))   # drawn with the tint: P x vc per channel
plain = np.tile(np.asarray(P808, dtype=np.uint8), (1, 100, 1))         # drawn without it (a renderer that ignored the vertex colour)
tinted_f = np.tile(np.round(np.asarray(F808, dtype=np.float64) * tint / 255.0).astype(np.uint8), (1, 100, 1))   # the flat render with the tint
plain_f = np.tile(np.asarray(F808, dtype=np.uint8), (1, 100, 1))
check('a tinted vertex colour drawn with its tint is PASS (a54 808 pieces 26 / 27)', J(tinted, 808, footprint=fp_all, flat=tinted_f, vc=vc_tint), 'PASS')
check('the same drawing against an untinted expectation (the old largest-byte plane) was UNCLEAR', J(tinted, 808, footprint=fp_all, flat=tinted_f, vc=np.full((1, 100), 255, np.uint8)), 'UNCLEAR')
check('a tinted vertex colour drawn WITHOUT its tint (a renderer that ignored the colour) is not PASS', J(plain, 808, footprint=fp_all, flat=plain_f, vc=vc_tint) in A.OK, False)
check('a grey vertex colour as a (H, W) plane or as three equal channels judges the same', (J(render([(100, scale(P8, 0.68))], 100), 802, footprint=fp_all, flat=render([(100, scale(F8, 0.68))], 100), vc=vc68),
      J(render([(100, scale(P8, 0.68))], 100), 802, footprint=fp_all, flat=render([(100, scale(F8, 0.68))], 100), vc=np.repeat(vc68[..., None], 3, axis=2))), ('PASS', 'PASS'))
ok_g, a_g, g_g = A.pattern_fit(np.array([scale(P8, 0.5)]), 802, scale=np.array([0.5]))
ok_c, a_c, g_c = A.pattern_fit(np.array([scale(P8, 0.5)]), 802, scale=np.array([[0.5, 0.5, 0.5]]))
check('pattern_fit: a grey scale and three equal channels give the same fit, and a = the first judge\'s scalar (0.5)', (bool(ok_g[0]), bool(ok_c[0]), round(float(a_g[0]), 3), round(float(a_c[0]), 3)), (True, True, 0.5, 0.5))
# WGS6 round 2: the flat feed has its own pastel colours (all three channels well above zero, |R - B| >= 70), so a per-channel tint and a red / blue swap are seen on EVERY key
swap_idx = [2, 1, 0]
min_margin = {'tint': 99.0, 'swap': 99.0}
for key in A.KEYS:
    Pk, Fk = np.asarray(A.PATTERN[key], dtype=np.float64), np.asarray(A.FLAT[key], dtype=np.float64)
    tile3 = lambda c: np.tile(np.round(c).astype(np.uint8), (1, 100, 1))   # noqa: E731
    pat_t, fl_t = tile3(Pk * tint / 255.0), tile3(Fk * tint / 255.0)
    check('the tint (255, 216, 159) drawn with its tint is PASS on key %d' % key, J(pat_t, key, footprint=fp_all, flat=fl_t, vc=vc_tint), 'PASS')
    r_t = A.judge_piece(tile3(Pk), key, footprint=fp_all, flat=tile3(Fk), vc=vc_tint)
    check('the tint (255, 216, 159) drawn UNTINTED is not PASS on key %d' % key, r_t[0] in A.OK, False)
    min_margin['tint'] = min(min_margin['tint'], r_t[1].get('flat_hue', 99.0))
    r_s = A.judge_piece(tile3(Pk[swap_idx]), key, footprint=fp_all, flat=tile3(Fk[swap_idx]), vc=np.full((1, 100), 255, np.uint8))
    check('red and blue swapped is not PASS on key %d' % key, r_s[0] in A.OK, False)
    min_margin['swap'] = min(min_margin['swap'], r_s[1].get('flat_hue', 99.0))
check('the flat colour test catches the untinted tint and the red / blue swap with at least 1.5 x its tolerance on every key (median misfit %.2f / %.2f)' % (min_margin['tint'], min_margin['swap']), min(min_margin.values()) >= 1.5, True)
check('every FLAT colour has all three channels at 100 or more, its largest 255, |R - B| >= 70, and no two keys within 30 levels', (all(min(c) >= 100 and max(c) == 255 and abs(c[0] - c[2]) >= 70 for c in A.FLAT.values()),
      min(max(abs(a - b) for a, b in zip(A.FLAT[i], A.FLAT[j])) for i in A.KEYS for j in A.KEYS if i < j) >= 30), (True, True))
# WGS6 round 2: HIDDEN (a view more than half hidden by earlier nearer pieces of its key is not judged); a piece hidden from its only camera is judged from an alternative one
st_h = {}
check('a PASS view 61 % hidden is HIDDEN (a53 801 / 802 piece 8)', A.apply_hidden('PASS', st_h, {'expect': 37000, 'hidden': 58500}), 'HIDDEN')
check('... and records the share and what it would have been', (round(st_h['hidden_share'], 2), st_h['hidden_was']), (0.61, 'PASS'))
check('a TOO FAINT view 99.8 % hidden is HIDDEN, not TOO FAINT (a54 piece 13)', A.apply_hidden('TOO FAINT', {}, {'expect': 84, 'hidden': 42996}), 'HIDDEN')
check('a view exactly half hidden is judged (more than half is the bar)', A.apply_hidden('PASS', {}, {'expect': 100, 'hidden': 100}), 'PASS')
check('a fault on a mostly hidden view stays a fault (never hidden behind a grey)', (A.apply_hidden('BLACK', {}, {'expect': 10, 'hidden': 990}), A.apply_hidden('DIM', {}, {'expect': 15257, 'hidden': 49095})), ('BLACK', 'DIM'))
check('a view with nothing hidden is unchanged', A.apply_hidden('PASS', {}, {'expect': 500, 'hidden': 0}), 'PASS')
check('HIDDEN front + a judged alt view: the piece takes the alt verdict', A.piece_verdict([('front', 'HIDDEN', {}), ('alt', 'FAINT', {})]), 'FAINT')
check('HIDDEN front, no alt: the piece is HIDDEN', A.piece_verdict([('front', 'HIDDEN', {})]), 'HIDDEN')
check('HIDDEN front + HIDDEN alt: the piece is HIDDEN', A.piece_verdict([('front', 'HIDDEN', {}), ('alt', 'HIDDEN', {})]), 'HIDDEN')
check('HIDDEN front + a faulting alt: the alt\'s fault', A.piece_verdict([('front', 'HIDDEN', {}), ('alt', 'DIM', {})]), 'DIM')
check('a weak piece: the worst of its judged views, HIDDEN ones left out', A.piece_verdict([('x+', 'HIDDEN', {}), ('x-', 'FAINT', {}), ('z+', 'HIDDEN', {})]), 'FAINT')
check('a weak piece hidden from all four views is HIDDEN', A.piece_verdict([('x+', 'HIDDEN', {}), ('x-', 'HIDDEN', {})]), 'HIDDEN')
check('HIDDEN never fails strict and is not a pass', (A.strict_failures({54: {808: {13: 'HIDDEN'}}}), 'HIDDEN' in A.OK), ([], False))
check('a piece with every pixel hidden (expect 0, hidden > 0) is not EDGE-ON', A.edge_on({'front': {'expect': 0, 'hidden': 42996}}), False)
check('a piece with no pixel at all stays EDGE-ON', A.edge_on({'front': {'expect': 0, 'hidden': 0}}), True)
# WGS6 round 2: a partly black screen. The dark share of the flat render (where the expectation is lit) is 0.000 on every one of the 2,860 PASS / FAINT views of the full run, so PASS allows 1 % of the
# measured pixels (TH['dark_patch']); a patch above that is UNCLEAR. The pattern share alone passed 16 % black (0.84 >= 0.80) and the real pieces' own p5 is 0.884, so it cannot carry this.
fp200 = np.ones((1, 200), dtype=bool)
vc200 = np.full((1, 200), 255, np.uint8)
Kb = (0, 0, 0)
for black_px, want_pass in ((0, True), (2, True), (3, False), (20, False), (32, False), (80, False)):   # of 200 measured pixels: 0 %, 1 %, 1.5 %, 10 %, 16 %, 40 %
    img_b = render([(200 - black_px, P8), (black_px, Kb)], 200)
    flat_b = render([(200 - black_px, F8), (black_px, Kb)], 200)
    check('%d of 200 pixels (%.1f %%) black on the flat render: %s' % (black_px, 100.0 * black_px / 200, 'PASS' if want_pass else 'not PASS'), J(img_b, 802, footprint=fp200, flat=flat_b, vc=vc200) in A.OK, want_pass)
check('a 10 % black patch is UNCLEAR (a fault), not BLACK and not PASS', J(render([(180, P8), (20, Kb)], 200), 802, footprint=fp200, flat=render([(180, F8), (20, Kb)], 200), vc=vc200), 'UNCLEAR')
check('where the vertex colour is 0 the pixels are expected dark: not a black patch', A.flat_hue(render([(100, Kb)], 100), np.zeros((1, 100), np.uint8), fp_all, 802)is None, True)
# WGS6: occlusion. The footprint is only the pixels where this piece is drawn: an earlier, nearer piece of the key hides the rest, and what is hidden is not judged (the hidden half is another piece's picture)
half = render([(50, P), (50, K)], 100)                                # the visible half is the pattern, the hidden half shows an earlier piece (black here)
fp_half = np.zeros((1, 100), dtype=bool)
fp_half[0, :50] = True
check('a piece half hidden by an earlier nearer piece is judged on its visible half: PASS', J(half, 804, footprint=fp_half), 'PASS')
check('the hidden half being black does not make it BLACK', J(half, 804, footprint=fp_half) != 'BLACK', True)
check('the same picture judged on the whole footprint is not PASS (the old check\'s view of it)', J(half, 804, footprint=np.ones((1, 100), dtype=bool)) in A.OK, False)
missing = render([(30, P), (20, A.CLEAR), (50, K)], 100)               # the visible half is only 60 % drawn, the hidden half is black
check('a piece half hidden whose VISIBLE half is missing is PARTIAL', J(missing, 804, footprint=fp_half), 'PARTIAL')
# WGS6: a piece the key's earlier pieces hide down to a few pixels (or whose visible part is an invisible fade) has nothing to judge: not NO PIXELS, not PASS
fp_tiny = np.zeros((1, 100), dtype=bool)
fp_tiny[0, :10] = True
vc_w = np.full((1, 100), 255, np.uint8)
check('opaque piece, 10 visible pixels expected, drawn: EDGE-ON (not judged)', J(render([(10, P)], 100), 804, footprint=fp_tiny, flat=render([(10, P)], 100), vc=vc_w, va=np.full((1, 100), 255, np.uint8)), 'EDGE-ON')
check('blended piece whose visible part is an invisible fade (alpha 1 of 255) and drew nothing: TOO FAINT', J(render([], 100), 804, footprint=fp_all, blended=True, flat=render([], 100), vc=vc_w, va=np.full((1, 100), 1, np.uint8)), 'TOO FAINT')
check('blended piece expected at alpha 4 of 255 (a signal of 3 levels, below meas_min) that drew 1 level: TOO FAINT, not NO PIXELS (a54 808 / 809 piece 13)', J(render([(100, (32, 37, 47))], 100), 804, footprint=fp_all, blended=True, flat=render([(100, (32, 37, 47))], 100), vc=vc_w, va=np.full((1, 100), 4, np.uint8)), 'TOO FAINT')
check('a piece expected to show 100 visible pixels that drew nothing is still NO PIXELS', J(render([], 100), 804, footprint=fp_all, flat=render([], 100), vc=vc_w, va=np.full((1, 100), 255, np.uint8)), 'NO PIXELS')
# round 5: a vertex-alpha screen whose texel went black is BLACK or DIM, never TOO FAINT; brightness judged against alpha x vc x P + (1 - alpha) x C
P0, F0 = A.PATTERN[800], A.FLAT[800]
for al in (0.1, 0.2, 0.4):
    va_ = np.full((1, 100), int(round(al * 255)), np.uint8)
    good = render([(100, blend(P0, al))], 100)                     # drawn right: the pattern blended at alpha
    good_f = render([(100, blend(F0, al))], 100)                   # ... and its flat render
    black = render([(100, blend((0, 0, 0), al))], 100)              # the texel went black: black blended at alpha
    vcw = np.full((1, 100), 255, np.uint8)
    vb = J(black, 800, footprint=fp_all, blended=True, flat=black, vc=vcw, va=va_)
    check('blended, black texel at alpha %.1f: not TOO FAINT and not PASS (got %s)' % (al, vb), vb not in A.NOT_JUDGED + A.OK, True)
    check('blended, drawn right at alpha %.1f: FAINT or PASS' % al, J(good, 800, footprint=fp_all, blended=True, flat=good_f, vc=vcw, va=va_) in A.OK, True)
va9 = np.full((1, 100), int(0.9 * 255), np.uint8)
dimmed = render([(100, blend(scale(P0, 0.35), 0.9))], 100)
dimmed_f = render([(100, blend(scale(F0, 0.35), 0.9))], 100)
check('alpha 0.9 with the texel at 35 % is DIM', J(dimmed, 800, footprint=fp_all, blended=True, flat=dimmed_f, vc=np.full((1, 100), 255, np.uint8), va=va9), 'DIM')
# round 5: an arena that errors, or loads fewer keys than the manifest, fails strict
MAN = {4: [800, 801, 804], 23: []}
check('an arena with errors fails strict', any('ERROR' in s for s in A.run_failures([{'arena': 4, 'pieces': {}, 'errors': ['arena_rec entrance rc 3'], 'mesh_keys': []}], MAN)), True)
check('an arena with fewer keys than the manifest fails strict', any('manifest says' in s for s in A.run_failures([{'arena': 4, 'pieces': {}, 'mesh_keys': [800, 801]}], MAN)), True)
check('the manifest\'s "none" arena with no keys passes', A.run_failures([{'arena': 23, 'pieces': {}, 'mesh_keys': []}], MAN), [])
check('an arena missing from the manifest fails', any('not in' in s for s in A.run_failures([{'arena': 5, 'pieces': {}, 'mesh_keys': [800]}], MAN)), True)
# round 5: the known list may not grow against main without a "# grows:" line
import known_growth_lint as KG  # noqa: E402
old_k = '# header\n4 801 3 DIM DIM x\n'
check('a grown known list without a reason fails', KG.growth_problem(old_k, old_k + '4 801 4 DIM DIM x\n') is not None, True)
check('a grown known list with "# grows:" passes', KG.growth_problem(old_k, '# grows: a new class\n' + old_k + '4 801 4 DIM DIM x\n'), None)
check('a shrunk known list passes', KG.growth_problem(old_k, '# header\n'), None)
# PAR1: a kept grab must change nothing (the rig run judges key 800 piece 0 twice per arena in one serve process; this is the comparison it uses)
check('two served jobs with the same verdict and pixel count: same', A.grab_repeat_result('PASS', {'px': 182748}, 'PASS', {'px': 182748})[0], True)
check('a kept grab (fewer pixels the second time) is caught', A.grab_repeat_result('PASS', {'px': 182748}, 'PARTIAL', {'px': 109000})[0], False)
check('a primed-grab render of 800 (its pattern through the grab) is judged like any key', J(render([(100, A.PATTERN[800])]), 800), 'PASS')
# not judged: TOO FAINT (a see-through copy too faint for the hue fit), EDGE-ON (no camera-facing pixel from any view)
check('a 4 % vertex-alpha copy that fails the hue test = TOO FAINT', J(render([(60, blend(A.PATTERN[800], 0.04)), (40, (34, 36, 46))]), 800, blended=True), 'TOO FAINT')
check('the same pixels on a piece with no vertex alpha stay UNCLEAR', J(render([(60, blend(A.PATTERN[800], 0.04)), (40, (34, 36, 46))]), 800), 'UNCLEAR')
check('a white screen on a blended piece is not TOO FAINT', J(render([(100, (255, 255, 255))]), 804, blended=True) in A.NOT_JUDGED + A.OK, False)
check('an opaque piece that fails the hue test stays UNCLEAR', J(render([(60, A.PATTERN[804]), (40, W)]), 804), 'UNCLEAR')
check('no camera-facing pixel from any view = EDGE-ON', A.edge_on({'front': {'expect': 0}, 'back': {'expect': 0}}), True)
check('one view with pixels is not EDGE-ON', A.edge_on({'front': {'expect': 0}, 'back': {'expect': 12}}), False)
check('TOO FAINT / EDGE-ON never fail strict', A.strict_failures({4: {804: {0: 'TOO FAINT', 1: 'EDGE-ON'}}}), [])
check('a key with a TOO FAINT piece and PASS pieces shows TOO FAINT', A.key_verdict({0: 'PASS', 1: 'TOO FAINT'}), 'TOO FAINT')
# the known list (open findings): listed and unchanged passes; unlisted fails; changed (PASS included) fails with "remove it"; BLACK is never listed.
# WGS6 round 2: a row pins ONE VIEW and a band for the number that made its verdict, and the piece's other views are judged as usual
KN = {(4, 801, 3): ('DIM', 'DIM', 'docs', 'x+', 'flat_ratio', 0.12, 0.42), (44, 800, 1): ('PARTIAL', 'PARTIAL', 'docs', 'front', 'coverage', 0.4, 0.7)}
VW = {(4, 801, 3): [('x+', 'DIM', {'flat_ratio': 0.27}), ('x-', 'FAINT', {}), ('z+', 'PASS', {}), ('z-', 'HIDDEN', {})]}
check('a listed DIM passes strict', A.strict_failures({4: {801: {3: 'DIM'}}}, KN, [4], VW), [])
check('an unlisted DIM fails strict', any('NEW finding' in s for s in A.strict_failures({4: {801: {4: 'DIM'}}}, KN, [4], VW)), True)
check('a listed piece that now PASSES fails: remove it', any('listed as DIM, now PASS: remove it from the list' in s for s in A.strict_failures({4: {801: {3: 'PASS'}}}, KN, [4], VW)), True)
check('a listed piece the run no longer has fails', any('no such piece' in s for s in A.strict_failures({44: {800: {0: 'PASS'}}}, KN, [44], VW)), True)
check('list entries of arenas not run are not checked', A.strict_failures({4: {801: {3: 'DIM'}}}, KN, [4], VW), [])
check('a BLACK fails strict even when listed', any('never accepted' in s for s in A.strict_failures({44: {800: {1: 'BLACK'}}}, {(44, 800, 1): ('BLACK', 'BLACK', 'x', 'front', 'pattern', 0.0, 1.0)}, [44], {})), True)
vw2 = dict(VW)
vw2[(4, 801, 3)] = [('x+', 'DIM', {'flat_ratio': 0.27}), ('x-', 'UNCLEAR', {}), ('z+', 'PASS', {}), ('z-', 'PASS', {})]
check('a NEW non-PASS view on a listed piece fails even though the piece\'s verdict (DIM, the worst) is unchanged', any('NEW non-PASS view' in s and 'x- is UNCLEAR' in s for s in A.strict_failures({4: {801: {3: 'DIM'}}}, KN, [4], vw2)), True)
vw3 = dict(VW)
vw3[(4, 801, 3)] = [('x+', 'DIM', {'flat_ratio': 0.55}), ('x-', 'FAINT', {})]
check('the listed view\'s number outside its band fails (a different cause of DIM)', any('outside the listed band 0.12-0.42' in s for s in A.strict_failures({4: {801: {3: 'DIM'}}}, KN, [4], vw3)), True)
vw4 = {(4, 801, 3): [('x-', 'DIM', {'flat_ratio': 0.27}), ('x+', 'FAINT', {})]}
check('the listed view no longer has the verdict (another view carries it) fails', any('listed view x+ is FAINT' in s for s in A.strict_failures({4: {801: {3: 'DIM'}}}, KN, [4], vw4)), True)
old_row = {(4, 801, 3): ('DIM', 'DIM', 'docs', None, None, None, None)}
check('a known row without a view / band (the old format) is refused', any('no view / band' in s and 'refused' in s for s in A.strict_failures({4: {801: {3: 'DIM'}}}, old_row, [4], VW)), True)
import tempfile  # noqa: E402
with tempfile.TemporaryDirectory() as td:
    kp = os.path.join(td, 'known.txt')
    vws = {(44, 805, 0): [('front', 'DIM', {'flat_ratio': 0.3})], (4, 801, 3): [('x+', 'DIM', {'flat_ratio': 0.27}), ('x-', 'PASS', {})], (44, 800, 1): [('front', 'PARTIAL', {'coverage': 0.5})]}
    check('a BLACK in the run: --write-known refuses it (named) and lists the rest', (A.write_known({44: {800: {0: 'BLACK'}, 805: {0: 'DIM'}}}, kp, '', vws), sorted(A.load_known(kp))),
          (['a44 800 piece 0: BLACK may never be listed'], [(44, 805, 0)]))
    check('--write-known writes the open findings only, each with its view and a band (measured +/- 0.15)', (A.write_known({4: {801: {3: 'DIM', 4: 'PASS', 5: 'TOO FAINT'}}, 44: {800: {1: 'PARTIAL'}}}, kp, '', vws), sorted(A.load_known(kp).items())),
          ([], [((4, 801, 3), ('DIM', 'DIM', A.KNOWN_OWNER['DIM'], 'x+', 'flat_ratio', 0.12, 0.42)), ((44, 800, 1), ('PARTIAL', 'PARTIAL', A.KNOWN_OWNER['PARTIAL'], 'front', 'coverage', 0.35, 0.65))]))
    check('--write-known refuses a finding with no view to pin', A.write_known({4: {801: {3: 'DIM'}}}, kp, '', {})[0].startswith('a4 801 piece 3: DIM has no view'), True)
with tempfile.TemporaryDirectory() as td2:   # a listed row keeps its own owner when the list is regenerated (a row owned by a renderer contract site)
    kp2 = os.path.join(td2, 'known.txt')
    with open(kp2, 'w', encoding='utf-8') as f2:
        f2.write('54 808 21 DIM DIM SZW_contract.md_site18 x+ flat_ratio 0.12 0.42' + chr(10))
    A.write_known({54: {808: {21: 'DIM', 22: 'DIM'}}}, kp2, '', {(54, 808, 21): [('x+', 'DIM', {'flat_ratio': 0.27})], (54, 808, 22): [('x+', 'DIM', {'flat_ratio': 0.3})]})
    check('--write-known keeps the owner of a row that is already listed with the same verdict', (A.load_known(kp2)[(54, 808, 21)][2], A.load_known(kp2)[(54, 808, 22)][2]), ('SZW_contract.md_site18', A.KNOWN_OWNER['DIM']))
# strict (--both): an unplanted fault fails, EXPECT_PLANT keys named
check('unplanted all PASS / FAINT', A.strict_failures({4: {804: {0: 'PASS', 1: 'FAINT'}}}), [])
sf = A.strict_failures({44: {800: {0: 'BLACK'}}, 4: {801: {3: 'PASS-BACK'}}})
check('unplanted BLACK under --both fails (a deleted TRN4 fix)', any(s.startswith('a44 800 piece 0: BLACK') and 'EXPECT_PLANT' in s for s in sf), True)
check('unplanted PASS-BACK fails', any(s.startswith('a4 801 piece 3: PASS-BACK') for s in sf), True)
# the planted table
un = {44: {800: {0: 'PASS', 1: 'PASS'}, 805: {0: 'PASS'}}, 53: {801: {0: 'PASS'}}, 4: {804: {0: 'PASS'}}}
pl = {44: {800: {0: 'BLACK', 1: 'BLACK'}, 805: {0: 'PASS'}}, 53: {801: {0: 'NO PIXELS'}}, 4: {804: {0: 'PASS'}}}
d0 = A.compare_planted(pl, un)
check('planted as expected (a44 800, a53 801 NO PIXELS = not PASS, a4 unchanged)', [d for d in d0 if d.startswith(('a44 800', 'a44 805', 'a4 ', 'a53 801'))], [])
pl2 = {44: {800: {0: 'BLACK', 1: 'PASS'}, 805: {0: 'PASS'}}, 53: {801: {0: 'PASS'}}, 4: {804: {0: 'BLACK'}}}
d2 = A.compare_planted(pl2, un)
check('a planted piece that stayed PASS is found', any(d.startswith('a44 800 piece 1: expected BLACK') for d in d2), True)
check('a53 801 PASS with the plant is found', any(d.startswith('a53 801 piece 0: expected not PASS') for d in d2), True)
check('a change outside the expected set is found', any(d.startswith('a4 804 piece 0') for d in d2), True)
# the report never needs a full result (an arena whose arena_rec failed); dropped pieces appear
args = argparse.Namespace(game='(unit)')
crashed = {'arena': 4, 'pieces': {}, 'errors': ['arena_rec entrance rc 3: FAIL']}
full = {'arena': 2, 'pieces': {801: {0: 'PASS'}}, 'views': {(801, 0): [('front', 'PASS', {'px': 100, 'pattern': 1.0, 'a_med': 1.0, 'g_med': 0.0, 'ag_med': 1.0})]}, 'mesh_keys': [801],
        'dropped': {801: (46, 32, 0.0137)}, 'piece_info': {}, 'beauty': {}, 'times': {}}
try:
    text = '\n'.join(A.report_lines([crashed, full], {}, False, args, 'n/a', 1.0, None, strict=A.strict_failures({})))
    check('a result without rig_keys still gives a report', 'ERROR arena_rec entrance rc 3' in text, True)
    check('dropped pieces appear in the report', 'a2 801: 46 pieces, 32 judged, 14 dropped' in text, True)
except Exception as ex:   # noqa: BLE001
    check('a result without rig_keys still gives a report (raised %s)' % type(ex).__name__, False, True)
# the colour table equals the rig's (frame_rig.cpp RigScreenFeed::PatternColour kCol)
src = open(os.path.join(HERE, 'frame_rig.cpp'), encoding='utf-8', errors='replace').read()
mt = re.search(r'kCol\[10\]\[3\] = \{(.*?)\};', src)
rig = [tuple(int(x) for x in t.split(',')) for t in re.findall(r'\{(\d+, \d+, \d+)\}', mt.group(1))] if mt else []
check('PATTERN == frame_rig.cpp kCol', rig, [A.PATTERN[k] for k in A.KEYS])
mf = re.search(r'kFlat\[10\]\[3\] = \{(.*?)\};', src)
rigf = [tuple(int(x) for x in t.split(',')) for t in re.findall(r'\{(\d+, \d+, \d+)\}', mf.group(1))] if mf else []
check('FLAT == frame_rig.cpp kFlat', rigf, [A.FLAT[k] for k in A.KEYS])

print('arena_sheet_unit: %s (%d failed)' % ('PASS' if not fails else 'FAIL', len(fails)))
sys.exit(1 if fails else 0)
