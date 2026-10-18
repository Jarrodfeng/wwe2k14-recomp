"""Session 16 item 7: wrestler mask from two renders of the same frame. Run A is normal, run B skips the skinned
vertex shaders (wrestlers, referee: gpu_debug_skip_vertex_shader, tools/input_scripts/short/matte_check_skip.txt); both
in replay mode with the same seed, so the frame is the same (src/replay.h). Read-only on the dumps; output next to A.

    py tools/matte/mask.py A_PREFIX B_PREFIX [--min-cm 2]

A_PREFIX / B_PREFIX = captures/NNN_LABEL (each with *_depth_K.bin dumps from --wwe2k14_matte=depth). Writes
A_PREFIX_mask.png: white where A's depth is nearer than B's by more than --min-cm world units (something skinned is in
front of what is behind it), black elsewhere; A_PREFIX_mask_overlay.png (the mask tinted over A's scene colour) and
A_PREFIX_mask.json (coverage, the depth / colour dumps used).
"""
import argparse
import glob
import json
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import depth as D  # noqa: E402


def frame_dumps(prefix):
    dumps = sorted(glob.glob(prefix + '_depth_[0-9]*.bin'), key=D.dump_key)
    depths = [b for b in dumps if D.is_depth_dump(b)]
    if not depths:
        raise SystemExit('no depth dump for %s' % prefix)
    d = depths[-1]
    colours = [b for b in dumps if not D.is_depth_dump(b) and D.dump_key(b) < D.dump_key(d)]
    return d, (colours[-1] if colours else None)


def linear(prefix, bin_path):
    m = json.load(open(prefix + '_matte.json'))
    dev, info = D.decode(bin_path)
    return D.linearize(dev, m['near'], m['far'], info['depth_float'])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--min-cm', type=float, default=2.0)
    a = ap.parse_args()
    da, ca = frame_dumps(a.a)
    db, _ = frame_dumps(a.b)
    za, zb = linear(a.a, da), linear(a.b, db)
    mask = (zb - za) > a.min_cm
    Image.fromarray((mask * 255).astype(np.uint8)).save(a.a + '_mask.png')
    rec = dict(a_depth=os.path.basename(da), b_depth=os.path.basename(db), coverage=float(mask.mean()),
               min_cm=a.min_cm)
    if ca:
        px, _ = D.decode_color(ca)
        rgb = px[..., [1, 2, 3]].astype(np.float32)
        tint = rgb.copy()
        tint[mask] = 0.45 * rgb[mask] + 0.55 * np.array([0, 255, 0], np.float32)
        Image.fromarray(tint.astype(np.uint8)).save(a.a + '_mask_overlay.png')
        rec['a_colour'] = os.path.basename(ca)
    with open(a.a + '_mask.json', 'w') as f:
        json.dump(rec, f, indent=1)
    print('%s_mask.png: %.2f%% of the frame (A %s vs B %s)' % (a.a, 100 * mask.mean(), rec['a_depth'], rec['b_depth']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
