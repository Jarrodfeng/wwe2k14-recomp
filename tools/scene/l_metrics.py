#!/usr/bin/env python3
r"""Lane L (Session 103): per-family distance of the rig's pictures to the game's, from a frame_rig.py work folder.

  py -I tools\scene\l_metrics.py WORKDIR FRAME [--json OUT]       WORKDIR = <out>\work_<capture>

Per family panel (ours_N_<family>.png, the family drawn alone) over the pixels it covers (not the clear colour, which is the corner of the EMPTY ring panel when there is one,
else the corner of the family panel): mean |diff| to the game picture in % of full scale, mean luminance ratio ours / game, and the share of covered pixels that are
brighter than the game by > 25 % of full scale ("washed"). Plus the whole picture (all pixels). Numbers only: no picture is written.
"""
import argparse, json, os, sys
import numpy as np
from PIL import Image

FAMILIES = ['arena', 'ring', 'props', 'wrestlers', 'crowd', 'screens', 'flashes', 'particles', 'other']


def load(p):
    return np.asarray(Image.open(p).convert('RGB'), dtype=np.float64)


def lum(a):
    return a @ np.array([0.2126, 0.7152, 0.0722])


def coverage(work, frame, fam):
    """the pixels family `fam` covers in this arm (not the clear colour, which is the corner of the ring panel when there is one)"""
    pre = os.path.join(work, "ours_%d" % frame)
    p = pre + "_%s.png" % fam
    if not os.path.exists(p):
        return None
    a = load(p)
    ring = pre + "_ring.png"
    c = load(ring)[0, 0] if os.path.exists(ring) else a[0, 0]
    return np.abs(a - c).max(axis=2) > 6


def compute(work, frame, ref_work=None, game_png=None):
    """ref_work: the arm whose coverage mask is used for EVERY arm (the before arm), so a darker arm is not measured over fewer pixels (review 6)
    game_png: the reference picture (default: the game's PRESENTED frame, work/game/frame_NNNNN.png; l_check passes the game's scene target before post and 2D: scene_ref.py)"""
    game = load(game_png or os.path.join(work, 'game', 'frame_%05d.png' % frame))
    pre = os.path.join(work, 'ours_%d' % frame)
    out = {}
    whole = load(pre + '_whole.png')
    d = np.abs(whole - game).mean(axis=2) / 255.0
    out['whole'] = {'mean_diff_pct': round(float(d.mean() * 100), 2), 'lum_ratio': round(float(lum(whole).mean() / max(lum(game).mean(), 1e-6)), 3), 'washed_pct': round(float(((lum(whole) - lum(game)) / 255.0 > 0.25).mean() * 100), 2)}
    clear = None
    ring = pre + '_ring.png'
    if os.path.exists(ring):
        clear = load(ring)[0, 0]
    for fam in FAMILIES:
        p = pre + '_%s.png' % fam
        if not os.path.exists(p):
            continue
        a = load(p)
        c = clear if clear is not None else a[0, 0]
        cov = (coverage(ref_work, frame, fam) if ref_work else None)
        if cov is None:
            cov = np.abs(a - c).max(axis=2) > 6
        n = int(cov.sum())
        if n < 50:
            out[fam] = {'covered_px': n}
            continue
        dd = (np.abs(a - game).mean(axis=2) / 255.0)[cov]
        la, lg = lum(a)[cov], lum(game)[cov]
        out[fam] = {'covered_px': n, 'mean_diff_pct': round(float(dd.mean() * 100), 2), 'lum_ratio': round(float(la.mean() / max(lg.mean(), 1e-6)), 3), 'washed_pct': round(float(((la - lg) / 255.0 > 0.25).mean() * 100), 2)}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('work')
    ap.add_argument('frame', type=int)
    ap.add_argument('--json', default=None)
    ap.add_argument('--ref', default=None, help='the work folder of the arm whose coverage mask is used')
    args = ap.parse_args()
    out = compute(args.work, args.frame, args.ref)
    for k, v in out.items():
        print('%-10s %s' % (k, v))
    if args.json:
        json.dump(out, open(args.json, 'w'), indent=1)


if __name__ == '__main__':
    sys.exit(main())
