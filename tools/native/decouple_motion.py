"""Session 95 (item F): motion measures for gpu_d3d_decouple phase 2 (numpy + PIL only; no game run).

  triple PREV MID NEXT   phase correlation between three shots of consecutive presents (N-1, the in-between, N) of ONE pan: the sub-pixel shift
                         prev->mid and prev->next (and mid->next). A mid that sits between its neighbours has shift(prev->mid) about half of
                         shift(prev->next); prints the ratio and a verdict (ok 0.35..0.65 on a shift of at least 1 px; a pan under 1 px proves nothing).
  differ A B             how much two pictures differ (mean absolute difference per channel 0..255, share of pixels over 8): the proof that the two
                         pictures of a tick are not equal (a layer-0-twice bug reads ~0). A = a default shot, B = the same tick under
                         --gpu_d3d_decouple=41 (view 1 only). On a still camera both are ~0 - judge on a pan.
  series DIR [--glob]    the shift of every consecutive pair of the PNG/BMP files in DIR (sorted by name): a 120 Hz present stream (alternating
                         mid, picture) should show equal steps; a stream of equal pairs shows 0, step, 0, step (the "solid 30" signature).
                         Prints the steps, their mean and the share of near-zero steps (< 0.15 of the mean).

Shots: the owned_check / screenshot / clip tools (a clip of the =41 arm: PNG frames). The window of interest is the middle 3/4 of the picture
(the HUD and the borders do not pan).
    py tools\\native\\decouple_motion.py triple a.png b.png c.png
    py tools\\native\\decouple_motion.py differ default.png view1.png
    py tools\\native\\decouple_motion.py series out\\clip_frames
"""
import sys
import glob
import os
import numpy as np
from PIL import Image


def load_gray(path):
    im = Image.open(path).convert('RGB')
    a = np.asarray(im, dtype=np.float64)
    h, w = a.shape[:2]
    y0, y1, x0, x1 = h // 8, h - h // 8, w // 8, w - w // 8
    g = a[y0:y1, x0:x1].mean(axis=2)
    return g


def hann(h, w):
    return np.outer(np.hanning(h), np.hanning(w))


def shift(a, b):
    """Sub-pixel (dx, dy) such that b(x) = a(x - d): phase correlation with a Hann window and a parabolic peak."""
    h = min(a.shape[0], b.shape[0])
    w = min(a.shape[1], b.shape[1])
    a, b = a[:h, :w], b[:h, :w]
    win = hann(h, w)
    fa = np.fft.fft2((a - a.mean()) * win)
    fb = np.fft.fft2((b - b.mean()) * win)
    r = fb * np.conj(fa)
    r /= np.maximum(np.abs(r), 1e-9)
    c = np.fft.ifft2(r).real
    py, px = np.unravel_index(np.argmax(c), c.shape)

    def sub(arr, i, n):
        m, p0, p1 = arr[(i - 1) % n], arr[i], arr[(i + 1) % n]
        d = m - 2 * p0 + p1
        return 0.0 if abs(d) < 1e-12 else 0.5 * (m - p1) / d

    fx = px + sub(c[py, :], px, w)
    fy = py + sub(c[:, px], py, h)
    if fx > w / 2:
        fx -= w
    if fy > h / 2:
        fy -= h
    return float(fx), float(fy), float(c.max())


def cmd_triple(p, m, n):
    a, b, c = load_gray(p), load_gray(m), load_gray(n)
    s_pm, s_pn, s_mn = shift(a, b), shift(a, c), shift(b, c)
    for name, s in (('prev->mid', s_pm), ('prev->next', s_pn), ('mid->next', s_mn)):
        print('%-11s dx %+7.3f  dy %+7.3f  (peak %.2f)' % (name, s[0], s[1], s[2]))
    full = np.hypot(s_pn[0], s_pn[1])
    half = np.hypot(s_pm[0], s_pm[1])
    if full < 1.0:
        print('verdict: UNDECIDED (prev->next moves %.2f px: pick a faster pan)' % full)
        return 2
    ratio = half / full
    # the mid must also lie on the same line: its shift projected on the full shift's direction
    d = np.array(s_pn[:2]) / full
    proj = (s_pm[0] * d[0] + s_pm[1] * d[1]) / full
    print('ratio |prev->mid| / |prev->next| = %.3f  (along the pan: %.3f; 0.5 = halfway)' % (ratio, proj))
    ok = 0.35 <= proj <= 0.65
    print('verdict: %s' % ('OK, the in-between sits about halfway' if ok else 'NOT halfway'))
    return 0 if ok else 1


def cmd_differ(a_path, b_path):
    a = np.asarray(Image.open(a_path).convert('RGB'), dtype=np.int16)
    b = np.asarray(Image.open(b_path).convert('RGB'), dtype=np.int16)
    h, w = min(a.shape[0], b.shape[0]), min(a.shape[1], b.shape[1])
    d = np.abs(a[:h, :w] - b[:h, :w])
    mad = float(d.mean())
    over = float((d.max(axis=2) > 8).mean())
    print('mean abs diff %.4f / 255, %.3f%% of pixels differ by more than 8' % (mad, over * 100))
    print('verdict: %s' % ('the pictures differ' if mad > 0.05 or over > 0.0005 else 'EQUAL (a still camera, or the layer is read twice)'))
    return 0


def cmd_series(folder):
    files = sorted(f for f in glob.glob(os.path.join(folder, '*')) if f.lower().endswith(('.png', '.bmp', '.jpg')))
    if len(files) < 3:
        print('need at least 3 frames in', folder)
        return 2
    steps = []
    prev = load_gray(files[0])
    for f in files[1:]:
        cur = load_gray(f)
        s = shift(prev, cur)
        steps.append(float(np.hypot(s[0], s[1])))
        prev = cur
    steps = np.array(steps)
    mean = float(steps.mean())
    near0 = float((steps < 0.15 * mean).mean()) if mean > 0 else 1.0
    print('%d frames, steps (px): %s' % (len(files), ' '.join('%.2f' % x for x in steps[:40])))
    print('mean %.3f px, share of near-zero steps %.1f%%' % (mean, near0 * 100))
    print('verdict: %s' % ('every present moves (a real 120 stream)' if near0 < 0.15 else 'many equal pairs: the stream is really %d%% duplicates' % round(near0 * 100)))
    return 0


def main():
    a = sys.argv[1:]
    if len(a) == 4 and a[0] == 'triple':
        return cmd_triple(*a[1:])
    if len(a) == 3 and a[0] == 'differ':
        return cmd_differ(*a[1:])
    if len(a) == 2 and a[0] == 'series':
        return cmd_series(a[1])
    print(__doc__)
    return 2


sys.exit(main())
