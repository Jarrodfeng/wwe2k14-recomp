"""Session 101 lane 191: how strong is the ring-mat (plane) shadow in a capture, frame by frame. fview headless, 0 game runs, one process at a time.
For each frame: the plane chain's own images (the mask target = the first target of the frame's plane-mask draws; the final blurred map = the target of the
last yBlur plane draw), and the picture with the chain's mask draws dropped (--draw-drop @class=all@site=0x82BF4D38, so the mask stays cleared and the mat
reads zero shadow) against the picture as captured: the pixels the chain darkens.
  py -I tools\\fview\\mat_shadow_census.py --fview out\\renderer\\dev\\fview.exe --capture DIR --out DIR --frames A:B:STEP [--thr 3]
Prints one line per frame and writes census.json (numbers only; pictures stay under --out, never committed)."""
import argparse, json, pathlib, subprocess
import numpy as np
from PIL import Image

PLANE_SITE = 0x82BF4D38


def fv(fview, cap, frame, extra, timeout=240):
    cmd = [str(pathlib.Path(fview).resolve()), '--capture', str(pathlib.Path(cap).resolve()), '--frame', str(frame)] + extra
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        raise SystemExit('fview failed (%d): %s\n%s' % (r.returncode, ' '.join(cmd), r.stdout[-600:] + r.stderr[-600:]))
    return r.stdout


def plane_ids(ops):
    """(mask target id, last plane-blur target id) of the frame, or None when it has no plane chain."""
    tgt, mask, blurs = None, None, []
    for o in ops:
        if o['kind'] == 'targets':
            tgt = o
        elif o['kind'] == 'draw':
            if o.get('pass_site') == PLANE_SITE and mask is None:
                mask = tgt['colors'][0]
            elif o.get('technique') == 'T_PlaneShadowBlur':
                blurs.append(tgt['colors'][0])
    return (mask, blurs[-1]) if mask is not None and blurs else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fview', required=True)
    ap.add_argument('--capture', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--frames', required=True, help='A:B:STEP or a comma list')
    ap.add_argument('--thr', type=int, default=3)
    ap.add_argument('--masks-only', action='store_true', help='only the chain images (about 5 s a frame); no picture arms (about 50 s a frame)')
    ap.add_argument('--extra', default='--thin arena,ring,wrestlers --crowd-ours --arena-ours --wrestlers-ours --ring-ours --flash-ours --props-ours --post-ours',
                    help='fview flags of the two picture arms (default: every promotion a thin match capture needs to show the ring; the mask / blur images read with none)')
    import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import fview_exe as fe   # lane 218: one locator, refuses a stale exe
    fe.add_allow_stale(ap)
    a = ap.parse_args()
    a.fview = fe.find_or_exit(prefer=a.fview, allow_stale=a.allow_stale, kind='dev')
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    if ':' in a.frames:
        p = [int(x) for x in a.frames.split(':')]
        frames = list(range(p[0], p[1], p[2] if len(p) > 2 else 1))
    else:
        frames = [int(x) for x in a.frames.split(',')]
    rows = []
    for f in frames:
        ojson = out / ('ops_%d.json' % f)
        fv(a.fview, a.capture, f, ['--ops', '--json', str(ojson)])
        ids = plane_ids(json.load(open(ojson))['ops'])
        ojson.unlink()
        if ids is None:
            print('frame %d: no plane chain' % f)
            continue
        m_id, b_id = ids
        fv(a.fview, a.capture, f, ['--image-png', '%d,%s' % (m_id, out / ('mask_%d.png' % f))])
        fv(a.fview, a.capture, f, ['--image-png', '%d,%s' % (b_id, out / ('blur_%d.png' % f))])
        r = dict(frame=f, mask_id=m_id, blur_id=b_id)
        mk = np.asarray(Image.open(out / ('mask_%d.png' % f)).convert('RGBA'))[..., 0].astype(np.int32)
        bl = np.asarray(Image.open(out / ('blur_%d.png' % f)).convert('RGBA'))[..., 0].astype(np.int32)
        r.update(mask_over8=int((mk > 8).sum()), mask_max=int(mk.max()), mask_over128=int((mk > 128).sum()), mask_over240=int((mk > 240).sum()),
                 mask_mean_covered=round(float(mk[mk > 8].mean()), 1) if (mk > 8).any() else 0.0,
                 blur_over8=int((bl > 8).sum()), blur_max=int(bl.max()), blur_over64=int((bl > 64).sum()))
        if a.masks_only:
            rows.append(r)
            print(json.dumps(r))
            continue
        fv(a.fview, a.capture, f, ['--judge-out', str(out / ('A_%d' % f))] + a.extra.split())
        fv(a.fview, a.capture, f, ['--draw-drop', '@class=all@site=0x%X' % PLANE_SITE, '--judge-out', str(out / ('X_%d' % f))] + a.extra.split())
        A = np.asarray(Image.open(out / ('A_%d' % f) / 'picture.png').convert('RGB')).astype(np.int32)
        X = np.asarray(Image.open(out / ('X_%d' % f) / 'picture.png').convert('RGB')).astype(np.int32)
        dm = np.abs(X - A).max(axis=2)
        d = (X - A).max(axis=2)
        r.update(px_changed=int((dm > a.thr).sum()), px_changed_over20=int((dm > 20).sum()), max_levels=int(dm.max()), picture=list(A.shape[:2]),
                 mean_dark_changed=round(float(d[dm > a.thr].mean()), 1) if (dm > a.thr).any() else 0.0)
        rows.append(r)
        print(json.dumps(r))
    json.dump(rows, open(out / 'census.json', 'w'), indent=1)


if __name__ == '__main__':
    main()
