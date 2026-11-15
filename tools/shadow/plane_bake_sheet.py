r"""S102 lane L2: the sheets for Jarrod's eyes (the ring-plane shadow bake is a LOOK lever). fview headless on Check B under the shipped default bundle; 0 game runs.

  py -I tools\shadow\plane_bake_sheet.py [--out DIR]      (default: out\s102\sheets\l2 of the MAIN checkout)

Writes, under DIR:
  l2_mat_stills.png     per frame (100, 200, 250 and 300): the ring mat crop OFF | ON (1024) | ON (2048), contrast-stretched around the mat's light grey so the faint shadow reads, plus OFF - ON x8
  l2_mat_zoom.png       one frame, the mat's lower right, nearest-neighbour 3x: OFF | ON
  l2_swim.png           the same mat crop on 8 consecutive frames while the camera moves: OFF row, ON row (the rope shadow's swim against steady)
  l2_bake_maps.png      the bake's softened map (1024), the composite (512) and the game's own plane map (image 221 of that frame is read as the OFF picture's crop) for one frame
Numbers only in the log (no game assets are written anywhere but DIR: pictures of our own renderer's output, never committed)."""
import argparse
import os
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
BUNDLE = ['--wrestlers-ours', '--shadow-casters-ours', '--shadows-ours', '--rim-ours', '--circles-ours', '--rim-chain-ours', '--small-passes-ours', '--plane-shadow-ours']
STILLS = [100, 200, 250, 300]
SWIM = list(range(224, 232))
ZOOM = 200


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def dump(fv, game, cap, oracle, d, frames, extra):
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    cmd = [fv, '--capture', cap, '--game', game] + BUNDLE + ['--plane-oracle', oracle, '--validation', '1', '--frames', '341', '--dump-frames', ','.join(map(str, frames)), '--dump-dir', d] + extra
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    if 'validation errors 0' not in (p.stdout + p.stderr) or p.returncode:
        raise SystemExit('fview arm failed: ' + ' '.join(extra) + '\n' + (p.stdout + p.stderr)[-800:])


def img(d, f):
    return Image.open(os.path.join(d, 'frame_%05d.png' % f)).convert('RGB')


def stretch(im):
    a = np.asarray(im).astype(float)
    return Image.fromarray(np.clip((a - 120.0) * 1.9, 0, 255).astype(np.uint8))


def label(im, text):
    out = Image.new('RGB', (im.width, im.height + 18), (24, 24, 24))
    out.paste(im, (0, 18))
    ImageDraw.Draw(out).text((4, 3), text, fill=(235, 235, 235))
    return out


def hstack(ims, gap=8):
    h = max(i.height for i in ims)
    out = Image.new('RGB', (sum(i.width for i in ims) + gap * (len(ims) - 1), h), (24, 24, 24))
    x = 0
    for i in ims:
        out.paste(i, (x, 0))
        x += i.width + gap
    return out


def vstack(ims, gap=8):
    w = max(i.width for i in ims)
    out = Image.new('RGB', (w, sum(i.height for i in ims) + gap * (len(ims) - 1)), (24, 24, 24))
    y = 0
    for i in ims:
        out.paste(i, (0, y))
        y += i.height + gap
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=None)
    a = ap.parse_args()
    root = ROOT
    main_co = main_checkout(root)
    out = a.out or os.path.join(main_co, 'out', 's102', 'sheets', 'l2')
    os.makedirs(out, exist_ok=True)
    fv = os.path.join(root, 'out', 'renderer', 'dev', 'fview.exe')
    game = os.path.join(main_co, 'game_data', 'extracted')
    work = os.path.join(root, 'out', 'l2')
    cap, oracle = os.path.join(work, 'checkb', 'capture'), os.path.join(work, 'checkb.plane')
    if not (os.path.isdir(cap) and os.path.isfile(oracle)):
        raise SystemExit('run tools\\shadow\\plane_bake_check.py --keep first (it unpacks Check B and dumps the plane oracle into out\\l2)')
    frames = sorted(set(STILLS + SWIM + [ZOOM, 250]))
    arms = {'off': [], 'on': ['--plane-bake'], 'on2048': ['--plane-bake=2048'], 'dropropes': ['--draw-drop', '@class=all@object=0xaaf56230'], 'nobake': ['--plane-bake', '--plane-bake-fault', 'no_bake'],
            'half_off': ['--shadow-half'], 'half_on': ['--shadow-half', '--plane-bake']}
    for k, ex in arms.items():
        dump(fv, game, cap, oracle, os.path.join(work, 'sheet_' + k), frames, ex)
    D = {k: os.path.join(work, 'sheet_' + k) for k in arms}
    box = (0, 470, 1280, 720)
    rows = []
    for f in STILLS:
        o, n1, n2 = img(D['off'], f), img(D['on'], f), img(D['on2048'], f)
        diff = np.abs(np.asarray(o).astype(int) - np.asarray(n1).astype(int)).max(2) * 8
        dm = Image.fromarray(np.clip(diff, 0, 255).astype(np.uint8)).convert('RGB')
        c = [label(stretch(i.crop(box)), t) for i, t in ((o, 'OFF (the game\'s chain)  frame %d' % f), (n1, 'ON, bake 1024'), (n2, 'ON, bake 2048'))] + [label(dm.crop(box), 'OFF - ON, x8')]
        rows.append(vstack(c, 4))
    # 2x2 per frame would be wide; stack frames vertically, each frame's four crops in a 2 x 2 block
    blocks = []
    for r in rows:
        h4 = r.height // 4
        tiles = [r.crop((0, i * (h4), r.width, (i + 1) * h4)) for i in range(4)]
        blocks.append(vstack([hstack(tiles[:2], 6), hstack(tiles[2:], 6)], 6))
    still_sheet = vstack(blocks, 14)
    still_sheet.save(os.path.join(out, 'l2_mat_stills.png'))
    zbox = (700, 570, 1100, 700)
    zo, zn = img(D['off'], ZOOM).crop(zbox), img(D['on'], ZOOM).crop(zbox)
    z = hstack([label(stretch(zo).resize((zo.width * 3, zo.height * 3), Image.NEAREST), 'OFF, 3x'), label(stretch(zn).resize((zn.width * 3, zn.height * 3), Image.NEAREST), 'ON (bake 1024), 3x')], 8)
    z.save(os.path.join(out, 'l2_mat_zoom.png'))
    sbox = (60, 560, 700, 720)
    top = hstack([label(stretch(img(D['off'], f).crop(sbox)), 'OFF %d' % f) for f in SWIM[:4]], 4)
    top2 = hstack([label(stretch(img(D['off'], f).crop(sbox)), 'OFF %d' % f) for f in SWIM[4:]], 4)
    bot = hstack([label(stretch(img(D['on'], f).crop(sbox)), 'ON %d' % f) for f in SWIM[:4]], 4)
    bot2 = hstack([label(stretch(img(D['on'], f).crop(sbox)), 'ON %d' % f) for f in SWIM[4:]], 4)
    vstack([top, bot, top2, bot2], 8).save(os.path.join(out, 'l2_swim.png'))
    maps = []
    for name, size in (('bake_%05d.png' % ZOOM, 512), ('comp_%05d.png' % ZOOM, 512)):
        p = os.path.join(D['on'], name)
        if os.path.isfile(p):
            im = Image.open(p).convert('RGB').resize((size, size), Image.NEAREST)
            maps.append(label(Image.fromarray(np.clip(np.asarray(im).astype(float) * 8, 0, 255).astype(np.uint8)), name + '  (x8)'))
    if maps:
        hstack(maps, 8).save(os.path.join(out, 'l2_bake_maps.png'))
    # the far side of the mat (the comb Jarrod saw), the feet at contact, a wrestler across a rope line (3x, nearest)
    def pair(f, box, name, k=3):
        o, n = img(D['off'], f).crop(box), img(D['on'], f).crop(box)
        hstack([label(stretch(o).resize((o.width * k, o.height * k), Image.NEAREST), 'OFF frame %d' % f), label(stretch(n).resize((n.width * k, n.height * k), Image.NEAREST), 'ON frame %d' % f)], 8).save(os.path.join(out, name))
    pair(ZOOM, (560, 500, 960, 620), 'l2_far_side.png')
    pair(ZOOM, (260, 600, 520, 720), 'l2_feet_contact.png')
    pair(250, (200, 560, 560, 720), 'l2_wrestler_on_rope_line.png', 2)
    # the half rate on, during a move: eight frames, half-rate OFF over half-rate ON
    hbox = (60, 560, 700, 720)
    h1 = hstack([label(stretch(img(D['half_off'], f).crop(hbox)), 'half OFF %d' % f) for f in SWIM[:4]], 4)
    h2 = hstack([label(stretch(img(D['half_on'], f).crop(hbox)), 'half ON %d' % f) for f in SWIM[:4]], 4)
    h3 = hstack([label(stretch(img(D['half_off'], f).crop(hbox)), 'half OFF %d' % f) for f in SWIM[4:]], 4)
    h4 = hstack([label(stretch(img(D['half_on'], f).crop(hbox)), 'half ON %d' % f) for f in SWIM[4:]], 4)
    vstack([h1, h2, h3, h4], 8).save(os.path.join(out, 'l2_half_rate_move.png'))
    # the line-centre overlay: rope line centres found along rows of the mat, the game's (red) and the bake's (green), over the OFF picture
    sys.path.insert(0, HERE)
    import plane_bake_lines as pbl
    f = ZOOM
    name = 'frame_%05d.png' % f
    dg = pbl.gray(os.path.join(D['dropropes'], name)) - pbl.gray(os.path.join(D['off'], name))
    db = pbl.gray(os.path.join(D['nobake'], name)) - pbl.gray(os.path.join(D['on'], name))
    base = stretch(img(D['off'], f)).convert('RGB')
    dr = ImageDraw.Draw(base)
    for y in range(500, 716, 4):
        for centres, col in ((pbl.lines(np.convolve(dg[y], np.ones(3) / 3, 'same'), 1.5), (255, 40, 40)), (pbl.lines(np.convolve(db[y], np.ones(3) / 3, 'same'), 1.5), (40, 255, 40))):
            for c, w_, pk in centres:
                dr.ellipse((c - 2, y - 2, c + 2, y + 2), fill=col)
    base.crop((0, 470, 1280, 720)).save(os.path.join(out, 'l2_line_centres_overlay.png'))
    with open(os.path.join(out, 'KEEP'), 'w') as fh:
        fh.write('S102 lane L2 sheets for Jarrod: keep until 2026-11-20\n')
    for k in arms:
        shutil.rmtree(D[k], ignore_errors=True)
    print('wrote', out)


if __name__ == '__main__':
    main()
