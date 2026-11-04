r"""Judge one screenshot pair with owned_verdict's own tolerance rules (Session 89; replaces Session 87's imgdiff.py,
which used max / % only and missed the 16x16 block rule - an OVER was called "within tolerance" by hand).

    py -I tools\native\shot_pair.py A.png B.png [--noise C.png] [--heat OUT.png]

Prints p99.9 of the per-pixel largest channel difference, the worst 16x16 block mean, the share of pixels over 4
(minus the noise share between B and C when --noise C is another run's shot of the B arm), the bounding box of the
differences, and WITHIN TOLERANCE / OVER (which limit) / IDENTICAL. Exit 0 / 1 (OVER) / 2 (bad input).
Limits are imported from tools\native\owned_verdict.py (TOL_SMALL, TOL_P999, TOL_BLOCK, TOL_SHARE).
"""
import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import owned_verdict as ov  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('a'); ap.add_argument('b')
    ap.add_argument('--noise', help="another run's shot of the B arm (noise = share over 4 between B and it)")
    ap.add_argument('--heat', help='write the tolerance heat map here')
    a = ap.parse_args()
    d = ov.load_diff(a.a, a.b)
    if d is None:
        print('sizes differ'); return 2
    name = f'{Path(a.a).name} vs {Path(a.b).name}'
    if not d.any():
        print(f'{name}: IDENTICAL'); return 0
    share = float((d > ov.TOL_SMALL).mean())
    p999 = float(np.percentile(d, 99.9))
    blk = ov.block_mean_max(d)
    noise = 0.0
    if a.noise:
        nd = ov.load_diff(a.b, a.noise)
        noise = float((nd > ov.TOL_SMALL).mean()) if nd is not None else 0.0
    ys, xs = np.nonzero(d > ov.TOL_SMALL)
    box = f'x {xs.min()}-{xs.max()}, y {ys.min()}-{ys.max()}' if len(ys) else 'none over 4'
    failed = []
    if p999 > ov.TOL_P999: failed.append(f'p99.9 {p999:.1f} > {ov.TOL_P999:g}')
    if blk > ov.TOL_BLOCK: failed.append(f'16x16 block mean {blk:.2f} > {ov.TOL_BLOCK:g}')
    if share > noise + ov.TOL_SHARE:
        failed.append(f'share over {ov.TOL_SMALL} {100 * share:.3f}% > noise {100 * noise:.3f}% + {100 * ov.TOL_SHARE:.1f}%')
    print(f'{name}: p99.9 {p999:.1f}, worst 16x16 block mean {blk:.2f}, {100 * share:.3f}% of pixels over '
          f'{ov.TOL_SMALL} (noise {100 * noise:.3f}%{"" if a.noise else ", no --noise shot"}), max {int(d.max())}; '
          f'differences at {box}')
    if a.heat:
        Image.fromarray(ov.heat_map(d)).save(a.heat); print(f'heat map {a.heat}')
    print('OVER (' + '; '.join(failed) + ')' if failed else 'WITHIN TOLERANCE')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
