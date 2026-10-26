r"""Untile a 1024x512 2-byte-per-pixel Xenos texture dump (GetTiledOffset2D, SDK texture/util.cpp) and report where
the non-zero pixels are: bounding box, row / column bands, and a PNG (non-zero = white) next to the dump.

    py -I tools\native\untile_dump.py DUMP.bin [WIDTH HEIGHT]   (Session 69)
"""
import sys
from PIL import Image

path = sys.argv[1]
w = int(sys.argv[2]) if len(sys.argv) > 2 else 1024
h = int(sys.argv[3]) if len(sys.argv) > 3 else 512
data = open(path, 'rb').read()
bpp_log2 = 1


def tiled(x, y, pitch):
    pitch = (pitch + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (bpp_log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << bpp_log2
    off = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return ((off & ~0x1FF) << 3) + ((y & 16) << 7) + ((off & 0x1C0) << 2) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (off & 0x3F)


img = Image.new('L', (w, h))
px = img.load()
nz = []
for y in range(h):
    for x in range(w):
        o = tiled(x, y, w)
        v = data[o] | data[o + 1]
        if v:
            px[x, y] = 255
            nz.append((x, y))
print(f'{path}: non-zero pixels {len(nz)} of {w * h} ({100 * len(nz) / (w * h):.2f}%)')
if nz:
    xs = [p[0] for p in nz]
    ys = [p[1] for p in nz]
    print(f'bounding box x {min(xs)}-{max(xs)}, y {min(ys)}-{max(ys)}')
    rows = sorted(set(ys))
    cols = sorted(set(xs))

    def bands(v):
        out, s, p = [], v[0], v[0]
        for a in v[1:]:
            if a != p + 1:
                out.append((s, p))
                s = a
            p = a
        out.append((s, p))
        return out
    print('row bands', bands(rows)[:12])
    print('col bands', bands(cols)[:12])
    vals = {}
    for x, y in nz[:20000]:
        o = tiled(x, y, w)
        k = data[o:o + 2].hex()
        vals[k] = vals.get(k, 0) + 1
    print('most common values', sorted(vals.items(), key=lambda kv: -kv[1])[:6])
img.save(path[:-4] + '_nonzero.png')
