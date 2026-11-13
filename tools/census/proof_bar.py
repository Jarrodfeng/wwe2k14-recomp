r"""Session 101 lane 143: the census's proof bar. The census's redraw of a capture's LAST frame must match the zip's picture.png (what the live game showed) within
owned_check's tolerance (tools\native\owned_verdict.py: p99.9 of the per-pixel largest channel difference <= 8, no 16x16 block mean > 2, share of pixels over 4 <= 0.3 %).
The strict tolerance is reported but does not gate (it fails on a healthy capture, see GUARD_BLOCK); the gate is a coarse block-mean guard. A redraw that misses the ring, the arena or anything else the live picture shows fails here, whatever the cause (the thin-draw bug of Check J1, run 7086: the lite draws were
dropped and the census said "99.4 % ours" of a picture without the ring and the arena).

  py -I tools\census\proof_bar.py REDRAW.png LIVE.png [--sheet OUT.jpg]      exit 0 within tolerance, 1 OVER, 2 cannot read
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

TOL_SMALL, TOL_P999, TOL_BLOCK, TOL_SHARE = 4, 8.0, 2.0, 0.003   # owned_verdict.py (Session 82), noise 0
# The census redraw and the live picture are NOT the same renderer pass (the crowd's frame timing, the live post / sampling), so owned_check's STRICT tolerance does not hold even for a
# healthy non-thin capture (measured 2026-11-12 on Check B's zip, last frame 340: p99.9 36, share over 4 96.8 %). It is reported as `strict` (info). The GUARD is coarse and calibrated on
# that healthy capture: the largest difference of the mean colour over any 16x16 block (a missing ring, arena or wrestler is a block off by 100+; Check B healthy: 32.6; the ringless
# J1 redraw: 219; J1 with only the spotlight beams missing: 169). A block over GUARD_BLOCK fails.
GUARD_BLOCK = 64.0


def block_mean_max(d, b=16):
    h, w = d.shape
    h2, w2 = h - h % b, w - w % b
    m = d[:h2, :w2].reshape(h2 // b, b, w2 // b, b).mean(axis=(1, 3))
    return float(m.max())


def compare(redraw, live):
    """-> (ok, numbers dict, reasons list, d array). Both paths are images; a size difference is resized (nearest) to the live picture's size and said."""
    a = Image.open(redraw).convert('RGB')
    b = Image.open(live).convert('RGB')
    note = ''
    if a.size != b.size:
        note = f'redraw {a.size[0]}x{a.size[1]} resized to the live picture {b.size[0]}x{b.size[1]}'
        a = a.resize(b.size, Image.LANCZOS)
    d = np.abs(np.asarray(a, dtype=np.int16) - np.asarray(b, dtype=np.int16)).max(axis=2)
    p999 = float(np.percentile(d, 99.9))
    blk = block_mean_max(d)
    share = float((d > TOL_SMALL).mean())
    bs = 16
    h, w = d.shape
    h2, w2 = h - h % bs, w - w % bs
    a16 = np.asarray(a, dtype=np.float32)[:h2, :w2].reshape(h2 // bs, bs, w2 // bs, bs, 3).mean(axis=(1, 3))
    b16 = np.asarray(b, dtype=np.float32)[:h2, :w2].reshape(h2 // bs, bs, w2 // bs, bs, 3).mean(axis=(1, 3))
    guard = float(np.abs(a16 - b16).max())
    strict = []
    if p999 > TOL_P999:
        strict.append(f'p99.9 {p999:.1f} > {TOL_P999:g}')
    if blk > TOL_BLOCK:
        strict.append(f'16x16 block mean {blk:.2f} > {TOL_BLOCK:g}')
    if share > TOL_SHARE:
        strict.append(f'share over {TOL_SMALL} {100 * share:.3f}% > {100 * TOL_SHARE:.1f}%')
    reasons = [f'a 16x16 block of the redraw differs from the live picture by {guard:.0f} (guard {GUARD_BLOCK:g}): a part of the picture is missing or wrong'] if guard > GUARD_BLOCK else []
    nums = {'p999': p999, 'block_mean_max': blk, 'share_over_4': share, 'max': int(d.max()), 'note': note, 'guard_block': guard, 'strict_ok': not strict, 'strict_reasons': strict}
    return not reasons, nums, reasons, d


def sheet(redraw, live, d, path, title):
    a = Image.open(redraw).convert('RGB').resize((640, 360))
    b = Image.open(live).convert('RGB').resize((640, 360))
    h = np.zeros(d.shape + (3,), np.uint8)
    h[(d >= 1) & (d <= TOL_SMALL)] = (40, 70, 200)
    h[(d > TOL_SMALL) & (d <= TOL_P999)] = (255, 170, 30)
    h[d > TOL_P999] = (255, 30, 30)
    s = Image.new('RGB', (1920, 380), 'black')
    s.paste(b, (0, 20))
    s.paste(a, (640, 20))
    s.paste(Image.fromarray(h).resize((640, 360), Image.NEAREST), (1280, 20))
    from PIL import ImageDraw
    ImageDraw.Draw(s).text((6, 4), title, fill='yellow')
    s.save(path, quality=90)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    redraw, live = sys.argv[1], sys.argv[2]
    try:
        ok, nums, reasons, d = compare(redraw, live)
    except Exception as e:  # cannot read: loud
        print(f'FAIL proof bar cannot read the pictures: {e}')
        sys.exit(2)
    print(('PASS' if ok else 'FAIL') + f' proof bar: guard block {nums["guard_block"]:.1f} (limit {GUARD_BLOCK:g}); strict owned_check tolerance ' + ('met' if nums['strict_ok'] else 'not met: ' + '; '.join(nums['strict_reasons']))
          + (' | ' + '; '.join(reasons) if reasons else '') + (' | ' + nums['note'] if nums['note'] else ''))
    if '--sheet' in sys.argv:
        sheet(redraw, live, d, sys.argv[sys.argv.index('--sheet') + 1], 'live picture | census redraw | tolerance heat map')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
