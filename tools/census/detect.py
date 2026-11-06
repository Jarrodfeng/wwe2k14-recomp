"""Session 92 census: cheap glitch detectors for one screenshot (numpy + PIL).

What it answers: "does this picture look broken?" - black / white / flat / magenta / noisy frames, and (for a pair of
shots 2 guest frames apart) flicker. Thresholds are first guesses and live in THRESH; the sheet prints the metric
distributions so they can be tuned (sheet.py recomputes every flag from the stored metrics, nothing is hidden).

  py tools\\census\\detect.py SHOT.png [SHOT2.png]     prints the metrics (and the pair difference)
Library: metrics(path, thumb_path=None) -> dict; pair_diff(path_a, path_b) -> float; flags(m, phase, stats) -> [..].
"""
import sys

import numpy as np
from PIL import Image

THRESH = {
    'black_luma': 8, 'black_frac': 0.97,       # > 97% of pixels with luma < 8
    'white_luma': 247, 'white_frac': 0.90,     # > 90% of pixels with luma > 247
    'flat_colours': 64,                        # fewer distinct colours than this
    'magenta_frac': 0.005,                     # > 0.5% pixels exactly (255,0,255)
    'noise_ratio': 3.0,                        # high-frequency energy > 3 x the family median
    'flicker_ratio': 1.5,                      # pair difference > family p95 and > 1.5 x the family median
}
# Phases whose pictures count for the black/white/flat flags (loads and boots are black by design).
PICTURE_PHASES = ('entrance', 'match', 'menus', 'other')


def _load(path):
    return Image.open(path).convert('RGB')


def metrics(path, thumb_path=None, thumb_width=960):
    """-> dict of the numbers for one screenshot; writes a JPEG thumbnail when thumb_path is given."""
    im = _load(path)
    w, h = im.size
    if thumb_path:
        t = im if w <= thumb_width else im.resize((thumb_width, round(h * thumb_width / w)), Image.LANCZOS)
        t.save(thumb_path, 'JPEG', quality=85)
    a = np.asarray(im)
    half = a[::2, ::2]                                   # exact pixels, for the exact-colour test
    mag = float(np.mean((half[..., 0] == 255) & (half[..., 1] == 0) & (half[..., 2] == 255)))
    small = np.asarray(im.reduce(4))                      # 1/4 size, box averaged
    luma = small[..., 0] * 0.299 + small[..., 1] * 0.587 + small[..., 2] * 0.114
    quant = a[::4, ::4].astype(np.uint32)
    packed = (quant[..., 0] << 16) | (quant[..., 1] << 8) | quant[..., 2]
    distinct = int(np.unique(packed).size)
    lap = (4 * luma[1:-1, 1:-1] - luma[:-2, 1:-1] - luma[2:, 1:-1] - luma[1:-1, :-2] - luma[1:-1, 2:])
    return {
        'w': w, 'h': h, 'luma_mean': round(float(luma.mean()), 2),
        'black_frac': round(float(np.mean(luma < THRESH['black_luma'])), 4),
        'white_frac': round(float(np.mean(luma > THRESH['white_luma'])), 4),
        'distinct': distinct, 'magenta_frac': round(mag, 5), 'hf': round(float(np.abs(lap).mean()), 3),
    }


def pair_diff(path_a, path_b):
    """Mean absolute difference (0-255) of two shots at 1/8 size (flicker between guest frames N and N+2)."""
    a = np.asarray(_load(path_a).reduce(8)).astype(np.int16)
    b = np.asarray(_load(path_b).reduce(8)).astype(np.int16)
    if a.shape != b.shape:
        return -1.0
    return round(float(np.abs(a - b).mean()), 3)


def flags(m, phase, stats=None):
    """The flags for one shot's metrics: [(name, number)]. stats = {'hf_median': x} for the family (noise)."""
    out = []
    if phase in PICTURE_PHASES:
        if m['black_frac'] > THRESH['black_frac']:
            out.append(('black', m['black_frac']))
        if m['white_frac'] > THRESH['white_frac']:
            out.append(('white', m['white_frac']))
        if m['distinct'] < THRESH['flat_colours'] and m['black_frac'] <= THRESH['black_frac']:
            out.append(('flat', m['distinct']))
    if m['magenta_frac'] > THRESH['magenta_frac']:
        out.append(('magenta', m['magenta_frac']))
    if phase in ('entrance', 'match') and stats and stats.get('hf_median') and m['hf'] > THRESH['noise_ratio'] * stats['hf_median']:
        out.append(('noise', m['hf']))
    return out


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    print(metrics(sys.argv[1]))
    if len(sys.argv) > 2:
        print('pair diff', pair_diff(sys.argv[1], sys.argv[2]))
