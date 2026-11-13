"""Session 100: builds the game's composite attires from the files alone, the way the game builds them on the GPU at a character load
(docs\\systems\\attire_build.md: CEditExSSCompositTexture). For the roster census at file level: every wrestler's finished attire atlas
without a game run, as a contact sheet for the eye and as the expected picture for in-game checks.

    py -I tools\\assets\\attire_build.py [--ids 249,100] [--out out\\s100\\attires] [--sheet out\\s100\\attires\\sheet.png] [--full]

Inputs (the player's own files, read-only):
  - pac\\misc01_Start.pac, archive entry p0/600: the attire colour table. u16 record count, then one 664-byte record per character: u16 id at
    +0; four attire blocks of 132 bytes from +108; each block's 16 RGB byte triples at +4 = 4 mask steps x the colours for the mask's
    R, G, B, A channel (the split pass's g_CompR..A; code: sub_83418580 reads them at CData[+16] + attire x 132 + 1316 + 4). A block of
    all 0xFF is an unused attire.
  - pac\\ch\\chNNN.pac, first archive: entry 12 = the base texture ("color", DDS DXT1 2048x1024), entries 13-16 = the masks ("Mask1".."Mask4",
    DDS A4R4G4B4 2048x1024). Each entry is a small wrapper (name, "dds") and then a standard DDS file.
The build (checked against Heenan's in-game atlas, runs 6906 / 6911 / capture s100_heenan_build): the atlas starts as the base; for each
mask k in order, per texel the first non-zero mask channel in the order R, G, B, A picks that channel's colour of step k, scaled by the
channel's value (0-15 / 15), and replaces the texel's RGB (alpha untouched); all four zero leaves the texel.

Outputs (game-derived pictures: under out\\ only, never committed): one PNG per (id, attire) at half size, or full size with --full, and a
contact sheet with each picture's id, name and attire.
"""
import argparse
import json
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools', 'wm30'))
import pac  # noqa: E402

RECORD = 664
BLOCKS_AT = 108
BLOCK = 132


def colour_table():
    """{id: [attire 0..3: None or 16 (r, g, b) tuples]} from misc01_Start.pac p0/600."""
    data = pac.load(os.path.join(pac.GAME, 'pac', 'misc01_Start.pac'))
    table = None
    for name, dec in pac.entries(data):
        if name == 'p0/600':
            table = dec
            break
    if table is None:
        raise SystemExit('misc01_Start.pac has no p0/600')
    count = struct.unpack_from('<H', table, 0)[0]
    out = {}
    for k in range(count):
        rec = table[2 + RECORD * k: 2 + RECORD * (k + 1)]
        cid = struct.unpack_from('<H', rec, 0)[0]
        attires = []
        for a in range(4):
            blk = rec[BLOCKS_AT + BLOCK * a: BLOCKS_AT + BLOCK * (a + 1)]
            cols = blk[4:52]
            attires.append(None if all(c == 0xFF for c in cols) else [tuple(cols[3 * i: 3 * i + 3]) for i in range(16)])
        out[cid] = attires
    return out


def first_archive(cid):
    """{entry id: decoded bytes} of a character pac's first archive, top level."""
    data = pac.load(os.path.join(pac.GAME, 'pac', 'ch', 'ch%d.pac' % cid))
    pos = 0x4000 if data[:4] == b'EPAC' else 0
    while pos + 8 <= len(data) and data[pos:pos + 4] != b'PACH':
        pos += 0x800
    return {eid: pac.decode(raw) for eid, raw in pac.pach(data, pos)}


def dds(blob):
    """(width, height, pixel format dict, level-0 bytes) of a wrapped DDS."""
    at = blob.find(b'DDS ', 0, 0x100)
    if at < 0:
        raise ValueError('no DDS header')
    h = at + 4
    height, width = struct.unpack_from('<II', blob, h + 8)
    pf = h + 72
    flags, fourcc, bits, rm, gm, bm, am = struct.unpack_from('<I4sIIIII', blob, pf + 4)
    return width, height, {'flags': flags, 'fourcc': fourcc, 'bits': bits, 'masks': (rm, gm, bm, am)}, blob[h + 124:]


def decode_dxt1(data, width, height):
    """RGB (float 0..1, height x width x 3) of a DXT1 level."""
    bw, bh = width // 4, height // 4
    blocks = np.frombuffer(data, dtype='<u2', count=bw * bh * 4).reshape(bh, bw, 4)
    c0, c1 = blocks[..., 0].astype(np.uint32), blocks[..., 1].astype(np.uint32)
    idx = (blocks[..., 2].astype(np.uint32) | (blocks[..., 3].astype(np.uint32) << 16))

    def rgb(c):
        return np.stack([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31], -1).astype(np.float32)
    p0, p1 = rgb(c0), rgb(c1)
    four = (c0 > c1)[..., None]
    p2 = np.where(four, (2 * p0 + p1) / 3, (p0 + p1) / 2)
    p3 = np.where(four, (p0 + 2 * p1) / 3, 0)
    pal = np.stack([p0, p1, p2, p3], 2)  # bh, bw, 4, 3
    out = np.zeros((bh, 4, bw, 4, 3), np.float32)
    for y in range(4):
        for x in range(4):
            sel = (idx >> (2 * (4 * y + x))) & 3
            out[:, y, :, x, :] = np.take_along_axis(pal, sel[..., None, None].repeat(3, -1), 2)[:, :, 0, :]
    return out.reshape(height, width, 3) / 255


def decode_masks(data, width, height, masks):
    """[R, G, B, A] channel values 0..1 of a 16-bit DDS mask level, by its bit masks."""
    v = np.frombuffer(data, dtype='<u2', count=width * height).reshape(height, width).astype(np.uint32)
    chans = []
    for m in masks:
        shift = (m & -m).bit_length() - 1 if m else 0
        top = m >> shift if m else 1
        chans.append(((v & m) >> shift).astype(np.float32) / top if m else np.zeros_like(v, np.float32))
    return chans


def build(cid, colours, entries, white='keep'):
    """The finished atlas (float RGB) of one attire: the base, then each mask's step. white: what a colour of 255 does: 'keep' (the
    default) leaves the texel as it was (the channel still claims it, so later channels of that step do not paint it); 'skip-steps'
    skips only steps whose four colours are all 255 and paints single 255 colours white; 'paint' paints every 255 white (the shader
    with the colour as a constant). Unconfirmed which the game does: no test for 255 in the build's functions; Heenan's 255 colours touch
    no texel, so his in-game atlas does not tell; Jarrod (2026-11-10): Undertaker's and Stone Cold's attires are black, and painting gave
    them large white garments ('keep' gives them black)."""
    w, h, pf, data = dds(entries[12])
    if pf['fourcc'] != b'DXT1':
        raise ValueError('base is %r, not DXT1' % pf['fourcc'])
    atlas = decode_dxt1(data, w, h)
    steps = 0
    for k, eid in enumerate(e for e in (13, 14, 15, 16) if e in entries):
        mw, mh, mpf, mdata = dds(entries[eid])
        if (mw, mh) != (w, h) or mpf['bits'] != 16:
            raise ValueError('mask %d is %dx%d %d-bit' % (eid, mw, mh, mpf['bits']))
        if white != 'paint' and all(colours[4 * k + c] == (255, 255, 255) for c in range(4)):
            continue
        chans = decode_masks(mdata, mw, mh, mpf['masks'])
        done = np.zeros((h, w), bool)
        for c in range(4):
            pick = ~done & (chans[c] != 0)
            if not (white == 'keep' and colours[4 * k + c] == (255, 255, 255)):
                col = np.array(colours[4 * k + c], np.float32) / 255
                atlas[pick] = chans[c][pick][:, None] * col
            done |= pick
        steps += 1
    return atlas, steps


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--ids', help='comma-separated character ids (default: every id in the colour table)')
    ap.add_argument('--out', default=os.path.join(REPO, 'out', 's100', 'attires'))
    ap.add_argument('--sheet', help='contact sheet path (default: OUT\\sheet.png)')
    ap.add_argument('--full', action='store_true', help='write full-size atlases (default half size)')
    ap.add_argument('--white', choices=('keep', 'skip-steps', 'paint'), default='keep',
                    help='a colour of 255: keep the texel (default), skip only all-255 steps, or paint white (see build(); unconfirmed)')
    a = ap.parse_args()
    table = colour_table()
    names = {e['id']: e['name'] for e in json.load(open(os.path.join(REPO, 'db', 'ids', 'characters.json'), encoding='utf-8'))['entries']}
    ids = [int(x) for x in a.ids.split(',')] if a.ids else sorted(table)
    os.makedirs(a.out, exist_ok=True)
    tiles = []
    report = []
    for cid in ids:
        attires = table.get(cid)
        if attires is None:
            report.append('%d: no colour record' % cid)
            continue
        try:
            entries = first_archive(cid)
        except (OSError, SystemExit) as e:
            report.append('%d: no pac (%s)' % (cid, e))
            continue
        masks = [e for e in (13, 14, 15, 16) if e in entries]
        for ai, colours in enumerate(attires):
            if colours is None:
                continue
            try:
                atlas, steps = build(cid, colours, entries, a.white)
            except (ValueError, KeyError) as e:
                report.append('%d attire %d: %s' % (cid, ai, e))
                continue
            img = Image.fromarray((atlas.clip(0, 1) * 255 + 0.5).astype(np.uint8))
            if not a.full:
                img = img.resize((img.width // 2, img.height // 2), Image.BILINEAR)
            path = os.path.join(a.out, 'attire_%d_%d.png' % (cid, ai))
            img.save(path)
            label = '%d %s a%d (%d masks)' % (cid, names.get(cid, '?'), ai, steps)
            tiles.append((label, img))
            report.append('%d attire %d: %s, %d mask steps (pac masks %s)' % (cid, ai, names.get(cid, '?'), steps, masks))
    for line in report:
        print(line)
    if tiles:
        tw, th, cols = 256, 128, 6
        rows = (len(tiles) + cols - 1) // cols
        sheet = Image.new('RGB', (cols * tw, rows * (th + 14)), (30, 30, 30))
        d = ImageDraw.Draw(sheet)
        for i, (label, img) in enumerate(tiles):
            x, y = (i % cols) * tw, (i // cols) * (th + 14)
            sheet.paste(img.resize((tw, th), Image.BILINEAR), (x, y + 14))
            d.text((x + 3, y + 1), label[:40], fill=(255, 220, 0))
        out = a.sheet or os.path.join(a.out, 'sheet.png')
        sheet.save(out)
        print('sheet: %s (%d attires)' % (out, len(tiles)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
