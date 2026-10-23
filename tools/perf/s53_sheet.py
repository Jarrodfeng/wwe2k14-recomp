"""Session 53: a numbered contact sheet of a run's screenshots (captures\\RUN_LABEL_NN.png).

    py tools\\perf\\s53_sheet.py RUN LABEL OUT.jpg [--cols 5] [--w 400]
"""
import argparse
import glob

from PIL import Image, ImageDraw

ap = argparse.ArgumentParser()
ap.add_argument('run')
ap.add_argument('label')
ap.add_argument('out')
ap.add_argument('--cols', type=int, default=5)
ap.add_argument('--w', type=int, default=400)
ap.add_argument('--dir', default=r'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\captures')
a = ap.parse_args()
fs = sorted(glob.glob(fr'{a.dir}\{a.run}_{a.label}_*.png'))
W, H = a.w, a.w * 9 // 16
rows = (len(fs) + a.cols - 1) // a.cols
sheet = Image.new('RGB', (W * a.cols, H * max(rows, 1)))
for i, f in enumerate(fs):
    im = Image.open(f).convert('RGB').resize((W, H))
    ImageDraw.Draw(im).text((4, 4), f[-6:-4], fill=(255, 255, 0))
    sheet.paste(im, ((i % a.cols) * W, (i // a.cols) * H))
sheet.save(a.out, quality=85)
print(len(fs), 'shots ->', a.out)
