"""Session 97: turn a tiled 360 texture page dumped with the input-script command `dump_mem` into a PNG, and compare it with
the file lightmap (`fade_lightmap00`, bg pac key 400) at the uvs where the ringside crowd samples it. Our code; reads only
the dump you give it and the player's own pac, prints numbers, writes only the PNG you ask for (never commit either).

  py -I tools\\assets\\untile_page.py DUMP.bin [--format 8888|dxt1] [--width 512 --height 512] [--order argb] [--no-swap]
        [--arena 5] [--set 20000] [--out page.png] [--ref-out lightmap.png]
  py -I tools\\assets\\untile_page.py --selftest

Memory layout (Xenos, from the SDK: thirdparty\\rexglue-sdk-src\\src\\graphics\\pipeline\\texture\\util.cpp GetTiledOffset2D, 32x32
texel tiles; DXT works in 4x4 blocks):
  8888  4 bytes per texel, pitch = width (multiple of 32), tiled. Guest memory is big-endian. --order names the byte order IN MEMORY
        (default 'argb' = D3D A8R8G8B8, the big-endian dword AARRGGBB). Unconfirmed for a resolve destination: the report prints the
        mean of each of the 4 byte lanes, so the lane that is alpha (constant) is visible; re-run with another --order.
  dxt1  8 bytes per 4x4 block, tiled over the block grid; guest memory swaps each 16-bit unit (--no-swap turns that off).
The base mip level starts at offset 0; anything after the first level (mips, tail) is ignored.

Crowd uvs: uv = (x / 5600, z / 5750) * 0.5 + 0.5 for the seated people of the arena's set (tools\\assets\\crowd.py rows/people),
v as texture rows (what the viewer does) and v flipped. The 'ratio' lines are page / file map at those points.
"""
import argparse
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, 'tools', 'wm30'))
sys.path.insert(0, HERE)

C184 = (1.0 / 5600.0, 1.0 / 5750.0)


def tiled_offsets(w, h, bpb_log2, pitch=None):
    """Byte offset of every element (x, y) of a w x h grid (elements = texels, or 4x4 blocks for DXT) in a tiled image.
    Port of GetTiledOffset2D (SDK texture/util.cpp), vectorised."""
    pitch = pitch or w
    pitch = (pitch + 31) & ~31
    y, x = np.mgrid[0:h, 0:w].astype(np.int64)
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (bpb_log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2
    off = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((off & ~0x1FF) << 3) + ((y & 16) << 7) + ((off & 0x1C0) << 2) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (off & 0x3F))


def untile_bytes(buf, w, h, bpb_log2, pitch=None):
    """-> array (h, w, 1 << bpb_log2) of uint8 from a tiled buffer."""
    off = tiled_offsets(w, h, bpb_log2, pitch)
    n = 1 << bpb_log2
    need = int(off.max()) + n
    if len(buf) < need:
        raise SystemExit('dump too short: %d bytes, the %dx%d image needs %d' % (len(buf), w, h, need))
    a = np.frombuffer(buf, dtype=np.uint8)
    return a[off[..., None] + np.arange(n)]


def tile_bytes(img, bpb_log2, pitch=None):
    """Inverse of untile_bytes (for the self test): img (h, w, n) -> tiled bytes."""
    h, w, n = img.shape
    off = tiled_offsets(w, h, bpb_log2, pitch)
    out = np.zeros(int(off.max()) + n, dtype=np.uint8)
    out[off[..., None] + np.arange(n)] = img
    return out.tobytes()


def decode_dxt1(blocks):
    """blocks (bh, bw, 8) little-endian standard DXT1 -> float RGBA (bh*4, bw*4, 4) in 0..1."""
    bh, bw, _ = blocks.shape
    c0 = blocks[..., 0].astype(np.uint32) | (blocks[..., 1].astype(np.uint32) << 8)
    c1 = blocks[..., 2].astype(np.uint32) | (blocks[..., 3].astype(np.uint32) << 8)
    idx = (blocks[..., 4].astype(np.uint32) | (blocks[..., 5].astype(np.uint32) << 8) |
           (blocks[..., 6].astype(np.uint32) << 16) | (blocks[..., 7].astype(np.uint32) << 24))

    def rgb(c):
        return np.stack([((c >> 11) & 31) * 255.0 / 31, ((c >> 5) & 63) * 255.0 / 63, (c & 31) * 255.0 / 31], -1)
    p0, p1 = rgb(c0), rgb(c1)
    four = (c0 > c1)[..., None]
    pal = np.stack([p0, p1,
                    np.where(four, (2 * p0 + p1) / 3, (p0 + p1) / 2),
                    np.where(four, (p0 + 2 * p1) / 3, np.zeros_like(p0))], -2)       # (bh, bw, 4, 3)
    alpha = np.ones((bh, bw, 4))
    alpha[..., 3] = np.where(c0 > c1, 1.0, 0.0)                                       # 3-colour mode: index 3 transparent
    out = np.zeros((bh * 4, bw * 4, 4))
    for py in range(4):
        for px in range(4):
            k = (idx >> (2 * (py * 4 + px))) & 3
            col = np.take_along_axis(pal, k[..., None, None].repeat(3, -1), -2)[..., 0, :]
            al = np.take_along_axis(alpha, k[..., None], -1)[..., 0]
            out[py::4, px::4, :3] = col
            out[py::4, px::4, 3] = al
    out[..., :3] /= 255.0
    return out


def load_page(path, fmt, w, h, order, swap):
    buf = open(path, 'rb').read()
    if fmt == 'dxt1':
        raw = untile_bytes(buf, w // 4, h // 4, 3)
        if swap:
            raw = raw.reshape(h // 4, w // 4, 4, 2)[..., ::-1].reshape(h // 4, w // 4, 8)
        return decode_dxt1(raw)
    raw = untile_bytes(buf, w, h, 2)                      # (h, w, 4) bytes in memory order
    lanes = {c: raw[..., i].astype(np.float64) / 255.0 for i, c in enumerate(order)}
    return np.stack([lanes['r'], lanes['g'], lanes['b'], lanes['a']], -1), raw


def file_lightmap(game, arena, name=b'fade_lightmap00'):
    import pac
    data = pac.load(os.path.join(game, 'pac', 'bg', 'bg%02d.pac' % arena))
    b = dict(pac.entries(data, 2))['p0/400']
    count = struct.unpack_from('<I', b, 0)[0]
    base = 0x10 + count * 0x20                           # records of 0x20: name[16], type[4], size, offset (offset is absolute)
    for i in range(count):
        r = 0x10 + i * 0x20
        if b[r:r + 16].split(b'\0')[0] == name:
            size, off = struct.unpack_from('<II', b, r + 20)
            dds = b[off:off + size]
            h, w = struct.unpack_from('<II', dds, 12)
            if dds[84:88] != b'DXT1':
                raise SystemExit('%s is not DXT1 (%r)' % (name, dds[84:88]))
            blocks = np.frombuffer(dds, np.uint8, (w // 4) * (h // 4) * 8, 128).reshape(h // 4, w // 4, 8)
            return decode_dxt1(blocks)                   # DDS files are linear (not tiled) [checked: mean 0.233 as in crowd_brightness.md]
    raise SystemExit('%s not in bg%02d key 400' % (name.decode(), arena))


def crowd_points(game, arena, set_id):
    import crowd
    import pac
    data = pac.load(os.path.join(game, 'pac', 'bg', 'bg%02d.pac' % arena))
    leaves = dict(pac.entries(data, 2))
    text = leaves['p0/%d/21100' % set_id].decode('cp932', 'replace')
    rows, people = crowd.parse_crowd(text)
    seats = {}
    for p in people:
        x, _, z, _, _ = crowd.place(rows, p)
        seats[(p['row'], p['seat'])] = (x, z)
    return np.array(list(seats.values()))


def sample(img, u, v):
    """Bilinear, wrap; img (h, w, c) float; u, v in 0..1 (v as texture rows)."""
    h, w = img.shape[:2]
    x, y = u * w - 0.5, v * h - 0.5
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = (x - x0)[:, None], (y - y0)[:, None]
    g = lambda yy, xx: img[yy % h, xx % w]
    return (g(y0, x0) * (1 - fx) * (1 - fy) + g(y0, x0 + 1) * fx * (1 - fy) +
            g(y0 + 1, x0) * (1 - fx) * fy + g(y0 + 1, x0 + 1) * fx * fy)


def luma(c):
    return c[:, 0] * 0.299 + c[:, 1] * 0.587 + c[:, 2] * 0.114


def selftest():
    rng = np.random.default_rng(7)
    ok = True
    # 1. the offset function is a bijection onto the buffer (a typo in the port would break this)
    for (w, h, b) in ((512, 512, 2), (128, 128, 3), (256, 128, 2), (96, 64, 2)):
        off = tiled_offsets(w, h, b)
        n = (w * h) << b
        full = (np.sort(off.ravel()) == np.arange(0, n, 1 << b)).all() if (w % 32 == 0 and h % 32 == 0) else True
        ok &= bool(full)
        print('offsets %dx%d bpb %d: %s' % (w, h, 1 << b, 'bijective' if full else 'WRONG'))
    # 2. 8888 round trip (including the memory order)
    img = rng.integers(0, 256, (512, 512, 4), dtype=np.uint8)
    tiled = tile_bytes(img, 2)
    back = untile_bytes(tiled, 512, 512, 2)
    r1 = bool((back == img).all())
    print('8888 round trip 512x512 exact:', r1)
    ok &= r1
    # 3. DXT1 round trip: random blocks, tiled with 16-bit swap, loaded through load_page
    blocks = rng.integers(0, 256, (128, 128, 8), dtype=np.uint8)
    sw = blocks.reshape(128, 128, 4, 2)[..., ::-1].reshape(128, 128, 8)
    tmp = os.path.join(os.environ.get('TEMP', '.'), 'untile_selftest.bin')
    open(tmp, 'wb').write(tile_bytes(sw, 3))
    got = load_page(tmp, 'dxt1', 512, 512, 'argb', True)
    r2 = bool(np.allclose(got, decode_dxt1(blocks)))
    print('DXT1 round trip 512x512 (tiled + 16-bit swap) exact:', r2)
    ok &= r2
    # 4. decoder on a known block: c0 = white, c1 = black, indices 0..3 per row
    blk = np.zeros((1, 1, 8), np.uint8)
    blk[0, 0, :4] = [0xFF, 0xFF, 0, 0]
    blk[0, 0, 4:] = [0b11100100] * 4
    d = decode_dxt1(blk)[0, :, 0]
    r3 = bool(np.allclose(d, [1, 0, 2 / 3, 1 / 3], atol=1e-6))
    print('DXT1 palette order (c0, c1, 2/3, 1/3):', r3, d.round(3).tolist())
    ok &= r3
    print('SELFTEST', 'PASS' if ok else 'FAIL')
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dump', nargs='?')
    ap.add_argument('--format', default='8888', choices=('8888', 'dxt1'))
    ap.add_argument('--width', type=int, default=512)
    ap.add_argument('--height', type=int, default=512)
    ap.add_argument('--order', default='argb', help='byte order in memory for 8888 (letters a r g b), default argb')
    ap.add_argument('--no-swap', action='store_true', help='dxt1: do not swap the 16-bit units')
    ap.add_argument('--arena', type=int, default=5)
    ap.add_argument('--set', type=int, default=20000)
    ap.add_argument('--game', default=os.path.join(REPO, 'game_data', 'extracted'))
    ap.add_argument('--out', help='PNG of the page (RGB; alpha as a second PNG *_a.png)')
    ap.add_argument('--ref-out', help='PNG of fade_lightmap00')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.dump:
        ap.error('DUMP.bin needed')
    from PIL import Image
    if sorted(a.order) != sorted('argb'):
        ap.error('--order must be a permutation of a r g b')
    if a.format == 'dxt1':
        page, raw = load_page(a.dump, 'dxt1', a.width, a.height, a.order, not a.no_swap), None
    else:
        page, raw = load_page(a.dump, '8888', a.width, a.height, a.order, False)
        print('byte lanes in memory (mean 0..1, min, max): ' + '; '.join(
            'b%d %.3f %.3f %.3f' % (i, raw[..., i].mean() / 255, raw[..., i].min() / 255, raw[..., i].max() / 255) for i in range(4)))
        print('  (the lane that is constant 1.0 or 0.0 is alpha; --order names the lanes in memory order)')
    print('page %dx%d: mean R %.3f G %.3f B %.3f A %.3f; luma %.3f; max RGB %.3f' % (
        a.width, a.height, *page[..., :4].reshape(-1, 4).mean(0), luma(page.reshape(-1, 4)).mean(), page[..., :3].max()))
    if a.out:
        Image.fromarray((page[..., :3] * 255 + 0.5).astype(np.uint8)).save(a.out)
        Image.fromarray((page[..., 3] * 255 + 0.5).astype(np.uint8)).save(os.path.splitext(a.out)[0] + '_a.png')
    ref = file_lightmap(a.game, a.arena)
    print('fade_lightmap00 (file, %dx%d): luma mean %.3f max %.3f' % (ref.shape[1], ref.shape[0], luma(ref.reshape(-1, 4)).mean(), ref[..., :3].max()))
    if a.ref_out:
        Image.fromarray((ref[..., :3] * 255 + 0.5).astype(np.uint8)).save(a.ref_out)
    pts = crowd_points(a.game, a.arena, a.set)
    u = pts[:, 0] * C184[0] * 0.5 + 0.5
    v = pts[:, 1] * C184[1] * 0.5 + 0.5
    print('%d seat positions (arena %d set %d), uv u %.3f..%.3f v %.3f..%.3f' % (len(pts), a.arena, a.set, u.min(), u.max(), v.min(), v.max()))
    for label, vv in (('v as rows (viewer)', v), ('v flipped', 1 - v)):
        pg, rf = sample(page, u, vv), sample(ref, u, vv)
        lp, lr = luma(pg), luma(rf)
        print('  %-20s page luma %.3f  file %.3f  ratio of means %.3f  median of ratios %.3f  | page RGB %.3f %.3f %.3f  file RGB %.3f %.3f %.3f' % (
            label, lp.mean(), lr.mean(), lp.mean() / max(lr.mean(), 1e-9), np.median(lp / np.maximum(lr, 1e-6)),
            *pg[:, :3].mean(0), *rf[:, :3].mean(0)))
    # whole-image comparison (is the page the file map, scaled or painted over?)
    if ref.shape[:2] == page.shape[:2]:
        a_, b_ = luma(page.reshape(-1, 4)), luma(ref.reshape(-1, 4))
        print('whole image: correlation page/file luma %.3f; ratio of means %.3f' % (np.corrcoef(a_, b_)[0, 1], a_.mean() / max(b_.mean(), 1e-9)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
