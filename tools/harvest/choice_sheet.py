#!/usr/bin/env python3
r"""S103 WGD: the picture of the whole-game choice diff (tools\harvest\choice_diff.py): arenas down, mismatch classes across, the count in each cell, red where non-zero,
and the five largest classes named with one example mesh each under the grid. Our own counts and names only (no game picture): may live anywhere under out\.

  py -I tools\harvest\choice_sheet.py MATRIX.json OUT.png [--title TEXT]      (choice_diff.py --sheet calls draw() itself)"""
import argparse
import json
import os
import sys

from PIL import Image, ImageDraw, ImageFont


def _font(size):
    for nm in ('consola.ttf', 'arial.ttf', 'DejaVuSansMono.ttf'):
        try:
            return ImageFont.truetype(nm, size)
        except OSError:
            continue
    return ImageFont.load_default()


def arena_names():
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'db', 'ids', 'arenas.json')
    try:
        return {e['id']: e['name'] for e in json.load(open(p, encoding='utf-8'))['entries']}
    except (OSError, ValueError, KeyError):
        return {}


def draw(matrix, out_png, title='choice diff'):
    arenas, classes = matrix['arenas'], matrix['classes']
    names = arena_names()
    f, fs, fb = _font(15), _font(13), _font(20)
    cw, rh, lw, hh = 46, 22, 230, 360   # cell width, row height, label column, header (rotated class numbers + names)
    ncol = max(1, len(classes))
    grid_w = lw + cw * ncol
    top5 = sorted(classes, key=lambda c: -matrix['totals'][c])[:5]
    legend_h = 26 * (len(classes) + 1) + 30 * 6 + 60
    heads = matrix.get('headline') or []
    top_h = 20 * len(heads) + 10
    W = max(1900, grid_w + 40)
    H = 50 + top_h + hh + rh * (len(arenas) + 1) + legend_h
    im = Image.new('RGB', (W, H), (250, 250, 250))
    d = ImageDraw.Draw(im)
    d.text((20, 12), title, fill=(0, 0, 0), font=fb)
    for i, h in enumerate(heads):   # the headline per family, word for word as the text table prints it
        d.text((20, 44 + 20 * i), h[:230], fill=(30, 30, 120), font=fs)
    y0 = 50 + hh + top_h
    for j, c in enumerate(classes):   # header: the class number and a short name, rotated
        x = lw + j * cw
        lab = '%d %s' % (j + 1, c[:44])
        tmp = Image.new('RGBA', (hh - 10, cw - 6), (0, 0, 0, 0))
        ImageDraw.Draw(tmp).text((2, 6), lab, fill=(0, 0, 0), font=fs)
        tmp = tmp.rotate(90, expand=True)
        im.paste(tmp, (x + 3, 50 + top_h), tmp)
    for i, a in enumerate(arenas):
        y = y0 + i * rh
        d.rectangle([0, y, grid_w, y + rh], fill=(255, 255, 255) if i % 2 else (240, 240, 244))
        d.text((8, y + 3), 'a%02d %s' % (a, names.get(a, '')[:24]), fill=(0, 0, 0), font=f)
        for j, c in enumerate(classes):
            n = matrix['counts'][c].get(str(a), 0)
            x = lw + j * cw
            if n:
                d.rectangle([x + 1, y + 1, x + cw - 1, y + rh - 1], fill=(220, 40, 40))
                d.text((x + 5, y + 3), str(n), fill=(255, 255, 255), font=f)
            else:
                d.text((x + cw // 2 - 3, y + 3), '.', fill=(170, 170, 170), font=f)
    y = y0 + len(arenas) * rh
    d.text((8, y + 3), 'total', fill=(0, 0, 0), font=f)
    for j, c in enumerate(classes):
        d.text((lw + j * cw + 3, y + 3), str(matrix['totals'][c]), fill=(150, 0, 0), font=fs)
    for j in range(ncol + 1):
        d.line([lw + j * cw, y0, lw + j * cw, y + rh], fill=(200, 200, 200))
    y += rh + 20
    d.text((8, y), 'classes (count = meshes x looks; distinct meshes):', fill=(0, 0, 0), font=fb)
    y += 28
    for j, c in enumerate(classes):
        d.text((8, y), '%2d  %5d  %4d  %s' % (j + 1, matrix['totals'][c], matrix['meshes'][c], c[:150]), fill=(0, 0, 0), font=fs)
        y += 22
    y += 14
    d.text((8, y), 'the five largest, one example each:', fill=(0, 0, 0), font=fb)
    y += 28
    for c in top5:
        e = matrix['examples'][c]
        d.text((8, y), '%s' % c[:150], fill=(150, 0, 0), font=fs)
        d.text((28, y + 15), 'a%02d %s %s mesh %s %s: game %s / ours %s' % (e['arena'], e['look'], e['file'], e['mesh'], e['effect'], e['game'], e['ours']), fill=(0, 0, 0), font=fs)
        y += 32
    nc = matrix.get('not_compared') or {}
    if nc:
        y += 6
        d.text((8, y), 'not compared: ' + '; '.join('%s %d' % (k, v) for k, v in sorted(nc.items(), key=lambda kv: -kv[1]))[:220], fill=(90, 90, 90), font=fs)
    im = im.crop((0, 0, W, min(H, y + 40)))
    os.makedirs(os.path.dirname(os.path.abspath(out_png)), exist_ok=True)
    im.save(out_png)
    return out_png


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('matrix')
    ap.add_argument('out')
    ap.add_argument('--title', default='choice diff')
    a = ap.parse_args()
    print('wrote', draw(json.load(open(a.matrix)), a.out, a.title))
    return 0


if __name__ == '__main__':
    sys.exit(main())
