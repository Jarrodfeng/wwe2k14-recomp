#!/usr/bin/env python3
r"""S103 T7: the STAGE MIRROR pairing tool. Pairs the game's in-between render targets with OUR maps by ROLE and says how they differ.

  py -I tools\scene\stage_mirror.py [DIR] [--frame F ...] [--out OUTDIR] [--json FILE]     # DIR = out\s103\stage_mirror (the dumps of --wwe2k14_dump_targets)
  py -I tools\scene\stage_mirror.py --page DUMP [--arena 4] [--out OUTDIR]                  # the L12 stage-page dump (stage_page_f<frame>.rgba8) as the `stage_page` role against the player's own page
  py -I tools\scene\stage_mirror.py --synthetic                                              # offline self-test (the gate): a known image against a scaled / shifted copy must report the scale / shift

Dumps: game_<role>.<n>_f<frame>.rgba8 and ours_<role>.<n>_f<frame>.rgba8 (u32 width, u32 height, RGBA8 rows), manifests manifest_game_f<frame>.txt / manifest_ours_f<frame>.txt.
Pairing: by ROLE and index (game role.n with ours role.n). When the game has more images of a role than we do (the shadow maps: props map, wrestler maps A / B) each of ours takes the game image it
matches best (lowest mean error) and the line says "best of k". A role only the game has, or only we have, is listed as UNPAIRED: a role the game has and we do not IS a finding.
Per paired role, written to OUTDIR (default DIR\sheets):  sheet_<role>.<n>_f<frame>.png = game | ours | diff (|difference| x2, labelled), and one line of numbers:
  size       both sizes (ours is resized to the game's for the comparison; `size ratio` is ours / game)
  level      mean of each image (RGB) and their ratio ours / game (the L12 question: "ours is 0.4x the game's")
  err        mean / max absolute difference (of 255) over RGB, unaligned
  IoU        of the two "ink" masks (pixels further than 10 % of the range from the image's background = its median), unaligned
  scale/shift  the ink boxes' size ratio (ours / game, x y) and centre offset in game pixels (ours - game), (the ink boxes' CENTRE offset, so a scale about the image centre also shows as a shift), then `aligned err` / `aligned IoU` after mapping ours' ink box onto the game's
Nothing from the game is written by this tool except what it is given under out\; ReadImage scales RGBA16F images by their peak on both sides, so compare SHAPE for 16F roles (the line says
nothing about absolute level there). Exit 1 only on a --synthetic failure or unreadable input."""
import argparse
import glob
import json
import os
import re
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
os.environ.setdefault('RDR_MAIN_CHECKOUT', r'C:\Users\Jarrod\wwe2k14-recomp')
MAIN = os.environ['RDR_MAIN_CHECKOUT']
INK = 0.10
NAME = re.compile(r'^(game|ours)_(.+)\.(\d+)_f(\d+)\.rgba8$')


def load_rgba8(path):
    raw = open(path, 'rb').read()
    w, h = struct.unpack_from('<II', raw, 0)
    px = np.frombuffer(raw, np.uint8, offset=8)
    if px.size != w * h * 4:
        raise SystemExit('FAIL  %s: %d bytes for %dx%d' % (path, px.size, w, h))
    return px.reshape(h, w, 4)


def resize_to(img, w, h):
    if img.shape[1] == w and img.shape[0] == h:
        return img
    return np.asarray(Image.fromarray(img).resize((w, h), Image.BILINEAR))


def ink_mask(img):
    """pixels further than INK of the range from the background (the median), over RGB."""
    f = img[..., :3].astype(np.float32)
    bg = np.median(f.reshape(-1, 3), axis=0)
    d = np.abs(f - bg).max(axis=2)
    top = float(d.max())
    if top < 4.0:
        return np.zeros(d.shape, bool)
    return d > INK * top


def box(mask):
    ys, xs = np.nonzero(mask)
    if ys.size == 0:
        return None
    return xs.min(), ys.min(), xs.max() + 1, ys.max() + 1


def iou(a, b):
    u = np.logical_or(a, b).sum()
    return float(np.logical_and(a, b).sum() / u) if u else 1.0


def err(a, b):
    d = np.abs(a[..., :3].astype(np.float32) - b[..., :3].astype(np.float32))
    return float(d.mean()), float(d.max())


def realign(img, sx, sy, dx, dy):
    """undo (ours = game scaled by (sx, sy) about its ink-box centre, then moved by (dx, dy)): map ours back onto the game's frame."""
    h, w = img.shape[:2]
    # ours(p) = game((p - c - d) / s + c)  =>  game(q) = ours((q - c) * s + c + d)
    cx, cy = w / 2.0, h / 2.0
    m = (sx, 0, cx - cx * sx + dx, 0, sy, cy - cy * sy + dy)
    return np.asarray(Image.fromarray(img).transform((w, h), Image.AFFINE, m, Image.BILINEAR))


def compare(game, ours):
    """numbers for one pair (ours resized to the game's size first)."""
    gh, gw = game.shape[:2]
    oh, ow = ours.shape[:2]
    o = resize_to(ours, gw, gh)
    r = {'game_size': [gw, gh], 'ours_size': [ow, oh], 'size_ratio': [ow / gw, oh / gh]}
    gm, om = game[..., :3].reshape(-1, 3).mean(0), o[..., :3].reshape(-1, 3).mean(0)
    r['mean_game'] = [float(x) for x in gm]
    r['mean_ours'] = [float(x) for x in om]
    r['level_ratio'] = [float(om[c] / gm[c]) if gm[c] > 0.5 else None for c in range(3)]
    r['err_mean'], r['err_max'] = err(game, o)
    gk, ok = ink_mask(game), ink_mask(o)
    r['iou'] = iou(gk, ok)
    r['ink_game'], r['ink_ours'] = int(gk.sum()), int(ok.sum())
    gb, ob = box(gk), box(ok)
    if gb and ob:
        gw_, gh_ = gb[2] - gb[0], gb[3] - gb[1]
        ow_, oh_ = ob[2] - ob[0], ob[3] - ob[1]
        sx, sy = ow_ / gw_, oh_ / gh_
        dx = (ob[0] + ob[2]) / 2.0 - (gb[0] + gb[2]) / 2.0
        dy = (ob[1] + ob[3]) / 2.0 - (gb[1] + gb[3]) / 2.0
        r['scale'] = [float(sx), float(sy)]
        r['shift'] = [float(dx), float(dy)]
        # the boxes are measured about the image centre in realign: use the game box centre as the scale centre, so move there
        aligned = realign_box(o, gb, ob)
        r['aligned_err_mean'], _ = err(game, aligned)
        r['aligned_iou'] = iou(gk, ink_mask(aligned))
    else:
        r['scale'] = r['shift'] = None
    return r, o


def realign_box(o, gb, ob):
    """map ours' ink box onto the game's ink box (per-axis scale + shift): the aligned image the `aligned` numbers use."""
    h, w = o.shape[:2]
    sx = (ob[2] - ob[0]) / float(gb[2] - gb[0])
    sy = (ob[3] - ob[1]) / float(gb[3] - gb[1])
    # game(q) = ours(ob.x0 + (q - gb.x0) * sx)
    m = (sx, 0, ob[0] - gb[0] * sx, 0, sy, ob[1] - gb[1] * sy)
    return np.asarray(Image.fromarray(o).transform((w, h), Image.AFFINE, m, Image.BILINEAR))


def text(r):
    s = '%dx%d vs %dx%d (ratio %.3f x %.3f); mean g %s o %s ratio %s; err mean %.2f max %.0f; IoU %.3f (ink g %d o %d)' % (
        r['game_size'][0], r['game_size'][1], r['ours_size'][0], r['ours_size'][1], r['size_ratio'][0], r['size_ratio'][1],
        '/'.join('%.1f' % v for v in r['mean_game']), '/'.join('%.1f' % v for v in r['mean_ours']),
        '/'.join('-' if v is None else '%.2f' % v for v in r['level_ratio']), r['err_mean'], r['err_max'], r['iou'], r['ink_game'], r['ink_ours'])
    if r['scale']:
        s += '; scale %.3f x %.3f shift %+.1f, %+.1f px; aligned err %.2f IoU %.3f' % (r['scale'][0], r['scale'][1], r['shift'][0], r['shift'][1], r['aligned_err_mean'], r['aligned_iou'])
    else:
        s += '; no ink in one of them (scale / shift not measured)'
    return s


def sheet(game, o, label, path):
    h, w = game.shape[:2]
    s = max(1, 512 // max(w, h)) if max(w, h) < 512 else 1
    d = np.abs(game[..., :3].astype(np.int16) - o[..., :3].astype(np.int16)) * 2
    diff = np.clip(d, 0, 255).astype(np.uint8)
    panels = [Image.fromarray(game[..., :3]), Image.fromarray(o[..., :3]), Image.fromarray(diff)]
    if s > 1:
        panels = [p.resize((w * s, h * s), Image.NEAREST) for p in panels]
    pw, ph = panels[0].size
    out = Image.new('RGB', (pw * 3 + 8, ph + 18), (20, 20, 20))
    dr = ImageDraw.Draw(out)
    for i, (p, t) in enumerate(zip(panels, ('game', 'ours', 'diff x2'))):
        out.paste(p, (i * (pw + 4), 18))
        dr.text((i * (pw + 4) + 2, 2), '%s  %s' % (t, label if i == 0 else ''), fill=(230, 230, 230))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    out.save(path)


def scan(d):
    found = {}
    for p in glob.glob(os.path.join(d, '*.rgba8')):
        m = NAME.match(os.path.basename(p))
        if not m:
            continue
        side, role, n, fr = m.group(1), m.group(2), int(m.group(3)), int(m.group(4))
        found.setdefault(fr, {}).setdefault(side, {}).setdefault(role, {})[n] = p
    return found


def read_last_writers(d, frame):
    """manifest_game_f<frame>.txt -> {(role, n): last writer role}. Column 8 (index 7); '-' / missing = unknown (never excludes)."""
    out = {}
    p = os.path.join(d, 'manifest_game_f%d.txt' % frame)
    if not os.path.isfile(p):
        return out
    for ln in open(p, encoding='utf-8'):
        c = ln.rstrip('\n').split('\t')
        if c[0] == 'game' and len(c) >= 9 and c[2].isdigit():
            out[(c[1], int(c[2]))] = c[7]
    return out


def pair_frame(side_game, side_ours, frame, outdir, lastw=None):
    """returns (paired lines, unpaired lines, json rows). A game image whose LAST writer is another role (a wrestler map B blurred in place, the plane mask taken over by its blur) shows that
    other pass, not this role's: it is skipped as a candidate and named."""
    lastw = lastw or {}
    lines, rows = [], []
    gl, ol = [], []
    for role in sorted(set(side_game) | set(side_ours)):
        g, o = side_game.get(role, {}), side_ours.get(role, {})
        if not g:
            ol.append('UNPAIRED ours-only: %s (%d image%s): we draw it, the game has none this frame' % (role, len(o), '' if len(o) == 1 else 's'))
            continue
        if not o:
            gl.append('UNPAIRED game-only: %s (%d image%s): the game has it, we do not (a finding)' % (role, len(g), '' if len(g) == 1 else 's'))
            continue
        for n in sorted(o):
            ours = load_rgba8(o[n])
            cands = [n] if (n in g and len(g) == len(o)) else sorted(g)
            best = None
            skipped = []
            for k in cands:
                if k not in g:
                    continue
                lw = lastw.get((role, k), '-')
                if lw not in ('-', role):
                    skipped.append('%s.%d (last written by %s)' % (role, k, lw))
                    continue
                r, ro = compare(load_rgba8(g[k]), ours)
                if best is None or r['err_mean'] < best[1]['err_mean']:
                    best = (k, r, ro)
            for sk in skipped:
                lines.append('SKIPPED game %s: the image shows another role\'s pass' % sk)
            if best is None:
                lines.append('UNPAIRED ours: %s.%d: no game image of the role to compare%s' % (role, n, ' (all were last written by another role)' if skipped else ''))
                continue
            k, r, ro = best
            tag = '%s.%d' % (role, n)
            how = '' if len(cands) == 1 else ' (game .%d, best of %d)' % (k, len(cands))
            sp = os.path.join(outdir, 'sheet_%s_f%d.png' % (tag, frame))
            sheet(load_rgba8(g[k]), ro, '%s f%d' % (tag, frame), sp)
            lines.append('%-18s%s: %s  -> %s' % (tag, how, text(r), sp))
            rows.append({'frame': frame, 'role': role, 'ours_n': n, 'game_n': k, **r})
    return lines, gl + ol, rows


def run_dir(d, frames, outdir, jpath):
    found = scan(d)
    if not found:
        print('FAIL  no game_/ours_ dumps in %s' % d)
        return 1
    allrows = []
    for fr in sorted(found):
        if frames and fr not in frames:
            continue
        side = found[fr]
        print('== frame %d: game roles %s | ours roles %s' % (fr, ', '.join(sorted(side.get('game', {}))) or '-', ', '.join(sorted(side.get('ours', {}))) or '-'))
        lines, unp, rows = pair_frame(side.get('game', {}), side.get('ours', {}), fr, outdir, read_last_writers(d, fr))
        for ln in lines + unp:
            print('  ' + ln)
        allrows += rows
    if jpath:
        json.dump(allrows, open(jpath, 'w'), indent=1)
    return 0


# ---- the offline proof ------------------------------------------------------------------------------------------------------------------
def synth_base(w=256, h=256):
    """a known image: a gradient floor with two soft discs and a bar (not symmetric, so a shift or scale is unambiguous)."""
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    img = np.full((h, w, 4), 255, np.float32)
    def disc(cx, cy, r, v):
        m = np.clip((r - np.hypot(xx - cx, yy - cy)) / 3.0, 0, 1)
        for c in range(3):
            img[..., c] = img[..., c] * (1 - m) + v * m
    disc(90, 100, 40, 20)
    disc(170, 150, 24, 60)
    img[60:70, 120:200, :3] = 90
    return np.clip(img, 0, 255).astype(np.uint8)


def synth_copy(base, scale, dx, dy, size=None):
    """base scaled about the image centre by `scale` then moved by (dx, dy) game pixels; optionally resampled to `size`."""
    h, w = base.shape[:2]
    cx, cy = w / 2.0, h / 2.0
    # out(p) = base((p - c - d) / s + c)
    m = (1 / scale, 0, cx - (cx + dx) / scale, 0, 1 / scale, cy - (cy + dy) / scale)
    im = Image.fromarray(base).transform((w, h), Image.AFFINE, m, Image.BILINEAR, fillcolor=(255, 255, 255, 255))
    if size:
        im = im.resize(size, Image.BILINEAR)
    return np.asarray(im)


def synthetic():
    fails = []

    def need(c, m):
        print(('ok    ' if c else 'FAIL  ') + m)
        if not c:
            fails.append(m)
    base = synth_base()
    r, _ = compare(base, base)
    need(r['err_mean'] < 0.01 and r['iou'] > 0.999 and abs(r['scale'][0] - 1) < 0.01 and abs(r['shift'][0]) < 0.6, 'identical: err 0, IoU 1, scale 1, shift 0 (err %.3f IoU %.3f)' % (r['err_mean'], r['iou']))
    # scaled copy (0.8x about the centre): the ink boxes must report ~0.8
    c = synth_copy(base, 0.8, 0, 0)
    r, _ = compare(base, c)
    need(abs(r['scale'][0] - 0.8) < 0.03 and abs(r['scale'][1] - 0.8) < 0.03, 'scaled 0.8x: reports scale %.3f x %.3f' % tuple(r['scale']))
    need(r['aligned_err_mean'] < r['err_mean'] * 0.7, 'scaled 0.8x: aligned err %.2f < unaligned %.2f' % (r['aligned_err_mean'], r['err_mean']))
    # shifted copy (+12, -7)
    c = synth_copy(base, 1.0, 12, -7)
    r, _ = compare(base, c)
    need(abs(r['shift'][0] - 12) < 1.5 and abs(r['shift'][1] + 7) < 1.5 and abs(r['scale'][0] - 1) < 0.03, 'shifted (+12,-7): reports shift %+.1f, %+.1f (scale %.3f)' % (r['shift'][0], r['shift'][1], r['scale'][0]))
    # scaled + shifted + a different size (ours at 128x128: the size ratio is 0.5)
    c = synth_copy(base, 1.25, -9, 5, size=(128, 128))
    r, _ = compare(base, c)
    need(abs(r['size_ratio'][0] - 0.5) < 1e-6, 'a 128x128 copy of a 256x256 image: size ratio %.3f' % r['size_ratio'][0])
    # `shift` is the ink-box CENTRE offset: scaling about the image centre moves the box centre by (s - 1) * (box centre - image centre) on top of the translation
    gb = box(ink_mask(base))
    ex = 0.25 * ((gb[0] + gb[2]) / 2.0 - 128) - 9
    ey = 0.25 * ((gb[1] + gb[3]) / 2.0 - 128) + 5
    need(abs(r['scale'][0] - 1.25) < 0.05 and abs(r['shift'][0] - ex) < 2.0 and abs(r['shift'][1] - ey) < 2.0,
         'scaled 1.25x + shifted (-9,+5) at half size: reports scale %.3f, box-centre shift %+.1f, %+.1f (expected %+.1f, %+.1f)' % (r['scale'][0], r['shift'][0], r['shift'][1], ex, ey))
    # a level change (the L12 case: ours is 0.4x as bright): the level ratio must say so
    dark = base.copy()
    dark[..., :3] = (dark[..., :3].astype(np.float32) * 0.4).astype(np.uint8)
    r, _ = compare(base, dark)
    need(all(abs(v - 0.4) < 0.02 for v in r['level_ratio']), 'a 0.4x level copy: level ratio %s' % '/'.join('%.2f' % v for v in r['level_ratio']))
    # an unrelated image must not look like a match
    other = np.full_like(base, 255)
    other[10:50, 10:50, :3] = 0
    r, _ = compare(base, other)
    need(r['iou'] < 0.2, 'an unrelated image: IoU %.3f' % r['iou'])
    # the whole directory path: files, pairing, unpaired roles, sheets
    import tempfile
    d = tempfile.mkdtemp(prefix='stage_mirror_syn_')
    def put(name, img):
        h, w = img.shape[:2]
        with open(os.path.join(d, name), 'wb') as f:
            f.write(struct.pack('<II', w, h))
            f.write(np.ascontiguousarray(img).tobytes())
    put('game_shadow_map.0_f100.rgba8', base)
    put('game_shadow_map.1_f100.rgba8', other)
    put('ours_shadow_map.0_f100.rgba8', synth_copy(base, 0.8, 4, 0))
    put('game_rim_map.0_f100.rgba8', base)           # the game has a rim map; ours has none: UNPAIRED game-only
    put('ours_circles.0_f100.rgba8', base)           # we have circles; the game has none: UNPAIRED ours-only
    lines, unp, rows = pair_frame(*[scan(d)[100][s] for s in ('game', 'ours')], 100, os.path.join(d, 'sheets'))
    need(len(rows) == 1 and rows[0]['role'] == 'shadow_map' and rows[0]['game_n'] == 0, 'two game shadow maps, one of ours: it takes the best match (game .%d)' % rows[0]['game_n'])
    need(any('game-only: rim_map' in u for u in unp) and any('ours-only: circles' in u for u in unp), 'unpaired roles are listed both ways (%d)' % len(unp))
    need(os.path.isfile(os.path.join(d, 'sheets', 'sheet_shadow_map.0_f100.png')), 'the game | ours | diff sheet is written')
    need(abs(rows[0]['scale'][0] - 0.8) < 0.05, 'through the directory path: scale %.3f' % rows[0]['scale'][0])
    # F2: the game's only plane image was last written by plane_blur: its pairing with ours' plane_mask is skipped and named, never compared
    put('game_plane_mask.0_f100.rgba8', base)
    put('ours_plane_mask.0_f100.rgba8', base)
    lines2, unp2, rows2 = pair_frame(*[scan(d)[100][s] for s in ('game', 'ours')], 100, os.path.join(d, 'sheets'), {('plane_mask', 0): 'plane_blur'})
    need(not any(x['role'] == 'plane_mask' for x in rows2) and any('SKIPPED game plane_mask.0 (last written by plane_blur)' in x for x in lines2), 'F2: a game image last written by another role is skipped and named')
    _, _, rows3 = pair_frame(*[scan(d)[100][s] for s in ('game', 'ours')], 100, os.path.join(d, 'sheets'), {('plane_mask', 0): 'plane_mask'})
    need(any(x['role'] == 'plane_mask' for x in rows3), 'F2 twin: the same pair is compared when its last writer is its own role')
    # F4: ours has two shadow maps, the game one: ours .1 has no equal-count 1:1 pair, so it is matched best-of; and a role whose every game image is skipped says UNPAIRED ours
    need(any(u.startswith('UNPAIRED ours: plane_mask.0') for u in lines2), 'F4: an ours image with no game image to compare prints UNPAIRED ours: role.n')
    put('ours_shadow_map.1_f100.rgba8', base)
    _, _, rows4 = pair_frame(*[scan(d)[100][s] for s in ('game', 'ours')], 100, os.path.join(d, 'sheets'))
    need(sorted(x['ours_n'] for x in rows4 if x['role'] == 'shadow_map') == [0, 1], 'F4: two ours shadow maps against two game shadow maps are both paired (n in g and equal counts = 1:1)')
    print('stage_mirror synthetic: %s' % ('FAIL' if fails else 'PASS'))
    return 1 if fails else 0


def page_mode(dump, arena, outdir):
    """the L12 stage-page dump as the `stage_page` role: ours = the player's own page (fade_lightmap01, then 00), resized to the dump's size."""
    sys.path.insert(0, HERE)
    import stage_page_check as spc
    game = spc.load_dump(dump)
    pages = spc.arena_pages(arena)
    fr = int(re.search(r'_f(\d+)', os.path.basename(dump)).group(1)) if re.search(r'_f(\d+)', os.path.basename(dump)) else 0
    rows = []
    for name in ('fade_lightmap01', 'fade_lightmap00'):
        o = pages[name]
        r, ro = compare(np.ascontiguousarray(game), np.ascontiguousarray(o))
        sp = os.path.join(outdir, 'sheet_stage_page_%s_f%d.png' % (name, fr))
        sheet(np.ascontiguousarray(game), ro, 'stage_page vs %s f%d' % (name, fr), sp)
        print('stage_page vs %-16s: %s  -> %s' % (name, text(r), sp))
        rows.append({'page': name, **r})
    return 0, rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir', nargs='?', default=os.path.join(MAIN, 'out', 's103', 'stage_mirror'))
    ap.add_argument('--frame', type=int, action='append')
    ap.add_argument('--out')
    ap.add_argument('--json')
    ap.add_argument('--page')
    ap.add_argument('--arena', type=int, default=4)
    ap.add_argument('--synthetic', action='store_true')
    a = ap.parse_args()
    if a.synthetic:
        sys.exit(synthetic())
    outdir = a.out or os.path.join(a.dir, 'sheets')
    if a.page:
        rc, _ = page_mode(a.page, a.arena, a.out or os.path.join(os.path.dirname(a.page), 'sheets'))
        sys.exit(rc)
    sys.exit(run_dir(a.dir, set(a.frame or []), outdir, a.json))


if __name__ == '__main__':
    main()
