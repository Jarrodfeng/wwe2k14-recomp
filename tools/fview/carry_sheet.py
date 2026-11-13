"""Session 101 lane 164: the carry-darkening picture. For a capture and a list of frames, renders with fview (headless, 0 game runs):
  A  the stream as captured            B  ps:170.w=1 (the shadow far weight forced to 1 on the wrestler draws)
  C  vs:170-171.w=0 (sphere occlusion strength zeroed)       D  B + C
and, to know WHERE each wrestler is, the frame again with each wrestler group's skin draws left out (--draw-drop @group=K) and with all the wrestler programs left out:
a wrestler's mask = the pixels that change when its draws go (visible pixels only; its gear draws carry no sphere constants, so only the skin draws form a group).
Writes one labelled contact sheet (crops of the pair at the source resolution) and a numbers file. Pictures stay under --out (out\\..., never committed).

  py -I tools\\fview\\carry_sheet.py --fview out\\renderer\\dev\\fview.exe --capture DIR --out out\\s101\\carry --frames 488,480,460,440,411,113
"""
import argparse, csv, json, math, pathlib, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ARMS = {
    'A': ('stream as captured', []),
    'B': ('ps:170.w=1', ['--const-override', 'ps:170.w=1']),
    'C': ('vs:170-171.w=0', ['--const-override', 'vs:170-171.w=0']),
    'D': ('both', ['--const-override', 'ps:170.w=1', '--const-override', 'vs:170-171.w=0']),
}
MASKS = {'M0': ['--draw-drop', '@class=wrestlers@group=0'], 'M1': ['--draw-drop', '@class=wrestlers@group=1'], 'MALL': ['--draw-drop', '@class=wrestlers']}
THRESH = 10


def run(fview, cap, frame, extra, out_dir, timeout=180):
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [str(pathlib.Path(fview).resolve()), '--capture', str(pathlib.Path(cap).resolve()), '--frame', str(frame)] + extra + ['--judge-out', str(out_dir)]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    lines = [l for l in r.stdout.splitlines() if l.startswith('[fview] const-override') or l.startswith('[fview] draw-drop')]
    if r.returncode != 0 or not (out_dir / 'picture.png').exists():
        raise SystemExit('fview failed (%d): %s\n%s' % (r.returncode, ' '.join(cmd), r.stdout[-800:] + r.stderr[-800:]))
    img = np.asarray(Image.open(out_dir / 'picture.png').convert('RGB')).astype(np.float64)
    return img, lines


def luma(img):
    return 0.2126 * img[..., 0] + 0.7152 * img[..., 1] + 0.0722 * img[..., 2]


def lin(img):
    c = img / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linluma(img):
    l = lin(img)
    return 0.2126 * l[..., 0] + 0.7152 * l[..., 1] + 0.0722 * l[..., 2]


def font(size):
    try:
        return ImageFont.load_default(size)
    except TypeError:
        return ImageFont.load_default()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fview', required=True)
    ap.add_argument('--capture', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--frames', required=True)
    ap.add_argument('--census', default='')
    import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import fview_exe as fe   # lane 218: one locator, refuses a stale exe
    fe.add_allow_stale(ap)
    a = ap.parse_args()
    a.fview = fe.find_or_exit(prefer=a.fview, allow_stale=a.allow_stale, kind='dev')
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    frames = [int(x) for x in a.frames.split(',')]

    census = {}
    if a.census and pathlib.Path(a.census).exists():
        for r in csv.DictReader(open(a.census)):
            census[(int(r['frame']), int(r['group']))] = r

    results = []
    rows = []
    for f in frames:
        imgs, lines = {}, {}
        for k, (name, extra) in ARMS.items():
            imgs[k], lines[k] = run(a.fview, a.capture, f, extra, out / 'arms' / ('f%d_%s' % (f, k)))
        for k, extra in MASKS.items():
            imgs[k], lines[k] = run(a.fview, a.capture, f, extra, out / 'arms' / ('f%d_%s' % (f, k)))
        base = imgs['A']
        masks = {}
        for g, k in ((0, 'M0'), (1, 'M1'), ('all', 'MALL')):
            masks[g] = np.abs(imgs[k] - base).max(axis=2) > THRESH
        res = {'frame': f, 'weights': {}, 'distance': None, 'changed_draws': {}, 'wrestlers': {}}
        for g in (0, 1):
            c = census.get((f, g))
            res['weights'][g] = (float(c['w_min']), float(c['w_max'])) if c else None
        if census.get((f, 0)) and census.get((f, 1)):
            p = [[float(census[(f, g)]['vs168' + c]) for c in 'xyz'] for g in (0, 1)]
            res['distance'] = math.dist(p[0], p[1])
        for k in ARMS:
            for l in lines[k]:
                if 'last bound frame' in l:
                    res['changed_draws'][k] = l.split('last bound frame:')[1].split(';')[0].strip()
        for g in (0, 1, 'all'):
            m = masks[g]
            n = int(m.sum())
            ys, xs = np.nonzero(m)
            w = {'pixels': n, 'bbox': [int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())] if n else None}
            for k in ARMS:
                w[k] = {'luma': float(luma(imgs[k])[m].mean()) if n else None, 'linear': float(linluma(imgs[k])[m].mean()) if n else None,
                        'moved_gt8': int((np.abs(imgs[k] - base).max(axis=2)[m] > 8).sum()) if n else 0}
            res['wrestlers'][str(g)] = w
        results.append(res)

        # the crop: both skin masks and the rest of the wrestler pixels, with a margin
        m = masks['all'] | masks[0] | masks[1]
        ys, xs = np.nonzero(m)
        if len(xs):
            pad = 40
            x0, x1 = max(0, int(xs.min()) - pad), min(base.shape[1], int(xs.max()) + pad)
            y0, y1 = max(0, int(ys.min()) - pad), min(base.shape[0], int(ys.max()) + pad)
        else:
            x0, y0, x1, y1 = 0, 0, base.shape[1], base.shape[0]
        diff = np.clip(np.abs(imgs['B'] - imgs['A']) * 4.0, 0, 255)
        panels = []
        for k in ('A', 'B', 'C', 'D'):
            panels.append((k, ARMS[k][0], imgs[k][y0:y1, x0:x1]))
        panels.append(('B-A', 'difference B minus A, x4', diff[y0:y1, x0:x1]))
        rows.append((f, res, panels))

    # the sheet: one row per frame; every panel labelled
    pw = max(p[2].shape[1] for r in rows for p in r[2])
    ph = max(p[2].shape[0] for r in rows for p in r[2])
    head, gap = 74, 8
    cols = 5
    W = cols * pw + (cols + 1) * gap
    H = len(rows) * (ph + head + gap) + gap + 60
    sheet = Image.new('RGB', (W, H), (24, 24, 28))
    d = ImageDraw.Draw(sheet)
    d.text((gap, 8), 'Carry darkening: top lead. PS c170.w (the shadow far weight) forced to 1 on the wrestler draws; VS c170-171.w (sphere occlusion strength) zeroed. Source resolution crops of the pair.', fill=(235, 235, 235), font=font(22))
    d.text((gap, 34), 'Lane 164, capture 20261112_172456_s100_owner_match (CPU v CPU, no carry). Luma = mean 0-255 inside the wrestler skin mask (visible pixels). W0 / W1 = the two wrestler skin groups.', fill=(200, 200, 200), font=font(18))
    y = 60
    for f, res, panels in rows:
        w0, w1 = res['weights'][0], res['weights'][1]
        low = 'W0' if (w0 and w1 and w0[0] < w1[0]) else ('W1' if (w0 and w1 and w1[0] < w0[0]) else 'neither')
        title = 'frame %d   pair distance %s   c170.w: W0 %s, W1 %s   lowered: %s' % (
            f, '%.1f' % res['distance'] if res['distance'] is not None else '?', '%.3f' % w0[0] if w0 else '?', '%.3f' % w1[0] if w1 else '?', low)
        d.text((gap, y), title, fill=(255, 220, 120), font=font(24))
        x = gap
        for k, name, img in panels:
            sub = ('%s: %s' % (k, name))
            if k in ARMS:
                l0, l1 = res['wrestlers']['0'][k]['luma'], res['wrestlers']['1'][k]['luma']
                sub += '   luma W0 %.1f  W1 %.1f' % (l0 if l0 is not None else float('nan'), l1 if l1 is not None else float('nan'))
            d.text((x, y + 30), sub, fill=(255, 255, 255), font=font(18))
            sheet.paste(Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)), (x, y + head - 6))
            x += pw + gap
        y += ph + head + gap
    sheet_path = out / 'carry_sheet.png'
    sheet.save(sheet_path)

    # a full-frame pair of the strongest row (stream | override), labelled
    best = min(rows, key=lambda r: min((r[1]['weights'][0] or (9, 9))[0], (r[1]['weights'][1] or (9, 9))[0]))
    f = best[0]
    A = Image.open(out / 'arms' / ('f%d_A' % f) / 'picture.png').convert('RGB')
    B = Image.open(out / 'arms' / ('f%d_B' % f) / 'picture.png').convert('RGB')
    full = Image.new('RGB', (A.width * 2 + 24, A.height + 56), (24, 24, 28))
    dd = ImageDraw.Draw(full)
    dd.text((8, 10), 'frame %d  A: stream as captured' % f, fill=(255, 255, 255), font=font(28))
    dd.text((A.width + 16, 10), 'frame %d  B: ps:170.w=1 on the wrestler draws' % f, fill=(255, 255, 255), font=font(28))
    full.paste(A, (8, 48))
    full.paste(B, (A.width + 16, 48))
    full_path = out / ('carry_full_f%d.png' % f)
    full.save(full_path)

    (out / 'carry_numbers.json').write_text(json.dumps(results, indent=1))
    print('sheet', sheet_path, sheet.size)
    print('full', full_path)
    for r in results:
        print('frame %d  dist %s  weights %s' % (r['frame'], '%.1f' % r['distance'] if r['distance'] is not None else '?', r['weights']))
        for g in ('0', '1', 'all'):
            w = r['wrestlers'][g]
            if not w['pixels']:
                print('   W%s mask empty' % g)
                continue
            s = '   W%s %6d px  luma ' % (g, w['pixels']) + '  '.join('%s %.2f' % (k, w[k]['luma']) for k in ARMS)
            s += '  | B-A %+.2f (%+.1f%%)  C-A %+.2f  D-A %+.2f  | px moved>8 by B: %d' % (
                w['B']['luma'] - w['A']['luma'], 100 * (w['B']['luma'] / w['A']['luma'] - 1), w['C']['luma'] - w['A']['luma'], w['D']['luma'] - w['A']['luma'], w['B']['moved_gt8'])
            print(s)


if __name__ == '__main__':
    main()
