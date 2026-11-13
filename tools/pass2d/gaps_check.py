"""S101 lane 23: the independent checks for the 2D pass's gap scenarios (tools/pass2d/gaps_test.cpp writes the pictures and layouts; selftest.py calls these).

Every check reads the exe's PNG (decoded here, not by the C++) and its layout JSON and compares them with a reading that does not use the pass's code:
  a4     the DDS bytes of the pac entry (tools/wm30/pac.py), decoded here as A4R4G4B4 and bilinearly sampled where the pixel is
  blend  closed-form blend formulas (straight, additive, opaque, premultiplied) over a known background
  gpu    the four colours of the picture the scene made, found at the quadrant centres of the sprite that shows it
  movie  the BT.601 limited-range formula with the game's own constants (docs/renderer/effects/ySprite.md), the fit rule, the alpha plane
  text   a wrap model built from the .yfb advances (selftest.read_font) and the rule in PASS2D.md; the clip box from the pixels
Each returns a list of violations (empty = the picture and the layout agree). Pictures hold game data: they stay under out/ (CLAUDE.md rule 1).
"""
import json
import os
import re
import struct
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / 'tools' / 'wm30'))
import pac  # noqa: E402  (the independent pac reader: XCTD -> EPAC -> PACH -> ZLIB / BPE)

SIZES = [(1280, 720), (2560, 1440), (3440, 1440)]
TOL = 3  # 8-bit levels: 4-bit textures are exact steps of 17, blends and bilinear weights are fixed point


# ---------------------------------------------------------------------------------------------------------------- PNG (the exe writes RGBA8, filter 0)
def read_png(path):
    d = Path(path).read_bytes()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', f'{path}: not a PNG'
    pos, idat, w, h = 8, b'', 0, 0
    while pos < len(d):
        n, t = struct.unpack_from('>I4s', d, pos)
        body = d[pos + 8:pos + 8 + n]
        if t == b'IHDR':
            w, h, depth, ctype = struct.unpack_from('>IIBB', body, 0)
            assert (depth, ctype) == (8, 6), f'{path}: not RGBA8'
        elif t == b'IDAT':
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    row = w * 4 + 1
    assert len(raw) == row * h, f'{path}: size mismatch'
    assert all(raw[y * row] == 0 for y in range(h)), f'{path}: a filtered row (the writer uses filter 0)'
    return w, h, raw, row


def pixel(img, x, y):
    w, h, raw, row = img
    x, y = max(0, min(w - 1, int(x))), max(0, min(h - 1, int(y)))
    o = y * row + 1 + x * 4
    return tuple(raw[o:o + 4])


def near(a, b, tol=TOL):
    return all(abs(int(p) - int(q)) <= tol for p, q in zip(a, b))


def rgb8(c):
    return tuple(int(round(max(0.0, min(1.0, v)) * 255)) for v in c)


def load(out_dir, scenario, w, h):
    j = json.loads((Path(out_dir) / f'gaps_{scenario}_{w}x{h}.json').read_text(encoding='utf-8'))
    return j, read_png(Path(out_dir) / f'gaps_{scenario}_{w}x{h}.png')


# ---------------------------------------------------------------------------------------------------------------- item 1: A4R4G4B4
def a4_ground_truth(game, pac_rel, top, leaf, name):
    """The DDS texture `name` of PACH #top / entry `leaf` of the pac, as (w, h, texels[y][x] = (r, g, b, a) 0..255), decoded from the file's own bytes:
    the DDS header must declare A4R4G4B4 (masks A F000, R 0F00, G 00F0, B 000F); a texel word's A is bits 15..12, R 11..8, G 7..4, B 3..0."""
    data = pac.load(str(game / pac_rel))
    pos, k = 0x4000, 0
    while pos + 8 <= len(data):
        if data[pos:pos + 4] != b'PACH':
            pos += 0x800
            continue
        count = struct.unpack_from('<I', data, pos + 4)[0]
        first = pos + 8 + 12 * count
        end = first
        for i in range(count):
            _, off, size = struct.unpack_from('<III', data, pos + 8 + 12 * i)
            end = max(end, first + off + size)
        if k == top:
            for path, dec in pac.walk(data[pos:end], f'p{top}'):
                if path != f'p{top}/{leaf}':
                    continue
                n = struct.unpack_from('<I', dec, 0)[0]
                for i in range(n):
                    r = 0x10 + 32 * i
                    nm = dec[r:r + 16].split(b'\0')[0].decode('latin1')
                    if nm.lower() != name.lower():
                        continue
                    size, off = struct.unpack_from('<II', dec, r + 20)
                    dds = dec[off:off + size]
                    assert dds[:4] == b'DDS ', 'not a DDS'
                    h, w = struct.unpack_from('<II', dds, 12)
                    fourcc, bits, rm, gm, bm, am = struct.unpack_from('<IIIIII', dds, 84)
                    assert (fourcc, bits, am, rm, gm, bm) == (0, 16, 0xF000, 0x0F00, 0x00F0, 0x000F), 'the DDS header does not declare A4R4G4B4'
                    words = struct.unpack_from(f'<{w * h}H', dds, 128)
                    texels = [[((wd >> 8 & 15) * 17, (wd >> 4 & 15) * 17, (wd & 15) * 17, (wd >> 12) * 17) for wd in words[y * w:(y + 1) * w]] for y in range(h)]
                    return w, h, texels
            raise AssertionError(f'{pac_rel} p{top}/{leaf}:{name} not found')
        k += 1
        pos = (end + 0x7FF) & ~0x7FF
    raise AssertionError(f'{pac_rel}: no PACH #{top}')


def bilinear(texels, w, h, u, v):
    """Texel-space (u, v) = (pixel centre / scale - 0.5), clamp to edge."""
    x0, y0 = int(u // 1), int(v // 1)
    fx, fy = u - x0, v - y0
    out = [0.0] * 4
    for dy, wy in ((0, 1 - fy), (1, fy)):
        for dx, wx in ((0, 1 - fx), (1, fx)):
            t = texels[max(0, min(h - 1, y0 + dy))][max(0, min(w - 1, x0 + dx))]
            for c in range(4):
                out[c] += t[c] * wx * wy
    return out


def check_a4(out_dir, truth):
    tw, th, texels = truth
    bad = []
    variety = sum(1 for y in range(0, th, 4) for x in range(0, tw, 4) if len(set(texels[y][x][:3])) > 1 or texels[y][x][3] not in (0, 255))
    if variety < 40:
        bad.append(f'a4: the texture has only {variety} sampled texels where the nibble order matters: a degenerate test')
    for w, h in SIZES:
        j, img = load(out_dir, 'a4', w, h)
        s = h / 720.0
        if j['problems']:
            bad.append(f'a4 {w}x{h}: problems {j["problems"][:2]}')
            continue
        x0, y0, rw, rh = j['sprites'][0]['rect']
        if abs(rw - tw * s) > 0.01 or abs(rh - th * s) > 0.01 or abs(x0) > 0.01 or abs(y0) > 0.01:
            bad.append(f'a4 {w}x{h}: sprite rect {j["sprites"][0]["rect"]}, expected 0,0,{tw * s},{th * s}')
            continue
        wrong = 0
        n = 0
        for py in range(1, int(th * s) - 1, 7):
            for px in range(1, int(tw * s) - 1, 5):
                exp = bilinear(texels, tw, th, (px + 0.5) / s - 0.5, (py + 0.5) / s - 0.5)
                got = pixel(img, px, py)
                n += 1
                if not near(got[:3], [round(v) for v in exp[:3]], TOL) or abs(got[3] - exp[3]) > TOL:
                    wrong += 1
        if wrong:
            bad.append(f'a4 {w}x{h}: {wrong} of {n} sampled pixels differ from the file\'s A4R4G4B4 texels (channel order or sampling)')
    return bad


# ---------------------------------------------------------------------------------------------------------------- item 4: blend
BG = (0.2, 0.4, 0.6)


def blend_expect(mode, tex, tint):
    """The closed forms. tex / tint are (r, g, b, a) 0..1. Returns the resulting rgb over BG."""
    sr = [tex[i] * tint[i] for i in range(4)]
    if mode == 'straight':
        return [sr[i] * sr[3] + BG[i] * (1 - sr[3]) for i in range(3)]
    if mode == 'additive':
        return [BG[i] + sr[i] * sr[3] for i in range(3)]
    if mode == 'opaque':
        return sr[:3]
    if mode == 'premult':  # the texture's colour already carries its alpha; the tint's alpha fades colour and alpha
        c = [tex[i] * tint[i] * tint[3] for i in range(3)]
        a = tex[3] * tint[3]
        return [c[i] + BG[i] * (1 - a) for i in range(3)]
    raise AssertionError(mode)


def check_blend(out_dir):
    half_red, half_white, white = (1, 0, 0, 0.5), (1, 1, 1, 0.5), (1, 1, 1, 1)
    solid, premult_tex = (1, 1, 1, 1), (128 / 255, 0, 0, 128 / 255)
    cases = [('straight', solid, half_red), ('additive', solid, half_red), ('opaque', solid, half_red), ('premult', premult_tex, white),
             ('premult', premult_tex, half_white), ('straight', premult_tex, white)]
    bad = []
    for w, h in SIZES:
        j, img = load(out_dir, 'blend', w, h)
        s = h / 720.0
        if j['problems']:
            bad.append(f'blend {w}x{h}: problems {j["problems"][:2]}')
            continue
        if not near(pixel(img, 5, 5)[:3], rgb8(BG), 2):
            bad.append(f'blend {w}x{h}: the background pixel is {pixel(img, 5, 5)}')
        for i, (mode, tex, tint) in enumerate(cases):
            cx, cy = (50 + 150 * i + 50) * s, 150 * s
            got = pixel(img, cx, cy)
            want = rgb8(blend_expect(mode, tex, tint))
            if not near(got[:3], want):
                bad.append(f'blend {w}x{h} sprite {i} ({mode}): pixel {got[:3]}, the formula says {want}')
    return bad


# ---------------------------------------------------------------------------------------------------------------- item 2: a surface the scene made
PICTURE = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)]  # top-left, top-right, bottom-left, bottom-right


def check_gpu(out_dir):
    bad = []
    for w, h in SIZES:
        j, img = load(out_dir, 'gpu', w, h)
        s = h / 720.0
        if j['problems']:
            bad.append(f'gpu {w}x{h}: problems {j["problems"][:2]}')
            continue
        if len(j['sprites']) != 2 or any(sp['source'] != 1 or sp['source_index'] != 0 for sp in j['sprites']):
            bad.append(f'gpu {w}x{h}: the sprites must both come from target 0 (a surface), the layout says {[(sp["source"], sp["source_index"]) for sp in j["sprites"]]}')
            # (no early exit: the pixels say the same thing on their own)
        # sprite 0: TopLeft (100, 100, 256 x 256); sprite 1: Right-anchored (1052, 100, 128 x 128)
        for label, (ox, oy, size) in (('picture', (100 * s, 100 * s, 256 * s)), ('picture_right', (w - 1280 * s + 1052 * s, 100 * s, 128 * s))):
            for q, (qx, qy) in enumerate(((0.25, 0.25), (0.75, 0.25), (0.25, 0.75), (0.75, 0.75))):
                got = pixel(img, ox + qx * size, oy + qy * size)
                if not near(got[:3], PICTURE[q], 2):
                    bad.append(f'gpu {w}x{h} {label} quadrant {q}: pixel {got[:3]}, the surface holds {PICTURE[q]} (magenta 255,0,255 = the stale file texture was drawn)')
    return bad


# ---------------------------------------------------------------------------------------------------------------- item 3: movies
QUAD = [(81, 90, 240), (145, 54, 34), (41, 240, 110), (235, 128, 128)]  # (Y, Cb, Cr), the same table as gaps_test.cpp


def yuv_to_rgb(y, cb, cr):
    """BT.601 limited range with the constants the game's own YUV sprite uses (docs/renderer/effects/ySprite.md)."""
    return [max(0, min(255, round(255 * v))) for v in (
        (y / 255) * 1.164 - 0.87075 + (cr / 255) * 1.596,
        ((y / 255) * 1.164 - 0.07275) - ((cb / 255) * 0.391 - 0.1955) - ((cr / 255) * 0.813 - 0.4065),
        (y / 255) * 1.164 - 1.08175 + (cb / 255) * 2.018)]


def check_movie(out_dir):
    bad = []
    for w, h in SIZES:
        j, img = load(out_dir, 'movie', w, h)
        s = h / 720.0
        if j['problems']:
            bad.append(f'movie {w}x{h}: problems {j["problems"][:2]}')
            continue
        fit = min(w / 64.0, h / 36.0)
        mw, mh = 64 * fit, 36 * fit
        mx, my = (w - mw) / 2, (h - mh) / 2
        full = next((m for m in j['movies'] if m['item'] == 0), None)
        host = next((m for m in j['movies'] if m['item'] == 1), None)
        if not full or not host:
            bad.append(f'movie {w}x{h}: the layout lacks a movie box')
            continue
        if any(abs(a - b) > 0.01 for a, b in zip(full['rect'], (mx, my, mw, mh))):
            bad.append(f'movie {w}x{h}: full-screen rect {full["rect"]}, the fit rule says {(mx, my, mw, mh)}')
        if any(abs(a - b) > 0.01 for a, b in zip(host['rect'], (900 * s, 40 * s, 320 * s, 180 * s))):
            bad.append(f'movie {w}x{h}: sprite movie rect {host["rect"]}')
        # the four quadrants of the backdrop (sample points clear of the borders and of the alpha movie's sprite)
        for q, (qx, qy) in enumerate(((0.25, 0.25), (0.55, 0.45), (0.25, 0.75), (0.75, 0.75))):
            got = pixel(img, mx + qx * mw, my + qy * mh)
            want = yuv_to_rgb(*QUAD[q])
            if not near(got[:3], want, 4):
                bad.append(f'movie {w}x{h} quadrant {q}: pixel {got[:3]}, BT.601 of Y,Cb,Cr {QUAD[q]} is {want}')
        # the bars of a wider screen show the clear colour
        if w > mw + 2 and not near(pixel(img, 10, h // 2)[:3], rgb8(BG), 2):
            bad.append(f'movie {w}x{h}: the bar pixel is {pixel(img, 10, h // 2)[:3]}, not the clear colour {rgb8(BG)}')
        # the alpha movie sits on a host sprite turned half a turn (its rotation is applied): the opaque half of its alpha plane (alpha 255, the movie's own
        # colour: Y,Cb,Cr of quadrant 0) is on the RIGHT, the transparent half (alpha 0) on the left shows the backdrop (quadrant 1)
        hx, hy, hw, hh = 900 * s, 40 * s, 320 * s, 180 * s
        left, right = pixel(img, hx + hw * 0.25, hy + hh * 0.75), pixel(img, hx + hw * 0.75, hy + hh * 0.75)
        if not near(right[:3], yuv_to_rgb(*QUAD[0]), 4):
            bad.append(f'movie {w}x{h}: the alpha movie\'s opaque half is {right[:3]}, expected {yuv_to_rgb(*QUAD[0])} (rotation not applied, or padded plane rows shown?)')
        if not near(left[:3], yuv_to_rgb(*QUAD[1]), 4):
            bad.append(f'movie {w}x{h}: the alpha movie\'s transparent half is {left[:3]}, expected the backdrop {yuv_to_rgb(*QUAD[1])}')
        # record order: the blue sprite listed after the host (same order) is drawn over the movie
        over = pixel(img, (1100 + 30) * s, (100 + 30) * s)
        if not near(over[:3], (0, 0, 255), 2):
            bad.append(f'movie {w}x{h}: the sprite listed after the movie host is {over[:3]}, not blue: the movie was drawn above it')
    return bad


# ---------------------------------------------------------------------------------------------------------------- item 5: wrapping and clip boxes
def advs_of(text, font, trim=2, kern=True):
    """Advances (sheet px) of each char of one logical line: w - trim - kern(this, next) (S101 U1, UI_PROMOTION_MAP.md section 6)."""
    out = []
    for i, c in enumerate(text):
        k = font.kerns.get((ord(c), ord(text[i + 1])), 0) if kern and i + 1 < len(text) else 0
        out.append(font[ord(c)][2] - trim - k)
    return out


def model_wrap(text, font, wrap, **rule):
    """The rule in PASS2D.md, written here from the stated words: break at spaces; a word joins the line when line + spaces + word fit; spaces at a
    break are dropped; a word wider than a line is cut between glyphs. Returns the lines as lists of (char, advance); the advances are those of the
    logical line (the pen rule of advs_of: the kern is with the next char of the logical line)."""
    # Stated rule for an indent (the spaces before a logical line's first word): kept only when it and the first word fit together on the line, else
    # dropped (a word that fits a line alone is never cut because of its indent).
    width = lambda s: sum(a for _, a in s)
    out = []
    for logical in text.split('\n'):
        av = advs_of(logical, font, **rule)
        items = list(zip(logical, av))
        pos = 0
        pairs = []
        for m in re.finditer(r'( *)([^ ]+)', logical):  # (spaces before, word) as slices of the items
            pairs.append((items[m.start(1):m.end(1)], items[m.start(2):m.end(2)]))
            pos = m.end()
        trailing = items[len(logical.rstrip(' ')):] if logical.rstrip(' ') != logical else []
        cur = None  # no line open
        for n, (sp, word) in enumerate(pairs):
            if cur is not None and width(cur) + width(sp) + width(word) > wrap:
                out.append(cur)
                cur = None
            if cur is None:
                cur = list(sp) if n == 0 and width(sp) + width(word) <= wrap else []
            else:
                cur = cur + list(sp)
            if width(cur) + width(word) <= wrap:
                cur = cur + list(word)
            else:  # wider than a line: cut between glyphs
                for it in word:
                    if cur and width(cur) + it[1] > wrap:
                        out.append(cur)
                        cur = []
                    cur = cur + [it]
        out.append((cur or []) + trailing)
    return out


def bright(p):
    return max(p[:3]) > 128


def check_text(out_dir, font):
    cell = max(g[3] for g in font.values())
    bad = []
    for w, h in SIZES:
        j, img = load(out_dir, 'text', w, h)
        s = h / 720.0
        if j['problems']:
            bad.append(f'text {w}x{h}: problems {j["problems"][:2]}')
            continue
        texts = j['texts']
        if len(texts) != 7:
            bad.append(f'text {w}x{h}: {len(texts)} texts')
            continue
        # per text: (size units, wrap units, align); the model works in glyph-sheet pixels, scaled by size. Text 5: the limit is the word + two spaces (its indent is four)
        indent_limit = sum(advs_of('ABCDEFGHIJKLM  ', font)) * 24.0 / cell
        specs = [(24, 500), (24, 260), (36, 0), (36, 0), (24, 300), (24, indent_limit), (24, 0)]
        for t, (size, wrap_u) in zip(texts, specs):
            scale = size * s / cell
            tag = f'text {w}x{h} "{t["utf8"][:14]}"'
            wrap = wrap_u * s / scale if wrap_u else 1e9  # in advance units
            lines = model_wrap(t['utf8'], font, wrap)
            if t['lines'] != len(lines):
                bad.append(f'{tag}: {t["lines"]} lines, the wrap model says {len(lines)} ({["".join(c for c, _ in l) for l in lines]})')
                continue
            if wrap_u and abs(t['wrap_px'] - wrap_u * s) > 0.01:
                bad.append(f'{tag}: wrap_px {t["wrap_px"]}')
            glyphs = t['glyphs']
            want = [(ord(c), li) for li, line in enumerate(lines) for c, _ in line]
            if [(g[0], g[1]) for g in glyphs] != want:
                bad.append(f'{tag}: the glyph / line assignment differs from the model')
                continue
            idx = 0
            for li, line in enumerate(lines):
                width = sum(a for _, a in line) * scale
                x0 = t['anchor_x'] - (width / 2 if t['align'] == 1 else width if t['align'] == 2 else 0)
                for c, ca in line:
                    g = glyphs[idx]
                    idx += 1
                    if abs(g[2] - x0) > 0.02 or abs(g[3] - (t['anchor_y'] + li * size * s)) > 0.02:
                        bad.append(f'{tag}: glyph {c!r} line {li} at {g[2]:.2f},{g[3]:.2f}, expected {x0:.2f},{t["anchor_y"] + li * size * s:.2f}')
                        li = None
                        break
                    x0 += ca * scale
                if li is None:
                    break
            # structural properties of a greedy wrap, read from the boxes (not from the model): no wrapped line is wider than the limit unless it is a single
            # glyph; no line starts with a space; no line could have taken the next line's first word
            if wrap_u and len(lines) > 1:
                for li, line in enumerate(lines):
                    lw = sum(a for _, a in line) * scale
                    if lw > wrap_u * s + 0.02 and len(line) > 1:
                        bad.append(f'{tag}: line {li} is {lw:.1f} px wide, the limit is {wrap_u * s:.1f}')
                    if li > 0 and line[0][0] == ' ':
                        bad.append(f'{tag}: line {li} starts with a space')
        # the clip box, from the pixels: text 2 is clipped to x 60..360, y 455..475 (units); text 3 is the same string unclipped
        clip = texts[2]
        want_clip = [int(60 * s // 1), int(455 * s // 1), -int(-(360 * s) // 1) - int(60 * s // 1), -int(-(475 * s) // 1) - int(455 * s // 1)]
        if not clip['has_clip'] or any(abs(a - b) > 1 for a, b in zip(clip['clip'], want_clip)):
            bad.append(f'text {w}x{h}: clip box {clip["clip"]}, expected about {want_clip}')
        cx0, cy0, cx1, cy1 = 60 * s, 455 * s, 360 * s, 475 * s
        band_y0, band_y1 = int(440 * s), int(500 * s)
        outside = inside = 0
        for y in range(band_y0, band_y1):
            for x in range(0, w, 1 if w < 2000 else 2):
                if bright(pixel(img, x, y)):
                    if cx0 - 1 <= x < cx1 + 1 and cy0 - 1 <= y < cy1 + 1:
                        inside += 1
                    else:
                        outside += 1
        if outside:
            bad.append(f'text {w}x{h}: {outside} text pixels outside the clip box')
        if inside < 20:
            bad.append(f'text {w}x{h}: only {inside} text pixels inside the clip box (nothing drawn?)')
        twin = sum(1 for y in range(int(560 * s), int(600 * s)) for x in range(int(cx1 + 8), w, 2) if bright(pixel(img, x, y)))
        if twin < 20:
            bad.append(f'text {w}x{h}: the unclipped twin does not reach past the box ({twin} pixels): the clip test cannot tell')
        # a clip box far beyond the display (1e30 units): the scissor is the whole display and the text is drawn
        huge = texts[6]
        if not huge['has_clip'] or huge['clip'] != [0, 0, w, h]:
            bad.append(f'text {w}x{h}: the huge clip box became {huge["clip"]}, expected [0, 0, {w}, {h}]')
        ink = sum(1 for y in range(int(680 * s), int(704 * s)) for x in range(int(60 * s), int(400 * s)) if bright(pixel(img, x, y)))
        if ink < 20:
            bad.append(f'text {w}x{h}: the text under the huge clip box is not drawn ({ink} pixels)')
    return bad
