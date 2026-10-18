"""Session 22 item 5: a side-by-side sheet of the same replay frames rendered by two runs (FSI left, FBO right).

    py tools/perf/fbo_sheet.py RUN_FSI RUN_FBO [--out PATH] [--width 1280]

Reads captures\\RUN_f_*.png from tools\\input_scripts\\short\\s22_fbo_shots.txt runs. Writes one PNG per shot pair
(full resolution, left | right) and a contact sheet (each pair scaled to --width) into
out\\harness\\step22\\fbo_sheet\\ (local only: game pictures are never committed). Also prints the mean absolute pixel
difference per pair (how different the two paths render that frame).
"""
import argparse
import glob
import os

from PIL import Image, ImageChops, ImageDraw, ImageStat

REPO = r'C:\Users\Jarrod\wwe2k14-recomp'
CAP = os.path.join(REPO, 'out', 'build', 'win-amd64-release', 'captures')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run_a')
    ap.add_argument('run_b')
    ap.add_argument('--labels', default='FSI,FBO')
    ap.add_argument('--out', default=os.path.join(REPO, 'out', 'harness', 'step22', 'fbo_sheet'))
    ap.add_argument('--width', type=int, default=1280, help='width of one pair in the contact sheet')
    a = ap.parse_args()
    la, lb = a.labels.split(',')
    os.makedirs(a.out, exist_ok=True)
    shots = sorted(os.path.basename(p)[len(a.run_a) + 1:] for p in glob.glob(os.path.join(CAP, f'{a.run_a}_f_*.png')))
    rows = []
    for shot in shots:
        pa = os.path.join(CAP, f'{a.run_a}_{shot}')
        pb = os.path.join(CAP, f'{a.run_b}_{shot}')
        if not os.path.exists(pb):
            print(f'{shot}: missing in run {a.run_b}')
            continue
        ia = Image.open(pa).convert('RGB')
        ib = Image.open(pb).convert('RGB')
        if ib.size != ia.size:
            ib = ib.resize(ia.size)
        mad = sum(ImageStat.Stat(ImageChops.difference(ia, ib)).mean) / 3
        w, h = ia.size
        pair = Image.new('RGB', (w * 2 + 8, h + 40), (20, 20, 20))
        pair.paste(ia, (0, 40))
        pair.paste(ib, (w + 8, 40))
        d = ImageDraw.Draw(pair)
        name = shot[2:-4]
        d.text((10, 10), f'{name}  {la} (run {a.run_a})', fill=(255, 255, 255))
        d.text((w + 18, 10), f'{lb} (run {a.run_b})   mean abs diff {mad:.2f}', fill=(255, 255, 255))
        pair.save(os.path.join(a.out, f'pair_{name}.png'))
        rows.append(pair.resize((a.width, int(pair.size[1] * a.width / pair.size[0]))))
        print(f'{name}: mean abs diff {mad:.2f}')
    if rows:
        sheet = Image.new('RGB', (a.width, sum(r.size[1] for r in rows)), (0, 0, 0))
        y = 0
        for r in rows:
            sheet.paste(r, (0, y))
            y += r.size[1]
        path = os.path.join(a.out, f'sheet_{a.run_a}_{a.run_b}.png')
        sheet.save(path)
        print('sheet:', path)


if __name__ == '__main__':
    main()
