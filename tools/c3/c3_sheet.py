#!/usr/bin/env python3
"""S102 lane C3: the sheet. Presented frames of the standard Bryan, match and Check B / brock captures, the whole bundle with both switches OFF against ON, side by side with the pixel difference
(the hash says byte-equal; the sheet lets Jarrod see it). fview headless (--dump-frames), no window.
  py -I tools\\c3\\c3_sheet.py [--root DIR] [--out DIR]"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import c3_check as c  # noqa: E402

CASES = (('bryan', 'entrance frame 60', 60), ('bryan', 'entrance frame 100', 100), ('match', 'match frame 60', 60), ('brock', 'Check B brock frame 60', 60))


def dump(exe, cap, game, work, tag, flags, frame):
    out = os.path.join(work, 'sheet_' + tag)
    os.makedirs(out, exist_ok=True)
    kind, path = cap
    cmd = [exe, '--case' if kind == 'zip' else '--capture', path, '--replay', '--frames', str(frame + 1), '--validation', '1', '--game', game, '--dump-frames', str(frame), '--dump-dir', out + os.sep] + flags
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=1700, errors='replace')
    files = [f for f in os.listdir(out) if f.lower().endswith('.png')]
    if p.returncode != 0 or not files:
        raise RuntimeError('fview %s exit %d: %s' % (tag, p.returncode, (p.stdout + p.stderr)[-600:]))
    return os.path.join(out, sorted(files)[-1])


def main():
    from PIL import Image, ImageChops, ImageDraw
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--out', default=os.path.join(c.main_checkout(ROOT), 'out', 's102', 'sheets', 'c3'))
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    os.makedirs(a.out, exist_ok=True)
    rows = []
    for which, label, frame in CASES:
        s = c.setup(root, which)
        if not s:
            continue
        exe, cap, game, work = s
        off = dump(exe, cap, game, work, '%s_%d_off' % (which, frame), c.BUNDLE, frame)
        on = dump(exe, cap, game, work, '%s_%d_on' % (which, frame), c.BUNDLE + c.SD + c.FD, frame)
        ia, ib = Image.open(off).convert('RGB'), Image.open(on).convert('RGB')
        diff = ImageChops.difference(ia, ib)
        bbox = diff.getbbox()
        nd = sum(1 for px in diff.getdata() if px != (0, 0, 0)) if bbox else 0
        rows.append((label, ia, ib, nd))
        print('%s: %dx%d, %d pixels differ' % (label, ia.size[0], ia.size[1], nd))
    if not rows:
        return 1
    tw = 640
    th = int(rows[0][1].size[1] * tw / rows[0][1].size[0])
    sheet = Image.new('RGB', (tw * 2 + 30, (th + 40) * len(rows) + 10), (24, 24, 24))
    d = ImageDraw.Draw(sheet)
    for i, (label, ia, ib, nd) in enumerate(rows):
        y = 10 + i * (th + 40)
        d.text((10, y), '%s   OFF (whole bundle)' % label, fill=(255, 255, 255))
        d.text((tw + 20, y), 'ON (shadow direct + frame direct)   pixels differing from OFF: %d' % nd, fill=(120, 255, 120) if nd == 0 else (255, 120, 120))
        sheet.paste(ia.resize((tw, th)), (10, y + 16))
        sheet.paste(ib.resize((tw, th)), (tw + 20, y + 16))
    path = os.path.join(a.out, 'c3_off_vs_on.png')
    sheet.save(path)
    print('sheet: ' + path)
    return 0


if __name__ == '__main__':
    sys.exit(main())
