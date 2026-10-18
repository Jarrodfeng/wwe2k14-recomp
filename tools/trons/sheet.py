"""Contact sheet of a run's screenshots (Session 17, tron checks).

py tools/trons/sheet.py RUN LABEL [COLS] [WIDTH] -> out/trons/sheet_RUN_LABEL.png (local only)
"""
import glob
import os
import sys

from PIL import Image, ImageDraw

run, label = sys.argv[1], sys.argv[2]
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 6
w = int(sys.argv[4]) if len(sys.argv) > 4 else 320
root = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
files = sorted(f for ext in ('png', 'bmp')
               for f in glob.glob(os.path.join(root, 'out/build/win-amd64-release/captures', f'{run}_{label}_*.{ext}')))
if not files:
    sys.exit('no screenshots')
ims = []
for f in files:
    im = Image.open(f).convert('RGB')
    im = im.resize((w, im.height * w // im.width))
    ImageDraw.Draw(im).text((4, 4), os.path.splitext(os.path.basename(f))[0], fill=(255, 255, 0))
    ims.append(im)
rows = (len(ims) + cols - 1) // cols
sheet = Image.new('RGB', (cols * w, rows * ims[0].height))
for i, im in enumerate(ims):
    sheet.paste(im, ((i % cols) * w, (i // cols) * im.height))
os.makedirs(os.path.join(root, 'out/trons'), exist_ok=True)
out = os.path.join(root, 'out/trons', f'sheet_{run}_{label}.png')
sheet.save(out)
print(out, len(ims))
