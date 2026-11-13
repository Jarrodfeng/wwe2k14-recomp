r"""Lane 62 (S101, lane UJ of docs\renderer\UI_PROMOTION_MAP.md section 9): the UI join census. Offline, 0 game runs.

Re-measures, with a committed tool, every [C] of the map's sections 1-2 and 6 on case zips / capture folders, against the textures and fonts read straight from
the player's own pacs (tools\ui\ui_pacs.py): what the 2D draws are, whether a texture handle key names one image and an image one key, which pac leaves each
2D-bound image equals byte for byte (the class), the font sheets, every glyph's uv against the .yfb record, the pen advance rule, the scales, the icon-sheet
rects, and (match zips) a search of every pac for the images no menu pac holds.

  py -I tools\ui\ui_join_census.py CAPTURE:KIND [CAPTURE:KIND ...] [--frames STEP] [--plant NAME] [--json OUT] [--no-search] [--reindex] [-q]
CAPTURE = a case zip (capture\ inside it) or a capture folder. KIND = menu | editor | match (default menu): menu / editor zips must join every sprite and glyph image
to a pac leaf; a match zip only reports (the HUD's art is the open question: its search is the answer).
Checks (each prints PASS / FAIL with counts; the exit code is 1 when any FAILs or an input cannot be read):
  index          every pac under game_data\extracted\pac was read (no error), textures were found.
  names          the 2D draws (op 19 at the six call sites of the map's 1.1) carry their origin: claimed > 0.
  key_image      a handle key (origin part) names one image and an image has one key (textured sprite / glyph draws).
  content_once   no 2D-bound image has a second content version in any frame of the capture.
  join           menu / editor: every sprite and glyph image equals (all captured bytes) a texture in a pac leaf; the others are listed as "not found".
  join_bytes     the join table (key -> pac leaf) re-read through the pac reader equals the capture's bytes for every key (the independent check).
  fonts          every glyph-site image is a fontHD sheet (a glyph sheet or an icon sheet).
  glyph_uv       every glyph quad on a glyph sheet has a uv rect equal to a .yfb record (code); icon-sheet rects are listed.
  scale          glyph quad width = record width x scale (scale = quad height / record height), within 0.05 px.
  advance        the pen rule: next left = this left + (w - 2 - kern) x scale is the largest group and covers most of the clean adjacent pairs.
  match_search   (match, or any unjoined image) the unjoined images searched in every pac (DDS hash, then a raw window search): found / not found.
--plant (tests only): key_shift (the join table off by one key), names_stripped (origins removed), corrupt_blob (the first sprite image's bytes corrupted: must be "not found").
Numbers only. Nothing from the game is written (the pac index under out\ holds hashes); --json is for under out\."""
import argparse
import collections
import json
import os
import re
import struct
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'arena'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'solver'))
import join_census as jc  # noqa: E402  (Capture, CensusError)
import ui_pacs as up  # noqa: E402

SITES = {0x82BC5688: 'sprite', 0x82BC3784: 'glyph', 0x82BC8BEC: 'spriteM', 0x82BC3EE8: 'rect', 0x82BC484C: 'gradient', 0x82BC7E50: 'movie'}
TEXTURED = ('sprite', 'glyph', 'spriteM')
PLANTS = ('key_shift', 'names_stripped', 'corrupt_blob')
SEC_WHERE = 68
MENU_PACS = ('menu/MnLayHD.pac', 'menu/menuHD.pac', 'menu/MnUIHD.pac')
TOL = 0.05


class Result:
    def __init__(self, quiet):
        self.checks, self.quiet = [], quiet

    na = ()

    def add(self, name, ok, detail):
        if name in self.na:  # a match zip with no glyph draws: these four checks have nothing to measure (menu / editor zips never get this)
            ok, detail = True, 'n/a (match zip, no glyph draws): ' + detail
        self.checks.append((name, ok, detail))
        print('%s  %-14s %s' % ('PASS' if ok else 'FAIL', name, detail))

    def info(self, text):
        if not self.quiet:
            print('      ' + text)

    @property
    def ok(self):
        return all(c[1] for c in self.checks)


# ---------------------------------------------------------------- reading a capture ------------------------------------------------------------------
def blob_info(cap, image, cache):
    """The latest content of an image (textures.bin RCTX blob): dict(fmt, subs, data_len, h_full, mip0 (w, h, bytes hash, len), key) or None; cached by (offset, size)."""
    c = cap.contents.get(image)
    if c is None:
        return None
    if c in cache:
        return cache[c]
    blob = bytes(cap.tex[c[0]:c[0] + c[1]])
    info = None
    if len(blob) >= 24 and blob[:4] == b'RCTX':
        _, img, cnt, fmt, db = struct.unpack_from('<IIIIQ', blob, 0)
        subs = [struct.unpack_from('<IIIIIIQ', blob, 24 + 32 * k) for k in range(cnt)]  # mip, layer, w, h, row_bytes, rows, offset
        start = 24 + 32 * cnt
        data = blob[start:start + db]
        m0 = next((s for s in subs if s[0] == 0 and s[1] == 0), None)
        if m0 is not None and len(data) >= m0[6] + m0[4] * m0[5]:
            mip0 = data[m0[6]:m0[6] + m0[4] * m0[5]]
            info = dict(fmt=fmt, subs=subs, mips=len({s[0] for s in subs}), data_len=len(data), h_full=up.h8(data), w=m0[2], h=m0[3], mip0_len=len(mip0),
                        h_mip0=up.h8(mip0), off=c[0] + start, db=db)
    cache[c] = info
    return info


def contents_history(cap):
    """{image: {(offset, size)}} over EVERY frame of the capture (not only the sampled ones): the content versions an image had."""
    hist = collections.defaultdict(set)
    for name in cap.frames:
        buf = cap.read(name)
        magic, ver, cf, nsec, gf, pi, flags = struct.unpack_from('<IIIIQII', buf, 0)
        for k in range(nsec):
            sid, es, cnt, off = struct.unpack_from('<IIQQ', buf, 32 + 24 * k)
            if sid == jc.SEC_CONTENTS and es == jc.CONT_DT.itemsize:
                for c in np.frombuffer(buf, jc.CONT_DT, cnt, off):
                    hist[int(c['image'])].add((int(c['off']), int(c['size'])))
    return hist


def frame_where(f):
    for k in range(struct.unpack_from('<I', f.buf, 12)[0]):
        sid, es, cnt, off = struct.unpack_from('<IIQQ', f.buf, 32 + 24 * k)
        if sid == SEC_WHERE and es == 24 and cnt:
            addr, words, menu_id, cam, clock = struct.unpack_from('<IIIIQ', f.buf, off)
            return (f.string(words) or ''), (menu_id if menu_id != 0xFFFFFFFF else -1)
    return None, -1


def draw_quad(cap, f, k):
    """The 4 inline vertices of a 2D draw: dict(x0, x1, y0, y1, u0, u1, v0, v1, ok) or None. Vertex order is a strip: TL, TR, BL, BR."""
    d = f.draws[k]
    if int(d['vertex_count']) != 4:
        return None
    L = f.layouts[int(d['layout'])]
    stride = int(L['stride'])
    els = f.elements[int(L['first']):int(L['first']) + int(L['count'])]
    pos = next((int(e['off']) for e in els if int(e['fmt']) in (106, 109)), None)
    uv = next((int(e['off']) for e in els if int(e['fmt']) == 103), None)
    raw = cap.geo[int(d['vertex_offset']):int(d['vertex_offset']) + int(d['vertex_bytes'])]
    if pos is None or raw.size < 4 * stride:
        return None
    rows = raw[:4 * stride].reshape(4, stride)
    p = np.ascontiguousarray(rows[:, pos:pos + 12]).view('<f4').reshape(4, 3)
    q = dict(x0=float(p[0, 0]), x1=float(p[1, 0]), y0=float(p[0, 1]), y1=float(p[2, 1]))
    q['ok'] = bool(abs(p[0, 1] - p[1, 1]) < 1e-3 and abs(p[0, 0] - p[2, 0]) < 1e-3 and abs(p[1, 0] - p[3, 0]) < 1e-3 and abs(p[2, 1] - p[3, 1]) < 1e-3)
    if uv is not None:
        t = np.ascontiguousarray(rows[:, uv:uv + 8]).view('<f4').reshape(4, 2)
        q.update(u0=float(t[0, 0]), u1=float(t[1, 0]), v0=float(t[0, 1]), v1=float(t[2, 1]))
    return q


def read_capture(path, step, plant):
    """Sampled frames -> rows of 2D draws + per-frame counts, plus the whole-capture content history. Everything numeric."""
    cap = jc.Capture(path)
    n = len(cap.frames)
    sample = list(range(0, n, step)) if step else list(range(n))
    rows, frames = [], []
    cache = {}
    bound = {}  # image -> blob info at its last sampled use
    for i in sample:
        cap.catch_up(i)
        f = cap.frame(i, plant if plant == 'no_names' else None)
        if plant == 'names_stripped':
            f.has_origins, f.origins = False, None
        cap.advance_contents(f, i)
        where, menu = frame_where(f)
        fr = dict(i=i, number=f.number, draws=len(f.draws), where=where, menu=menu, kinds=collections.Counter(), claimed=0)
        if f.origins is not None:
            for k in range(len(f.draws)):
                og = f.origins[k]
                site = SITES.get(int(og['call_site']))
                if site is None or int(og['op']) != 19:
                    continue
                d = f.draws[k]
                fr['kinds'][site] += 1
                fr['claimed'] += 1
                img = None
                if int(d['tex_count']) > 0:
                    img = int(f.slots[int(d['tex_first'])]['image'])
                row = dict(frame=i, k=k, site=site, obj=int(og['object']), key=int(og['part']), image=img, klass=int(og['klass']), widget=int(og['widget']),
                           quad=draw_quad(cap, f, k) if site in TEXTURED else None)
                rows.append(row)
                if img is not None and site in TEXTURED:
                    bound[img] = blob_info(cap, img, cache)
        frames.append(fr)
    hist = contents_history(cap)
    return cap, dict(rows=rows, frames=frames, bound=bound, cache=cache, hist=hist, n=n, step=step)


# ---------------------------------------------------------------- the join ----------------------------------------------------------------------------
def build_lookup(index):
    lk = collections.defaultdict(list)
    for rel, ent in index.items():
        for t in ent['textures']:
            lk[(t[3], t[4], t[11])].append((rel, t))
    return lk


def join_image(info, lk):
    """info = a blob_info. -> ('exact' | 'mip0' | 'none', [(pac, leaf, offset, name)]). exact = every captured byte equals the leaf's texture data (same length);
    mip0 = only mip 0 equals (the capture holds a different mip chain)."""
    if info is None:
        return 'none', []
    exact, part = [], []
    for rel, t in lk.get((info['w'], info['h'], info['h_mip0']), []):
        (exact if (t[10] == info['h_full'] and t[8] == info['data_len']) else part).append((rel, t[0], t[1], t[2]))
    if exact:
        return 'exact', sorted(set(exact))
    return ('mip0', sorted(set(part))) if part else ('none', [])


def corrupt(cap, info, cache):
    """The plant: flip every 61st byte of the image's data in the capture's textures.bin copy (so no 64-byte window can equal the file)."""
    arr = np.array(cap.tex, copy=True)
    for i in range(info['off'], info['off'] + info['db'], 61):
        arr[i] ^= 0xFF
    cap._tex = arr
    cache.clear()


def pick(cands):
    pref = [c for c in cands if c[0] in MENU_PACS]
    return (pref or cands)[0]


def analyse(label, kind, path, step, plant, index, lk, fonts, args, R):
    t0 = time.time()
    print('== %s [%s] %s' % (label, kind, os.path.basename(path)))
    cap, S = read_capture(path, step, plant)
    rows, frames, bound, hist = S['rows'], S['frames'], S['bound'], S['hist']
    out = dict(label=label, kind=kind, frames=S['n'], sampled=len(frames), step=S['step'])

    # ---- counts per frame / screen (the map's 1.2) ----
    kinds = collections.Counter()
    for fr in frames:
        kinds.update(fr['kinds'])
    n2d = [sum(fr['kinds'].values()) for fr in frames]
    non2d = [fr['draws'] - sum(fr['kinds'].values()) for fr in frames]
    out['draws2d'] = dict(min=min(n2d), max=max(n2d), mean=round(float(np.mean(n2d)), 2), non2d_mean=round(float(np.mean(non2d)), 1),
                          kinds={k: round(v / len(frames), 2) for k, v in sorted(kinds.items())})
    R.info('frames %d (sampled %d, every %d): 2D draws a frame %d-%d mean %.1f; other draws mean %.1f; per frame %s' % (
        S['n'], len(frames), step or 1, min(n2d), max(n2d), np.mean(n2d), np.mean(non2d), out['draws2d']['kinds']))
    screens = collections.defaultdict(list)
    for fr in frames:
        screens[(fr['where'], fr['menu'])].append(fr)
    out['screens'] = {}
    for (w, m), frs in sorted(screens.items(), key=lambda x: -len(x[1]))[:8]:
        d2 = [sum(fr['kinds'].values()) for fr in frs]
        out['screens']['%s/menu %d' % (w, m)] = dict(frames=len(frs), mean2d=round(float(np.mean(d2)), 1), min=min(d2), max=max(d2))
        R.info('screen "%s" menu %d: %d frames, 2D %d-%d mean %.1f' % (w, m, len(frs), min(d2), max(d2), np.mean(d2)))
    claimed = sum(fr['claimed'] for fr in frames)
    R.add('names', claimed > 0, '%d 2D draws claimed by origin in %d frames (a stripped capture claims 0)' % (claimed, len(frames)))
    if claimed == 0:
        R.add('index', True, 'skipped: no claimed draws')
        return out

    # ---- glyph class / widget / sprite objects (1.2 bullets) ----
    g = [r for r in rows if r['site'] == 'glyph']
    out['glyph_klass'] = dict(collections.Counter(r['klass'] for r in g))
    out['with_widget'] = sum(1 for r in rows if r['widget'])
    sp = [r for r in rows if r['site'] == 'sprite']
    out['sprite_objects'] = len({r['obj'] for r in sp})
    out['sprite_keys'] = len({r['key'] for r in sp})
    R.info('glyph draws class %s; draws with a widget name: %d; sprite site: %d objects carry %d keys' % (out['glyph_klass'], out['with_widget'], out['sprite_objects'], out['sprite_keys']))

    # ---- key <-> image ----
    tex = [r for r in rows if r['site'] in TEXTURED and r['image'] is not None]
    img_of_key, key_of_img = collections.defaultdict(set), collections.defaultdict(set)
    for r in tex:
        img_of_key[r['key']].add(r['image'])
        key_of_img[r['image']].add(r['key'])
    bad_k = {k: v for k, v in img_of_key.items() if len(v) > 1}
    bad_i = {k: v for k, v in key_of_img.items() if len(v) > 1}
    pairs = {(r['obj'], r['key']) for r in sp}
    out['key_image'] = dict(keys=len(img_of_key), images=len(key_of_img), key_two_images=len(bad_k), image_two_keys=len(bad_i), sprite_obj_key_pairs=len(pairs))
    R.add('key_image', not bad_k and not bad_i, '%d keys <-> %d images (%d keys with two images, %d images under two keys); sprite (object, key) pairs %d' % (
        len(img_of_key), len(key_of_img), len(bad_k), len(bad_i), len(pairs)))

    # ---- content versions ----
    ver = {im: len({up.h8(bytes(cap.tex[o:o + s])) for (o, s) in hist.get(im, ())}) for im in key_of_img}
    multi = {im: v for im, v in ver.items() if v > 1}
    out['content_versions'] = dict(images=len(ver), with_second_version=len(multi))
    R.add('content_once', not multi, '%d 2D-bound images, %d with a second content version over all %d frames %s' % (len(ver), len(multi), S['n'], sorted(multi)[:6]))

    # ---- the join ----
    sprite_imgs = sorted({r['image'] for r in tex if r['site'] != 'glyph'})
    glyph_imgs = sorted({r['image'] for r in tex if r['site'] == 'glyph'})
    all_imgs = sorted(set(sprite_imgs) | set(glyph_imgs))
    R.na = ('fonts', 'glyph_uv', 'scale', 'advance') if kind == 'match' and not glyph_imgs else ()
    if plant == 'corrupt_blob' and sprite_imgs:
        corrupt(cap, bound[sprite_imgs[0]], S['cache'])
        for im in all_imgs:
            bound[im] = blob_info(cap, im, S['cache'])
        print('      (plant corrupt_blob: image %d bytes corrupted)' % sprite_imgs[0])
    joins = {im: join_image(bound.get(im), lk) for im in all_imgs}
    exact = {im for im, (st, _) in joins.items() if st == 'exact'}
    mip0 = {im for im, (st, _) in joins.items() if st == 'mip0'}
    none = [im for im, (st, _) in joins.items() if st == 'none']
    classes = collections.Counter(len(c) for st, c in joins.values() if st == 'exact')
    in_menu = sum(1 for im in exact if any(c[0] in MENU_PACS for c in joins[im][1]))
    pac_sets = collections.Counter(tuple(sorted({c[0] for c in joins[im][1]})) for im in exact)
    out['join'] = dict(sprite_images=len(sprite_imgs), glyph_images=len(glyph_imgs), exact=len(exact), mip0_only=len(mip0), not_found=len(none), in_menu_pacs=in_menu,
                       class_sizes={str(k): v for k, v in sorted(classes.items())}, max_class=max(classes) if classes else 0)
    R.info('images: %d sprite-site + %d glyph-site; exact %d (in MnLayHD / menuHD / MnUIHD: %d), mip0 only %d, not found %d; class sizes (leaves per image) %s' % (
        len(sprite_imgs), len(glyph_imgs), len(exact), in_menu, len(mip0), len(none), dict(sorted(classes.items()))))
    for ps, c in pac_sets.most_common(6):
        R.info('  %d images equal leaves in %s' % (c, list(ps)))
    for im in none:
        b = bound.get(im)
        R.info('  image %d: not found (%s)' % (im, 'no blob' if b is None else '%dx%d fmt %d mips %d %d bytes' % (b['w'], b['h'], b['fmt'], b['mips'], b['data_len'])))
    out['mip0_only'] = {}
    for im in sorted(mip0):
        b, cands = bound[im], joins[im][1]
        leaves = [(t[0], t[1]) for rel, t in lk.get((b['w'], b['h'], b['h_mip0']), [])]
        lens = sorted({t[8] for rel, t in lk.get((b['w'], b['h'], b['h_mip0']), [])})
        mips = sorted({t[7] for rel, t in lk.get((b['w'], b['h'], b['h_mip0']), [])})
        out['mip0_only'][str(im)] = dict(dims='%dx%d' % (b['w'], b['h']), capture_mips=b['mips'], capture_bytes=b['data_len'], leaf_mips=mips, leaf_bytes=lens,
                                         leaves=len(cands), pacs=sorted({c[0] for c in cands})[:8], first=['%s:%s%s' % (c[0], c[1], (':' + c[3]) if c[3] else '') for c in cands[:3]])
        R.info('  image %d: mip 0 equals %d leaves in %d pacs %s; capture %d mips %d bytes; those leaves: mips %s, bytes %s' % (
            im, len(cands), len({c[0] for c in cands}), sorted({c[0] for c in cands})[:5], b['mips'], b['data_len'], mips, lens[:4]))
    need_all = kind in ('menu', 'editor')
    R.add('join', (not none and not mip0) if need_all else True,
          '%d of %d images exact (%d sprite-site, %d glyph-site), mip0-only %d, not found %d%s' % (
              len(exact), len(all_imgs), len(sprite_imgs), len(glyph_imgs), len(mip0), len(none), '' if need_all else ' (match zip: reported, not required)'))

    # ---- the table key -> leaf, and its independent re-read ----
    table = {}
    for r in tex:
        st, c = joins.get(r['image'], ('none', []))
        if st in ('exact', 'mip0'):
            table[r['key']] = (r['image'],) + pick(c) + (st,)
    if plant == 'key_shift' and len(table) > 1:
        ks = sorted(table)
        old = dict(table)
        for a, b in zip(ks, ks[1:] + ks[:1]):
            table[a] = (old[a][0],) + old[b][1:]
    bad, checked = [], 0
    for key, (im, rel, leaf, off, name, st) in sorted(table.items(), key=lambda kv: (kv[1][1], kv[1][2])):
        want = bytes(cap.tex[bound[im]['off']:bound[im]['off'] + bound[im]['db']])
        got = up.read_dds(rel, leaf, off)
        checked += 1
        if st == 'mip0':  # only mip 0 is claimed equal
            want, got = want[:bound[im]['mip0_len']], (got or b'')[:bound[im]['mip0_len']]
        if got is None or got[:len(want)] != want or (st == 'exact' and len(got) != len(want)):
            bad.append((key, im, rel, leaf))
    out['join_bytes'] = dict(keys=len(table), checked=checked, bad=len(bad))
    R.add('join_bytes', bool(table) and not bad, '%d keys re-read through the pac reader, %d differ%s' % (checked, len(bad), '' if table else ' (no joined keys: nothing to check)'))
    out['table'] = {'%#x' % k: '%s:%s%s [%s]' % (v[1], v[2], (':' + v[4]) if v[4] else '', v[5]) for k, v in sorted(table.items())}

    # ---- fonts ----
    sheet_of = {}
    for fname, fd in fonts.items():
        for si, (leaf, hfull, lay) in enumerate(fd['sheets']):
            sheet_of[hfull] = (fname, si, leaf, lay)
    img_font = {}
    for im in glyph_imgs:
        b = bound.get(im)
        img_font[im] = sheet_of.get(b['h_full']) if b else None
    unknown = [im for im, v in img_font.items() if v is None]
    fn_counts = collections.Counter('%s/%s' % (v[0], 'glyphs' if v[1] == 0 else 'icons(%s)' % v[2]) for v in img_font.values() if v)
    out['fonts'] = dict(glyph_images=len(glyph_imgs), unknown=len(unknown), found=dict(fn_counts))
    R.add('fonts', not unknown and bool(glyph_imgs), '%d glyph-site images: %s; not a fontHD sheet: %d' % (len(glyph_imgs), dict(fn_counts), len(unknown)))

    # ---- glyph uv -> record, scale, advance ----
    rect_codes = {}
    for fname, fd in fonts.items():
        rc = collections.defaultdict(list)
        for c, r in fd['yfb']['rects'].items():
            rc[tuple(r)].append(c)
        rect_codes[fname] = rc
    n_glyph = n_known = n_icon = n_bad_uv = 0
    icon_rects = collections.Counter()
    scale_hist, wdev, wchk, wide, xr_hist = collections.Counter(), 0, 0, 0, collections.Counter()
    xr_by, adv_by = collections.Counter(), collections.Counter()
    glyph_rows = []
    for r in g:
        fi = img_font.get(r['image'])
        q = r['quad']
        if fi is None or q is None or 'u0' not in q:
            continue
        n_glyph += 1
        fname, si, leaf, lay = fi
        sw, sh = lay['w'], lay['h']
        rect = (round(q['u0'] * sw), round(q['v0'] * sh), round((q['u1'] - q['u0']) * sw), round((q['v1'] - q['v0']) * sh))
        exact_uv = all(abs(a - b) < 0.02 for a, b in zip((q['u0'] * sw, q['v0'] * sh, (q['u1'] - q['u0']) * sw, (q['v1'] - q['v0']) * sh), rect))
        if si > 0:
            n_icon += 1
            icon_rects[(fname, leaf) + rect] += 1
            continue
        codes = rect_codes[fname].get(rect)
        if codes and exact_uv:
            n_known += 1
            s = (q['y1'] - q['y0']) / rect[3] if rect[3] else 0
            xr = (q['x1'] - q['x0']) / (rect[2] * s) if rect[2] and s > 0 else 0
            rr = dict(r, rect=rect, codes=codes, font=fname, s=s, xr=xr)
            glyph_rows.append(rr)
            if s > 0 and q['ok'] and rect[2]:
                scale_hist[round(s, 2)] += 1
                wchk += 1
                wdev += abs((q['x1'] - q['x0']) - rect[2] * s) > TOL   # not the record width x scale
                wide += (q['x1'] - q['x0']) - rect[2] * s > TOL         # WIDER than it: never expected
                xr_hist[round(xr, 2)] += 1
                xr_by[(fname, round(s, 2), round(xr, 3))] += 1
        else:
            n_bad_uv += 1
    out['glyph_uv'] = dict(glyph_draws=n_glyph, record=n_known, icon_sheet=n_icon, no_record=n_bad_uv, ambiguous_codes=sum(1 for r in glyph_rows if len(r['codes']) > 1))
    R.add('glyph_uv', n_glyph > 0 and n_bad_uv == 0, '%d glyph-site quads: %d equal a .yfb record (%d rects shared by several codes), %d on an icon sheet, %d match no record' % (
        n_glyph, n_known, out['glyph_uv']['ambiguous_codes'], n_icon, n_bad_uv))
    out['icon_rects'] = {('%s %s x%d y%d w%d h%d' % k): v for k, v in sorted(icon_rects.items())}
    R.info('icon-sheet rects drawn: %d distinct (%s)' % (len(icon_rects), sorted({k[4:] for k in icon_rects})[:8]))
    out['scale'] = dict(quads=wchk, width_off=int(wdev), wider=int(wide), scales={str(k): v for k, v in sorted(scale_hist.items())},
                        x_ratio={str(k): v for k, v in sorted(xr_hist.items(), key=lambda x: -x[1])[:12]},
                        x_ratio_by_font_scale={'%s s%s x%s' % k: v for k, v in sorted(xr_by.items(), key=lambda x: -x[1])[:16]})
    R.info('x ratio by (font, scale, ratio): %s' % out['scale']['x_ratio_by_font_scale'])
    R.add('scale', wchk > 0 and wide == 0 and wdev <= 0.3 * wchk,
          '%d glyph quads: height = 40 x scale, scales %s; width = record width x scale within %.2f px for %d, NARROWER by a per-string factor for %d (x ratios, top: %s), wider for %d' % (
              wchk, dict(sorted(scale_hist.items())), TOL, wchk - wdev, wdev, dict(sorted(xr_hist.items(), key=lambda x: -x[1])[:6]), wide))

    # ---- the pen advance ----
    res_count, kern_n = collections.Counter(), collections.Counter()
    npairs = n_rule = n_rule_swapped = n_rule_nokern = 0
    pair_rows = []
    prev = None
    for r in glyph_rows:
        if prev is not None:
            q, p = r['quad'], prev['quad']
            same = (r['obj'] == prev['obj'] and r['key'] == prev['key'] and r['frame'] == prev['frame'] and abs(q['y0'] - p['y0']) < 0.01 and
                    abs(r['s'] - prev['s']) < 0.005 and abs(r['xr'] - prev['xr']) < 0.004 and r['xr'] > 0 and q['x0'] > p['x0'] and q['ok'] and p['ok'] and
                    len(r['codes']) == 1 and len(prev['codes']) == 1 and r['font'] == prev['font'])
            if same:
                a, b = prev['codes'][0], r['codes'][0]
                fd = fonts[r['font']]['yfb']['pairs']
                sx = prev['s'] * prev['xr']  # the horizontal scale: the vertical one times the string's squeeze (1.0 when none)
                delta = (q['x0'] - p['x0']) / sx - prev['rect'][2]
                kern = fd.get((a, b), 0)
                pair_rows.append((r['font'], sx, delta, kern, fd.get((b, a), 0)))
        prev = r
    # the per-font constant c (the pen overlap in sheet px): the mode of -(delta + kern) over the pairs; the rule is delta = -(c + kern)
    modes = {}
    for fname in {p[0] for p in pair_rows}:
        modes[fname] = collections.Counter(round(-(p[2] + p[3]) * 2) / 2 for p in pair_rows if p[0] == fname and abs(p[2]) < 20).most_common(1)[0][0]
    for fname, sx, delta, kern, kern_sw in pair_rows:
        c = modes[fname]
        npairs += 1
        ok_rule = abs(delta + c + kern) < TOL / sx
        adv_by[(fname, 'rule' if ok_rule else round(delta * 2) / 2)] += 1
        if ok_rule:
            n_rule += 1
            kern_n[(fname, kern)] += 1
        else:
            res_count[round(delta * 2) / 2] += 1
        n_rule_swapped += abs(delta + c + kern_sw) < TOL / sx
        n_rule_nokern += abs(delta + c) < TOL / sx
    near = sum(1 for p in pair_rows if abs(p[2]) < 20)
    out['advance'] = dict(pairs=npairs, pairs_within_a_word=near, rule=int(n_rule), constant_c_by_font=modes, rule_by_font_kern={'%s kern %d' % k: v for k, v in sorted(kern_n.items())},
                          rule_swapped_orientation=int(n_rule_swapped), rule_no_kern=int(n_rule_nokern), other_offsets={str(k): v for k, v in sorted(res_count.items(), key=lambda x: -x[1])[:10]})
    out['advance']['by_font'] = {'%s %s' % k: v for k, v in sorted(adv_by.items(), key=lambda x: -x[1])[:24]}
    R.info('advance by (font, delta group): %s' % out['advance']['by_font'])
    R.add('advance', npairs > 0 and n_rule >= 0.8 * near,
          '%d adjacent pairs (%d within a word, |delta| < 20 sheet px): %d follow next left = this left + (w - c - kern) x scale with c %s (by kern %s); with the pair orientation swapped %d, ignoring kern %d; other deltas (top) %s' % (
              npairs, near, n_rule, modes, dict(sorted(kern_n.items())), n_rule_swapped, n_rule_nokern, dict(sorted(res_count.items(), key=lambda x: -x[1])[:6])))

    # ---- the unjoined images: every pac ----
    out['not_found_images'] = len(none) + len(mip0)
    if none + sorted(mip0):
        if args.no_search:
            R.add('match_search', True, 'skipped (--no-search): %d unjoined images' % (len(none) + len(mip0)))
        else:
            out['search'] = search_unjoined(sorted(none + sorted(mip0)), bound, cap, index, R)
    else:
        R.add('match_search', True, 'nothing unjoined')
    out['seconds'] = round(time.time() - t0, 1)
    return out


def search_unjoined(imgs, bound, cap, index, R):
    """For each unjoined image: a raw window search (3 windows of 64 bytes of its mip 0, also 16-bit byte-swapped) in every decoded leaf of every pac."""
    needles, meta = [], {}
    for im in imgs:
        b = bound.get(im)
        if b is None:
            meta[im] = dict(status='no blob')
            continue
        mip0 = bytes(cap.tex[b['off']:b['off'] + b['mip0_len']])
        wins = []
        for frac in (0.25, 0.5, 0.75):
            p = (int(len(mip0) * frac) // 64) * 64
            for q in range(p, min(p + 64 * 64, len(mip0) - 63), 64):
                w = mip0[q:q + 64]
                if len(set(w)) > 8:
                    wins.append(w)
                    break
        if not wins:
            meta[im] = dict(status='uniform', dims='%dx%d' % (b['w'], b['h']))
            continue
        meta[im] = dict(status='searched', dims='%dx%d' % (b['w'], b['h']), fmt=b['fmt'], bytes=b['data_len'], windows=len(wins))
        for wi, w in enumerate(wins):
            needles.append(('%d:%d:raw' % (im, wi), w))
            needles.append(('%d:%d:swap16' % (im, wi), bytes(np.frombuffer(w, '<u2').byteswap().tobytes())))
    hits, errors, npacs = up.scan_needles(needles) if needles else ([], [], 0)
    found = collections.defaultdict(set)
    for nid, pac, leaf, off in hits:
        found[int(nid.split(':')[0])].add((pac, leaf, nid.split(':')[2]))
    summary = {}
    for im in imgs:
        m = meta[im]
        if m['status'] == 'searched':
            hs = sorted(found.get(im, ()))
            m['found'] = [list(h) for h in hs][:6]
            m['status'] = 'found in %d leaves of %s' % (len(hs), sorted({h[0] for h in hs})[:4]) if hs else 'not found in any pac'
        summary[str(im)] = m
        R.info('  unjoined image %d (%s): %s' % (im, m.get('dims', ''), m['status']))
    R.add('match_search', not errors, '%d unjoined images; %d pacs searched raw (%d errors); found %d, not found %d%s' % (
        len(imgs), npacs, len(errors), sum(1 for m in summary.values() if m['status'].startswith('found')),
        sum(1 for m in summary.values() if m['status'].startswith('not found')), ''.join('; ' + e[0] + ': ' + e[1] for e in errors[:3])))
    return summary


# ---------------------------------------------------------------- main --------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('captures', nargs='+')
    ap.add_argument('--frames', type=int, default=0, help='sample every STEP-th frame (default: about 250 frames per capture)')
    ap.add_argument('--plant', choices=PLANTS)
    ap.add_argument('--json')
    ap.add_argument('--no-search', action='store_true')
    ap.add_argument('--reindex', action='store_true')
    ap.add_argument('-q', '--quiet', action='store_true')
    args = ap.parse_args()
    R = Result(args.quiet)
    main_dir = jc.MAIN
    try:
        index = up.index_all(args.reindex)
        errs = [(k, e['error']) for k, e in index.items() if e['error']]
        ntex = sum(len(e['textures']) for e in index.values())
        R.add('index', not errs and ntex > 0, '%d pacs read, %d leaves, %.2f GB decoded, %d textures hashed; %d pacs with an error %s' % (
            len(index), sum(e['leaves'] for e in index.values()), sum(e['bytes'] for e in index.values()) / 1e9, ntex, len(errs), errs[:3]))
        lk = build_lookup(index)
        fonts = up.load_fonts()
        for fname, fd in fonts.items():
            y = fd['yfb']
            R.info('%s: %d glyphs, %d kerning pairs of %d tail records (%d other), sheets %s' % (fname, y['count'], len(y['pairs']), y['tail_records'], y['tail_other'],
                                                                                             [(s[0], '%dx%d %s' % (s[2]['w'], s[2]['h'], s[2]['fourcc'])) for s in fd['sheets']]))
        outs = []
        for spec in args.captures:
            m = re.match(r'^(.*):(menu|editor|match)$', spec)
            path, kind = (m.group(1), m.group(2)) if m else (spec, '')
            kind = kind or 'menu'
            if kind not in ('menu', 'editor', 'match'):
                raise jc.CensusError('%s: kind must be menu, editor or match' % spec)
            full = path if os.path.isabs(path) else os.path.join(main_dir, path)
            if not os.path.exists(full):
                raise jc.CensusError('%s: not found (a census that reads nothing must fail)' % full)
            outs.append(analyse(os.path.basename(os.path.dirname(full)) if full.lower().endswith('.zip') else os.path.basename(full), kind, full, args.frames or max(1, len(jc.Capture(full).frames) // 250), args.plant, index, lk, fonts, args, R))
        if args.json:
            os.makedirs(os.path.dirname(os.path.abspath(args.json)), exist_ok=True)
            json.dump(dict(captures=outs, checks=[list(c) for c in R.checks]), open(args.json, 'w'), indent=1, default=str)
    except jc.CensusError as e:
        print('FAIL  input: %s' % e)
        return 1
    except SystemExit as e:
        print('FAIL  input: %s' % e)
        return 1
    print('%s: %d checks, %d FAIL' % ('OK' if R.ok else 'FAILED', len(R.checks), sum(1 for c in R.checks if not c[1])))
    return 0 if R.ok else 1


if __name__ == '__main__':
    sys.exit(main())
