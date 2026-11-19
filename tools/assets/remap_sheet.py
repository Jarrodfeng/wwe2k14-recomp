"""S103 ASH: a contact sheet of the 12 remapped attires composed the way the stream-free loader does (offline, from the player's own files).

    py -I tools\\assets\\remap_sheet.py [--out out\\s103\\ash]

Per chosen key: the game's remap (misc01_Start.pac p0/600, rule of src\\assets\\attire_remap.h Resolve) gives the loaded key; the base (entry 12) and masks
(13..16) come from the PACH of that loaded key (named in the EPAC header at 0x80C, 16-byte records: name[8], LE32 sector from 0x4000); the 16 colours
are the chosen key's tens-digit slot of the record (+64 + 48 s = colour set 0 after "Default SS Change Color Model"), composed by attire_build.build.
Game-derived pictures: under out\\ only, never committed.
"""
import argparse
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import attire_build as ab  # noqa: E402

pac = ab.pac
KEYS = [(249, 'Heenan', [24912]), (104, 'Jericho', [10412, 10422, 10424]), (109, 'Hogan', [10914, 10934]), (131, 'CM Punk', [13112]),
        (197, 'Savage', [19712, 19722]), (253, 'Elizabeth', [25312, 25322, 25332])]
EXPECT = {24912: 24902, 10412: 10402, 10422: 10402, 10424: 10404, 10914: 10904, 10934: 10924, 13112: 13102, 19712: 19702, 19722: 19702,
          25312: 25302, 25322: 25302, 25332: 25302}


def records():
    data = pac.load(os.path.join(pac.GAME, 'pac', 'misc01_Start.pac'))
    tab = next(dec for name, dec in pac.entries(data) if name == 'p0/600')
    n = struct.unpack_from('<H', tab, 0)[0]
    return [tab[2 + ab.RECORD * k: 2 + ab.RECORD * (k + 1)] for k in range(n)]


def remap(recs, key):
    cid, s, kind = key // 100, (key // 10) % 10, key % 10
    r = next((x for x in recs if struct.unpack_from('<H', x, 0)[0] == cid and x[4 + s] == 1), None)
    if r is None or kind not in (2, 4):
        return key, s
    word, att = (r[24 + s], r[34 + s]) if kind == 2 else (r[44 + s], r[54 + s])
    if word not in (0, 1):
        return key, s
    return cid * 100 + att * 10 + (2 if word == 0 else 4), s


def pach_of_key(cid, key):
    data = pac.load(os.path.join(pac.GAME, 'pac', 'ch', 'ch%d.pac' % cid))
    for o in range(0x80C, 0x4000 - 15, 16):
        nm = data[o:o + 8]
        if not nm.isdigit():
            break
        if int(nm) == key:
            base = 0x4000 + struct.unpack_from('<I', data, o + 8)[0] * 0x800
            return {eid: pac.decode(raw) for eid, raw in pac.pach(data, base)}
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--out', default=os.path.join(ab.REPO, 'out', 's103', 'ash'))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    recs = records()
    table = ab.colour_table()
    tiles = []
    for cid, name, keys in KEYS:
        for key in keys:
            loaded, slot = remap(recs, key)
            note = '' if EXPECT[key] == loaded else ' MISMATCH(expected %d)' % EXPECT[key]
            ents = pach_of_key(cid, loaded)
            cols = table[cid][slot]
            if ents is None or cols is None:
                print('%s %d -> %d slot %d: %s' % (name, key, loaded, slot, 'no PACH' if ents is None else 'unused slot'))
                continue
            atlas, steps = ab.build(cid, cols, ents)
            img = Image.fromarray((atlas.clip(0, 1) * 255 + 0.5).astype(np.uint8)).resize((512, 256), Image.BILINEAR)
            img.save(os.path.join(a.out, 'tile_%d.png' % key))
            lab = '%s %d -> base %d, colour slot %d, %d mask steps%s' % (name, key, loaded, slot, steps, note)
            print(lab)
            tiles.append((lab, img))
    cols_n, tw, th = 3, 512, 256
    rows = (len(tiles) + cols_n - 1) // cols_n
    sheet = Image.new('RGB', (cols_n * tw, rows * (th + 18)), (30, 30, 30))
    d = ImageDraw.Draw(sheet)
    for i, (lab, img) in enumerate(tiles):
        x, y = (i % cols_n) * tw, (i // cols_n) * (th + 18)
        sheet.paste(img, (x, y + 18))
        d.text((x + 3, y + 3), lab, fill=(255, 220, 0))
    out = os.path.join(a.out, 'remap_sheet.png')
    sheet.save(out)
    print('sheet:', out)


if __name__ == '__main__':
    main()
