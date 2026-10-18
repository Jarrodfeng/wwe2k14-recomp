"""Session 16: a small contact sheet of images (rows x columns) for checks; output stays under out\\.

    py tools/replay/contact_sheet.py OUT.png --cols N --width W IMG1 IMG2 ...  [--labels a,b,...]
"""
import argparse

from PIL import Image, ImageDraw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('images', nargs='+')
    ap.add_argument('--cols', type=int, default=2)
    ap.add_argument('--width', type=int, default=640)
    ap.add_argument('--labels', default='')
    a = ap.parse_args()
    ims = [Image.open(p).convert('RGB') for p in a.images]
    labels = a.labels.split(',') if a.labels else [''] * len(ims)
    w = a.width
    h = int(ims[0].height * w / ims[0].width)
    rows = (len(ims) + a.cols - 1) // a.cols
    sheet = Image.new('RGB', (w * a.cols, h * rows), 'black')
    d = ImageDraw.Draw(sheet)
    for i, im in enumerate(ims):
        x, y = (i % a.cols) * w, (i // a.cols) * h
        sheet.paste(im.resize((w, h)), (x, y))
        if i < len(labels) and labels[i]:
            d.rectangle([x, y, x + 8 * len(labels[i]) + 8, y + 16], fill='black')
            d.text((x + 4, y + 2), labels[i], fill='white')
    sheet.save(a.out)
    print(a.out, sheet.size)


if __name__ == '__main__':
    main()
