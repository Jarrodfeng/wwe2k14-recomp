#!/usr/bin/env python3
"""Lane 35: sheets that show OUR draws (the stands members' blocks) against the stream, not the stream's own cards (0 game runs).

  py -I tools\\crowd\\crowd_blocks_sheet.py [--root DIR] [--frames N,N,N | --top 3] [--no-build]

Per frame: arms stream (switch off), ours (--crowd-ours), hidden (--crowd-fault hide_ours: the same frame with the stream's block draws dropped and none of ours recorded).
The mask = pixels ours changed relative to hidden (the pixels our injected draws wrote). Output in out\\s101\\crowd\\ (of the main checkout when this runs in a worktree):
  ours_blocks_fNNN.png        stream | ours | ours with the mask tinted and outlined | difference x4, every panel labelled
  (the far cards stay the stream's, so these sheets show only the stands members' blocks that WE draw; every member at its own LOD group's body and palette)
  ours_blocks_zoom_fNNN.png   a 3x crop centred where our bodies cover most: stream | ours
--top N: the N frames with the most members drawn by us (a replay with --crowd-census)."""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))


def main_checkout(here):
    try:
        c = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if c:
            return os.path.dirname(c)
    except Exception:
        pass
    return here


def run(cmd, timeout=900):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--frames', default='')
    ap.add_argument('--top', type=int, default=3)
    ap.add_argument('--no-build', action='store_true')
    a = ap.parse_args()
    import numpy as np
    from PIL import Image, ImageDraw
    root = os.path.abspath(a.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    out = os.path.join(root, 'out', 's101', 'crowd')
    cap = os.path.join(out, 'case_brock', 'capture')
    if not os.path.isdir(os.path.join(cap, 'scene')):
        print('FAIL no capture at ' + cap + ' (run crowd_ours_check.py first: it unpacks Check B\'s zip)')
        return 1
    if not a.no_build:
        rc, t = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], 1800)
        if rc:
            print(t[-2000:])
            return 1
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    exe = _fe.find_or_exit(root=root, kind='dev')
    frames = [int(x) for x in a.frames.split(',') if x]
    if not frames:
        csv = os.path.join(out, 'census.csv')
        rc, t = run([exe, '--capture', cap, '--crowd-ours', '--game', game, '--replay', '--frames', '9999', '--crowd-census', csv])
        if rc:
            print(t[-2000:])
            return 1
        rows = [l.strip().split(',') for l in open(csv) if l.strip()][1:]
        r = sorted(((int(f), int(m)) for f, m in rows), key=lambda x: -x[1])
        print('members drawn by us, top frames: ' + ', '.join('f%d=%d' % x for x in r[:8]))
        frames = []
        for f, m in r:  # the top N, at least 6 frames apart (neighbours are the same picture)
            if all(abs(f - g) >= 6 for g in frames):
                frames.append(f)
            if len(frames) == a.top:
                break
    lum = np.array([0.3, 0.59, 0.11])
    L = lambda im: im @ lum
    for f in frames:
        arms = {}
        for name, extra in (('stream', []), ('ours', ['--crowd-ours']), ('hidden', ['--crowd-ours', '--crowd-fault', 'hide_ours'])):
            d = os.path.join(out, 'blk_%d_%s' % (f, name))
            rc, t = run([exe, '--capture', cap, '--frame', str(f), '--judge-out', d] + extra + (['--game', game] if extra else []))
            png = os.path.join(d, 'picture.png')
            if rc or not os.path.isfile(png):
                print(t[-1500:])
                return 1
            arms[name] = np.asarray(Image.open(png).convert('RGB'), dtype=np.float64)
            if name == 'ours':
                census = [l for l in t.splitlines() if 'members' in l and 'pairing' in l]
                print('f%d: %s' % (f, census[-1] if census else ''))
        stream, ours, hidden = arms['stream'], arms['ours'], arms['hidden']
        mask = np.abs(L(ours) - L(hidden)) > 3.0
        # outline = mask minus its erosion
        er = mask.copy()
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            er &= np.roll(mask, (dy, dx), (0, 1))
        edge = mask & ~er
        tint = ours.copy()
        tint[mask] = tint[mask] * 0.6 + np.array([255, 0, 255]) * 0.4
        tint[edge] = np.array([255, 255, 0])
        diff = np.clip(np.abs(L(ours) - L(stream))[:, :, None].repeat(3, 2) * 4.0, 0, 255)
        mad = float(np.abs(L(ours) - L(stream)).mean())
        n_mask = int(mask.sum())

        def panel(im, title):
            p = Image.fromarray(np.clip(im, 0, 255).astype(np.uint8))
            o = Image.new('RGB', (1280, 750), (20, 20, 20))
            o.paste(p, (0, 30))
            ImageDraw.Draw(o).text((8, 8), title, fill=(255, 255, 255))
            return o
        sheet = Image.new('RGB', (2560, 1500))
        titles = ['STREAM (reference: the game\'s own draws; Check B run 7051 frame %d, switch off)' % f,
                  'OURS (--crowd-ours: stands members\' blocks drawn by us from scene record %d; far cards stay the stream\'s)' % (f - 1),
                  'OURS with OUR draws marked: magenta tint + yellow outline = pixels our injected draws wrote (%d px)' % n_mask,
                  'DIFFERENCE |ours - stream| x4 (luminance); mean %.2f' % mad]
        for i, (im, tt) in enumerate(zip([stream, ours, tint, diff], titles)):
            sheet.paste(panel(im, tt), (1280 * (i % 2), 750 * (i // 2)))
        sheet.save(os.path.join(out, 'ours_blocks_f%d.png' % f))
        # zoom: the 320x180 window (3x) with the most mask pixels
        H, W = mask.shape
        ii = np.pad(mask.astype(np.int64).cumsum(0).cumsum(1), ((1, 0), (1, 0)))
        best, at = -1, (0, 0)
        for y in range(0, H - 180, 20):
            for x in range(0, W - 320, 20):
                s = ii[y + 180, x + 320] - ii[y, x + 320] - ii[y + 180, x] + ii[y, x]
                if s > best:
                    best, at = s, (x, y)
        x, y = at
        z = Image.new('RGB', (1920, 600), (20, 20, 20))
        for i, (im, tt) in enumerate([(stream, 'STREAM, 3x of (%d,%d)-(%d,%d)' % (x, y, x + 320, y + 180)), (ours, 'OURS, 3x (our bodies cover %d px of this window)' % best)]):
            c = Image.fromarray(np.clip(im, 0, 255).astype(np.uint8)).crop((x, y, x + 320, y + 180)).resize((960, 540), Image.LANCZOS)
            z.paste(c, (960 * i, 40))
            ImageDraw.Draw(z).text((960 * i + 8, 12), tt, fill=(255, 255, 255))
        z.save(os.path.join(out, 'ours_blocks_zoom_f%d.png' % f))
        print('f%d: mask %d px, mean diff %.2f, zoom window at (%d,%d)' % (f, n_mask, mad, x, y))
    print('wrote sheets under ' + out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
