"""Side-by-side pictures of the emulated and the native renderer (Session 38), for Jarrod's summaries.

Takes screenshots of the same frozen moment (the s38 routes take them with gpu_native_view 4 = the emulated scene
from the EDRAM, 1 = native, and 0 = the final emulated picture) and puts them next to each other, each labelled, at
half size, plus a magnified crop of the same spot from each. Output goes to out\\native\\ only (game pictures never go
into the repository).

    py tools\\native\\side_by_side.py LEFT.png RIGHT.png OUT.png [--labels "emulated,native"] [--crop X,Y,W,H]
"""
import argparse

from PIL import Image, ImageDraw, ImageFont


def label(img, text):
    d = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype('arialbd.ttf', max(18, img.height // 30))
    except OSError:
        font = ImageFont.load_default()
    box = d.textbbox((0, 0), text, font=font)
    pad = 8
    d.rectangle((0, 0, box[2] + 2 * pad, box[3] + 2 * pad), fill=(0, 0, 0))
    d.text((pad, pad), text, fill=(255, 255, 255), font=font)
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('left')
    ap.add_argument('right')
    ap.add_argument('out')
    ap.add_argument('--labels', default='emulated (EDRAM),native')
    ap.add_argument('--crop', help='X,Y,W,H in full-size pixels: a magnified crop of both under the pictures')
    a = ap.parse_args()
    names = a.labels.split(',')
    left, right = Image.open(a.left).convert('RGB'), Image.open(a.right).convert('RGB')
    if right.size != left.size:  # a native picture at another gpu_native_scale: shown at the emulated size
        right = right.resize(left.size, Image.LANCZOS)
    w, h = left.width // 2, left.height // 2
    halves = [label(left.resize((w, h), Image.LANCZOS), names[0]), label(right.resize((w, h), Image.LANCZOS), names[1])]
    rows = h
    crops = []
    if a.crop:
        x, y, cw, ch = (int(v) for v in a.crop.split(','))
        scale = w / cw
        crops = [img.crop((x, y, x + cw, y + ch)).resize((w, int(ch * scale)), Image.NEAREST) for img in (left, right)]
        rows += crops[0].height
    out = Image.new('RGB', (2 * w + 8, rows + (8 if crops else 0)), (255, 255, 255))
    out.paste(halves[0], (0, 0))
    out.paste(halves[1], (w + 8, 0))
    for i, c in enumerate(crops):
        out.paste(label(c, f'{names[i]}, {a.crop} magnified'), (i * (w + 8), h + 8))
    out.save(a.out)
    print(f'{a.out}: {out.width}x{out.height}')


if __name__ == '__main__':
    main()
