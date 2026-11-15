r"""S102 lane L1b: compare two directories of cube dumps texel by texel (0 game runs).
  py -I tools\l1b\l1b_cubes.py OFF_DIR ON_DIR [--json OUT]
The dumps are written by fview `--sweep` with FVIEW_SWEEP_LAYERS=1 FVIEW_SWEEP_CUBE_DUMP=<dir> [FVIEW_CUBE_DUMP_EVERY=N]: one file a frame and cube, f<frame>_i<image>.bin = the six 64x64 RGBA8 layers
back to back. Every file of OFF is compared with the same name in ON (a missing one is a failure). Prints the histogram of the per-channel absolute difference over every texel of every cube (levels of 255),
the share of texels with any channel off by more than 1 level, the worst difference and its frame / cube / layer / texel, and a per-layer line. Machine line: HIST total=.. d0=.. d1=.. d2=.. d3p=.. max=..
Exit 1 on an unreadable / missing dump."""
import json
import os
import sys

import numpy as np


def compare(off_dir, on_dir):
    names = sorted(n for n in os.listdir(off_dir) if n.endswith('.bin'))
    if not names:
        raise SystemExit('FAIL no dumps in ' + off_dir)
    hist = np.zeros(256, dtype=np.int64)
    plus = minus = 0   # review: the signed split of the 1-level differences (on - off): a one-sided bias would show
    texel_over1 = texels = 0
    worst = (0, None)
    per_layer = np.zeros((6, 4), dtype=np.int64)   # layer: texels with d>=1, d>=2, d>=3, total
    missing = 0
    sizes = set()
    for n in names:
        p_on = os.path.join(on_dir, n)
        if not os.path.exists(p_on):
            missing += 1
            continue
        a = np.fromfile(os.path.join(off_dir, n), dtype=np.uint8)
        b = np.fromfile(p_on, dtype=np.uint8)
        if a.size != b.size or a.size % (6 * 64 * 64 * 4):
            raise SystemExit('FAIL %s: %d vs %d bytes' % (n, a.size, b.size))
        sizes.add(a.size)
        a = a.reshape(-1, 6, 64, 64, 4).astype(np.int16)
        b = b.reshape(-1, 6, 64, 64, 4).astype(np.int16)
        d = np.abs(a - b)
        plus += int(((b - a) == 1).sum())
        minus += int(((b - a) == -1).sum())
        hist += np.bincount(d.ravel(), minlength=256)
        per_texel = d.max(axis=-1)
        texel_over1 += int((per_texel > 1).sum())
        texels += per_texel.size
        for layer in range(6):
            pl = per_texel[:, layer]
            per_layer[layer] += [int((pl >= 1).sum()), int((pl >= 2).sum()), int((pl >= 3).sum()), pl.size]
        m = int(d.max())
        if m > worst[0]:
            idx = np.unravel_index(int(np.argmax(d)), d.shape)
            worst = (m, '%s cube-in-file %d layer %d texel (%d, %d) channel %d: off %d on %d' % (n, idx[0], idx[1], idx[3], idx[2], idx[4], int(a[idx]), int(b[idx])))
    total = int(hist.sum())
    out = {'files': len(names), 'missing': missing, 'values': total, 'texels': texels, 'd0': int(hist[0]), 'd1': int(hist[1]), 'd2': int(hist[2]), 'd3p': int(hist[3:].sum()), 'max': worst[0],
           'texels_over_1': texel_over1, 'worst': worst[1], 'plus1': plus, 'minus1': minus, 'per_layer': per_layer.tolist()}
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) < 2:
        print('usage: l1b_cubes.py OFF_DIR ON_DIR [--json OUT]')
        return 1
    res = compare(args[0], args[1])
    print('cube dumps: %d files compared (%d missing in ON), %d channel values, %d texels' % (res['files'], res['missing'], res['values'], res['texels']))
    t = max(1, res['values'])
    print('  |off - on| per channel: 0 -> %.4f %%, 1 -> %.4f %%, 2 -> %.4f %%, 3+ -> %.4f %%, max %d' % (100.0 * res['d0'] / t, 100.0 * res['d1'] / t, 100.0 * res['d2'] / t, 100.0 * res['d3p'] / t, res['max']))
    print('  texels with a channel off by more than 1 level: %d of %d (%.5f %%)' % (res['texels_over_1'], res['texels'], 100.0 * res['texels_over_1'] / max(1, res['texels'])))
    print('  worst: %s' % res['worst'])
    print('  signed split of the 1-level differences (on - off): +1 %d, -1 %d' % (res['plus1'], res['minus1']))
    for layer, row in enumerate(res['per_layer']):
        print('  layer %d: texels off >=1: %.3f %%, >=2: %.4f %%, >=3: %.4f %%' % (layer, 100.0 * row[0] / max(1, row[3]), 100.0 * row[1] / max(1, row[3]), 100.0 * row[2] / max(1, row[3])))
    print('HIST total=%d d0=%d d1=%d d2=%d d3p=%d max=%d over1=%d missing=%d plus1=%d minus1=%d' % (res['values'], res['d0'], res['d1'], res['d2'], res['d3p'], res['max'], res['texels_over_1'], res['missing'], res['plus1'], res['minus1']))
    if '--json' in sys.argv:
        with open(sys.argv[sys.argv.index('--json') + 1], 'w') as f:
            json.dump(res, f)
    return 1 if res['missing'] else 0


if __name__ == '__main__':
    sys.exit(main())
