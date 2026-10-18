"""Screenshot regression check (Session 6 harness, spec B8 in docs/optimization_candidates.md; extended in Session 8
task 16 with the cross-resolution method and the > 32 level).

Compares captures from the input script's `screenshot LABEL` command (<exe folder>/captures/NNN_LABEL.bmp):
  MAD    mean absolute difference over every R, G, B value (0-255)
  off%   share of pixels where any channel differs by more than 16
  off32  share of pixels where any channel differs by more than 32
Thresholds: --static (default)  pass if MAD <= 0.5           (same build twice: 0.00 on static screens, 052 vs 053)
            --animated          pass if MAD <= 5 or off% <= 10 (title 1.50 / 3.30%, main menu 2.44 / 6.53%, 069 vs 072)
            --cross-res         images of different sizes (e.g. the 4K preset vs 720p): the larger one is downscaled
                                to the smaller with PIL Image.LANCZOS; same aspect ratio required. A resized pair keeps
                                its label's rule plus the resampling allowance:
                                  static:   MAD <= 2.0 and off32 <= 0.5% (the resampling level itself: a static
                                            screen's own noise is ~0; the run-175 pre-check, 4K logo screens vs 720p
                                            run 146: MAD 0.8-1.1, <= 0.11% off by > 32, off 1.0-2.0%; Lanczos
                                            reproduces those numbers, an area average (BOX) does not: MAD 0.5-0.8,
                                            off32 up to 0.67%)
                                  animated: MAD <= 5 + 2.0 or off% <= 10 + 2.0 (the animated rule plus the
                                            resampling level: a moving screen's A/A noise alone is above 2.0 MAD)
                                A same-size pair is judged by --static / --animated as before.

    py tools/bench/shot_diff.py A.bmp B.bmp [--animated] [--cross-res] [--heatmap out\\bench\\shotdiff\\x.png]
    py tools/bench/shot_diff.py --runs 052 053 [--animated-labels "title|main_menu"] [--heatmap-dir DIR]
    py tools/bench/shot_diff.py --runs 146 175 --cross-res        (720p defaults vs the 4K preset, logo screens)

Batch mode pairs NNN_LABEL.bmp with MMM_LABEL.bmp for every label both runs have. Exit code 1 if any pair fails
(or a label is missing on one side with --strict). Captures are game output: heatmaps go under out\\ (gitignored),
never into the repo. References and the regress check: tools/bench/shot_refs.py, docs/harness/screens.md.
"""
import argparse
import glob
import os
import re
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
DEFAULT_CAPTURES = os.path.join(REPO, 'out', 'build', 'win-amd64-release', 'captures')
STATIC_MAX_MAD = 0.5
ANIMATED_MAX_MAD = 5.0
ANIMATED_MAX_OFF_PCT = 10.0
OFF_LEVEL = 16
OFF_LEVEL_HI = 32
# Cross-resolution ("resampling only") level, run 175: MAD 0.8-1.1, <= 0.11 % of pixels off by > 32, off16 1.0-2.0 %.
XRES_MAX_MAD = 2.0         # static resized pair: the whole rule; animated: added to ANIMATED_MAX_MAD
XRES_MAX_OFF32 = 0.5
XRES_ALLOW_OFF16 = 2.0     # animated resized pair: added to ANIMATED_MAX_OFF_PCT
XRES_FILTER = Image.LANCZOS


def load(path):
    with Image.open(path) as im:
        return np.asarray(im.convert('RGB'), dtype=np.int16)


def load_pair(a_path, b_path, cross_res=False):
    """-> (a, b, how, resized) as int16 arrays of one size, or raises ValueError (size / aspect differs).
    cross_res: the larger image is downscaled to the smaller one (XRES_FILTER, Lanczos)."""
    with Image.open(a_path) as ia, Image.open(b_path) as ib:
        a, b = ia.convert('RGB'), ib.convert('RGB')
    how = '%dx%d' % a.size
    resized = False
    if a.size != b.size:
        if not cross_res:
            raise ValueError('size differs: %dx%d vs %dx%d' % (a.size + b.size))
        ra, rb = a.size[0] / a.size[1], b.size[0] / b.size[1]
        if abs(ra - rb) > 0.01:
            raise ValueError('aspect differs: %dx%d vs %dx%d' % (a.size + b.size))
        if a.size[0] * a.size[1] > b.size[0] * b.size[1]:
            how = '%dx%d -> %dx%d' % (a.size + b.size)
            a = a.resize(b.size, XRES_FILTER)
        else:
            how = '%dx%d -> %dx%d' % (b.size + a.size)
            b = b.resize(a.size, XRES_FILTER)
        resized = True
    return np.asarray(a, dtype=np.int16), np.asarray(b, dtype=np.int16), how, resized


def compare(a_path, b_path, heatmap=None, cross_res=False):
    """{mad, off_pct (> 16), off32, max, size, resized} or {error}. Without cross_res, sizes must match."""
    try:
        a, b, how, resized = load_pair(a_path, b_path, cross_res)
    except ValueError as e:
        return dict(error=str(e))
    except OSError as e:
        return dict(error='unreadable: %s' % e)
    d = np.abs(a - b)
    dm = d.max(axis=2)
    mad = float(d.mean())
    off = float((dm > OFF_LEVEL).mean() * 100.0)
    off32 = float((dm > OFF_LEVEL_HI).mean() * 100.0)
    if heatmap:
        os.makedirs(os.path.dirname(os.path.abspath(heatmap)), exist_ok=True)
        img = np.clip(dm.astype(np.float32) * 4.0, 0, 255).astype(np.uint8)  # x4 so small differences show
        Image.fromarray(img).save(heatmap)  # 2-D uint8 -> greyscale
    return dict(mad=mad, off_pct=off, off32=off32, max=int(d.max()), size=how, resized=resized)


def verdict(res, animated):
    if 'error' in res:
        return False
    if res.get('resized'):
        if animated:  # keep the animated rule, add the resampling level (do not replace it: A/A alone is > 2.0)
            return (res['mad'] <= ANIMATED_MAX_MAD + XRES_MAX_MAD
                    or res['off_pct'] <= ANIMATED_MAX_OFF_PCT + XRES_ALLOW_OFF16)
        return res['mad'] <= XRES_MAX_MAD and res['off32'] <= XRES_MAX_OFF32
    if animated:
        return res['mad'] <= ANIMATED_MAX_MAD or res['off_pct'] <= ANIMATED_MAX_OFF_PCT
    return res['mad'] <= STATIC_MAX_MAD


def line(label, res, animated, ok):
    if 'error' in res:
        return '%-28s FAIL  %s' % (label, res['error'])
    kind = ('animated' if animated else 'static') + (', cross-res' if res.get('resized') else '')
    return '%-28s %-4s  MAD %6.2f  off %6.2f%%  off32 %6.2f%%  max %3d  (%s, %s)' % (
        label, 'ok' if ok else 'FAIL', res['mad'], res['off_pct'], res['off32'], res['max'], res['size'], kind)


def captures_of(captures, run):
    out = {}
    for ext in ('.bmp', '.png'):  # Session 17: PNG by default (a label in both: the PNG wins)
        for p in glob.glob(os.path.join(captures, '%s_*%s' % (run, ext))):
            out[os.path.basename(p)[len(run) + 1:-len(ext)]] = p
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0], epilog=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='*', help='two BMP files to compare')
    ap.add_argument('--runs', nargs=2, metavar=('A', 'B'), help='compare two capture runs by label (e.g. 052 053)')
    ap.add_argument('--captures', default=DEFAULT_CAPTURES, help='captures folder (default: %(default)s)')
    ap.add_argument('--captures-b', help='captures folder for run B if different (e.g. a snapshot exe folder)')
    g = ap.add_mutually_exclusive_group()
    g.add_argument('--static', action='store_true', help='static thresholds (default)')
    g.add_argument('--animated', action='store_true', help='animated thresholds for every comparison')
    ap.add_argument('--cross-res', action='store_true',
                    help='allow different sizes: downscale the larger (Lanczos); static pairs are judged at the '
                         'resampling level, animated pairs by the animated rule plus that allowance')
    ap.add_argument('--animated-labels', default='', help='batch: regex of labels judged as animated')
    ap.add_argument('--labels', default='', help='batch: only labels matching this regex')
    ap.add_argument('--strict', action='store_true', help='batch: a label missing on one side is a failure')
    ap.add_argument('--heatmap', help='single pair: write a difference image (x4 gain) here')
    ap.add_argument('--heatmap-dir', help='batch: write LABEL.png difference images here')
    args = ap.parse_args()

    failures = 0
    if args.runs:
        ra, rb = ('%03d' % int(x) for x in args.runs)
        ca = captures_of(args.captures, ra)
        cb = captures_of(args.captures_b or args.captures, rb)
        labels = sorted(set(ca) | set(cb))
        if args.labels:
            labels = [x for x in labels if re.search(args.labels, x)]
        if not labels:
            print('no captures for %s / %s in %s' % (ra, rb, args.captures))
            return 1
        print('captures %s vs %s (%s)' % (ra, rb, args.captures))
        for label in labels:
            if label not in ca or label not in cb:
                missing = ra if label not in ca else rb
                print('%-28s %-4s  only in %s' % (label, 'FAIL' if args.strict else 'skip',
                                                   rb if missing == ra else ra))
                failures += 1 if args.strict else 0
                continue
            animated = args.animated or bool(args.animated_labels and re.search(args.animated_labels, label))
            heat = os.path.join(args.heatmap_dir, label + '.png') if args.heatmap_dir else None
            res = compare(ca[label], cb[label], heat, args.cross_res)
            ok = verdict(res, animated)
            failures += 0 if ok else 1
            print(line(label, res, animated, ok))
    elif len(args.files) == 2:
        res = compare(args.files[0], args.files[1], args.heatmap, args.cross_res)
        ok = verdict(res, args.animated)
        failures += 0 if ok else 1
        print(line(os.path.basename(args.files[1]), res, args.animated, ok))
    else:
        ap.error('give two BMP files or --runs A B')
    print('%d failure(s)' % failures)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
