# Session 85: a contact sheet of the same shots from two runs (default vs a setting), plus an amplified difference.
#   py tools\native\s85_sheet.py OUT.jpg LABEL_A DIR_A PREFIX_A LABEL_B DIR_B PREFIX_B [shots=a,b,c]
# Shots are DIR\PREFIX_s82f_<x>.png (720p runs are 1280 wide; 1440p ones are shrunk to the same width).
import sys
from PIL import Image, ImageChops, ImageDraw

out, la, da, pa, lb, db, pb = sys.argv[1:8]
shots = (sys.argv[8] if len(sys.argv) > 8 else "a,b,c").split(",")
W = 640
rows = []
for s in shots:
    ims = []
    for d, p in ((da, pa), (db, pb)):
        im = Image.open(f"{d}\\{p}_s82f_{s}.png").convert("RGB")
        ims.append(im.resize((W, int(im.height * W / im.width)), Image.LANCZOS))
    diff = ImageChops.difference(ims[0], ims[1]).point(lambda v: min(255, v * 6))
    rows.append((ims[0], ims[1], diff))
H = rows[0][0].height
sheet = Image.new("RGB", (W * 3, (H + 18) * len(rows)), (20, 20, 20))
dr = ImageDraw.Draw(sheet)
for i, row in enumerate(rows):
    y = i * (H + 18)
    for j, (im, t) in enumerate(zip(row, (la, lb, "difference x6"))):
        sheet.paste(im, (j * W, y + 18))
        dr.text((j * W + 6, y + 3), f"{t} ({shots[i]})", fill=(255, 255, 255))
sheet.save(out, quality=88)
print(out)
