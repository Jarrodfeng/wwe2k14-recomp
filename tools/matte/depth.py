"""Session 16 item 7: depth mattes. Turns the game's depth resolves dumped by the SDK (gpu_depth_dump, written next to
a screenshot by --wwe2k14_matte=depth: captures/NNN_LABEL_depth_K.bin + .json, and NNN_LABEL_matte.json with the
camera's near / far) into images.

    py tools/matte/depth.py CAPTURE_BIN_OR_DIR [--all] [--raw]

For each dump: untile (Xenos 2D tiling, as ReXGlue's texture conversion), byte-swap per the copy's endian field, take
the 24-bit depth (D24S8: depth in the top 24 bits; D24FS8: 20e4 float depth), and write
  NNN_LABEL_depth.png      16-bit grayscale, LINEAR depth from the camera's near / far (white = near, black = far
                           plane), when the matte json has near / far; else the raw depth range stretched
  NNN_LABEL_depth_raw.png  (--raw) 16-bit raw device depth (0..1 -> 0..65535)
  NNN_LABEL_depth.json     near, far, size, format, percentiles of the linear depth, which dump was used
  NNN_LABEL_color.png      the scene colour of the SAME frame (the colour resolve right before the depth copy: no HUD,
                           before the post effects), so depth and colour line up exactly
The main scene's depth is the largest depth resolve of the frame (shadow maps are smaller); --all writes every dump.
Output stays next to the captures (out of git: game-derived).
"""
import argparse
import glob
import json
import os
import sys

import numpy as np
from PIL import Image

FORMATS = {6: ('8_8_8_8', 4), 7: ('2_10_10_10', 4), 22: ('24_8', 4), 23: ('24_8_FLOAT', 4), 36: ('32_FLOAT', 4),
           24: ('16_16', 4), 5: ('16', 2)}


def tiled_offset(x, y, pitch, log2_bpp):
    """Xenos 2D tiled byte offset of block (x, y) in a texture of pitch (blocks, multiple of 32); vectorised."""
    macro = ((y // 32) * (pitch // 32)) << (log2_bpp + 7)
    micro = ((y & 6) << 2) << log2_bpp
    row = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2_bpp)) + ((y & 1) << 4)
    macro_c = (x // 32) << (log2_bpp + 7)
    micro_c = (x & 7) << log2_bpp
    off = row + (macro_c + ((micro_c & ~0xF) << 1) + (micro_c & 0xF))
    off = ((off & ~0x1FF) << 3) + ((off & 0x1C0) << 2) + (off & 0x3F) + ((y & 16) << 7) + \
        (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)
    return off


def f24_to_float(v):
    """Xenos 20e4 float depth (24 bits) -> float."""
    e = (v >> 20) & 0xF
    m = v & 0xFFFFF
    out = np.where(e == 0, m / float(1 << 20) * 2.0 ** -15, (1.0 + m / float(1 << 20)) * 2.0 ** (e.astype(np.int64) - 15))
    return out


def decode(bin_path):
    meta = json.load(open(bin_path[:-4] + '.json'))
    data = np.fromfile(bin_path, dtype=np.uint8)
    info, pitch_reg = meta['copy_dest_info'], meta['copy_dest_pitch']
    endian, fmt = info & 7, (info >> 7) & 0x3F
    pitch, height = pitch_reg & 0x3FFF, (pitch_reg >> 16) & 0x3FFF
    name, bpp = FORMATS.get(fmt, ('format %d' % fmt, 4))
    log2 = {1: 0, 2: 1, 4: 2, 8: 3}[bpp]
    pitch_al = (pitch + 31) // 32 * 32
    height_al = (height + 31) // 32 * 32
    start = meta['written_address'] - meta['copy_dest_base']
    full = np.zeros(max(start + len(data), pitch_al * height_al * bpp + 4096), dtype=np.uint8)
    full[start:start + len(data)] = data
    y, x = np.mgrid[0:height, 0:pitch]
    offs = tiled_offset(x.astype(np.int64), y.astype(np.int64), pitch_al, log2)
    if bpp == 4:
        b = np.stack([full[offs + i] for i in range(4)], axis=-1).astype(np.uint32)
        # The dump is the texture as guest memory holds it: big-endian words (the copy's endian field is how the
        # GPU fetches it; run 1287: depth in the top 24 bits, stencil 0 in the low byte, read big-endian).
        word = (b[..., 0] << 24) | (b[..., 1] << 16) | (b[..., 2] << 8) | b[..., 3]
        d24 = word >> 8
        depth_float = (meta['depth_info'] >> 16) & 1  # RB_DEPTH_INFO.depth_format: 1 = D24FS8
        dev = f24_to_float(d24) if depth_float else d24 / float((1 << 24) - 1)
    else:
        b = np.stack([full[offs + i] for i in range(2)], axis=-1).astype(np.uint32)
        word = (b[..., 0] << 8) | b[..., 1]
        dev = word / 65535.0
    return dev, dict(width=int(pitch), height=int(height), format=name, endian=int(endian), bytes=int(len(data)),
                     depth_float=bool((meta['depth_info'] >> 16) & 1))


def linearize(dev, near, far, reversed_z):
    # D3D-style projection: depth d in [0, 1] (0 = near) -> view distance z = n f / (f - d (f - n)). The game's
    # depth-of-field converter (sub_827F14C0, src/cinecam.h) uses this mapping with the render view's near / far.
    # The 2K14 depth buffer is D24FS8 and reversed (run 1287: the ring a few metres away reads ~0.03, the far crowd
    # ~0.0015): d = 1 - stored.
    d = np.clip(1.0 - dev if reversed_z else dev, 0.0, 1.0)
    return near * far / (far - d * (far - near))


def is_depth_dump(bin_path):
    return (json.load(open(bin_path[:-4] + '.json'))['copy_control'] & 7) >= 4


def dump_key(bin_path):
    return int(bin_path[bin_path.rindex('_') + 1:-4])


def decode_color(bin_path):
    """A full-size colour resolve (8_8_8_8, the dump's big-endian words) -> RGB uint8 (byte order chosen by
    --color-order, default ARGB as the words read)."""
    meta = json.load(open(bin_path[:-4] + '.json'))
    data = np.fromfile(bin_path, dtype=np.uint8)
    pitch_reg = meta['copy_dest_pitch']
    pitch, height = pitch_reg & 0x3FFF, (pitch_reg >> 16) & 0x3FFF
    pitch_al = (pitch + 31) // 32 * 32
    start = meta['written_address'] - meta['copy_dest_base']
    full = np.zeros(max(start + len(data), pitch_al * ((height + 31) // 32 * 32) * 4 + 4096), dtype=np.uint8)
    full[start:start + len(data)] = data
    y, x = np.mgrid[0:height, 0:pitch]
    offs = tiled_offset(x.astype(np.int64), y.astype(np.int64), pitch_al, 2)
    return np.stack([full[offs + i] for i in range(4)], axis=-1), meta


def write_color(prefix, dumps, depth_bp, order):
    """The scene colour of the same frame: the colour resolve dumped right before the chosen depth resolve."""
    k = dump_key(depth_bp)
    cands = [bp for bp in dumps if not is_depth_dump(bp) and dump_key(bp) < k]
    if not cands:
        return
    bp = max(cands, key=dump_key)
    px, meta = decode_color(bp)
    idx = {'ARGB': (1, 2, 3), 'RGBA': (0, 1, 2), 'BGRA': (2, 1, 0), 'ABGR': (3, 2, 1)}[order]
    rgb = px[..., list(idx)]
    Image.fromarray(rgb.astype(np.uint8)).save(prefix + '_color.png')
    print('  %s_color.png (scene colour of the same frame: %s, resolve %d, %s)' % (
        prefix, os.path.basename(bp), meta['resolve_index'], order))


def process(prefix, dumps, use_all, raw, reverse=None, far_view=None, color_order='ARGB'):
    matte_json = prefix + '_matte.json'
    matte = json.load(open(matte_json)) if os.path.exists(matte_json) else {}
    sized = []
    for bp in dumps:
        if not is_depth_dump(bp):
            continue
        try:
            dev, info = decode(bp)
        except Exception as e:  # noqa: BLE001
            print('  %s: cannot decode (%s)' % (bp, e))
            continue
        sized.append((dev.size, bp, dev, info))
    if not sized:
        return
    sized.sort(key=lambda t: (-t[0], -dump_key(t[1])))  # the largest; of equal ones the last (a whole frame)
    write_color(prefix, dumps, sized[0][1], color_order)
    for n, (_, bp, dev, info) in enumerate(sized if use_all else sized[:1]):
        out = prefix + ('_depth' if n == 0 else '_depth%d' % n)
        near, far = matte.get('near'), matte.get('far')  # the camera's planes: used for the distances
        img_far = min(far, far_view) if (far and far_view) else far  # the image's far end (--far-view)
        rec = dict(dump=os.path.basename(bp), **info, near=near, far=far, image_far=img_far,
                   image_scale='log' if far_view else 'linear')
        if raw:
            Image.fromarray((np.clip(dev, 0, 1) * 65535).astype(np.uint16)).save(out + '_raw.png')
        if near and far and far > near:
            rev = info['depth_float'] if reverse is None else reverse
            z = linearize(dev, near, far, rev)
            img = 1.0 - (z - near) / (img_far - near)   # white = near
            if far_view:  # spread the grey levels over [near, far_view] with a log curve (arena scale)
                img = 1.0 - np.log(np.clip(z, near, img_far) / near) / np.log(img_far / near)
            rec.update(linear=True, reversed=bool(rev), z_p1=float(np.percentile(z, 1)), z_p50=float(np.percentile(z, 50)),
                       z_p99=float(np.percentile(z, 99)))
        else:
            lo, hi = np.percentile(dev, 1), np.percentile(dev, 99)
            img = 1.0 - (dev - lo) / max(1e-9, hi - lo)
            rec.update(linear=False, dev_p1=float(lo), dev_p99=float(hi))
        Image.fromarray((np.clip(img, 0, 1) * 65535).astype(np.uint16)).save(out + '.png')
        with open(out + '.json', 'w') as f:
            json.dump(rec, f, indent=1)
        print('  %s.png (%dx%d %s, %s)' % (out, info['width'], info['height'], info['format'],
                                           'near %.1f far %.1f, image to %.0f' % (near, far, img_far) if rec.get('linear') else
                                           'raw range stretched'))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('path', help='a *_depth_K.bin dump, or a captures folder (every *_depth_0.bin in it)')
    ap.add_argument('--all', action='store_true', help='every depth resolve of the frame, not just the largest')
    ap.add_argument('--raw', action='store_true', help='also the raw device depth as 16-bit PNG')
    ap.add_argument('--forward-z', action='store_true', help='depth is not reversed (default: reversed for D24FS8)')
    ap.add_argument('--far-view', type=float, default=0, help='image far plane (world units; 0 = the camera far), log scale')
    ap.add_argument('--color-order', default='ARGB', choices=['ARGB', 'RGBA', 'BGRA', 'ABGR'],
                    help='byte order of the colour dump (default ARGB)')
    a = ap.parse_args()
    if os.path.isdir(a.path):
        firsts = sorted(glob.glob(os.path.join(a.path, '*_depth_0.bin')))
    else:
        firsts = [a.path]
    for first in firsts:
        prefix = first[:first.rindex('_depth_')]
        dumps = sorted(glob.glob(prefix + '_depth_[0-9]*.bin'))
        print(prefix)
        process(prefix, dumps, a.all, a.raw, False if a.forward_z else None, a.far_view or None, a.color_order)
    return 0


if __name__ == '__main__':
    sys.exit(main())
