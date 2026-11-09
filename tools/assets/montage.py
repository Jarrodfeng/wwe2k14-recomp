"""Joins pictures into a grid: py -I tools/assets/montage.py OUT.png COLS IN1.png IN2.png ... (each scaled to 960 wide)."""
import sys
from PIL import Image
out, cols, ins = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
ims = [Image.open(p).convert('RGB') for p in ins]
w = 960
ims = [i.resize((w, int(i.height * w / i.width))) for i in ims]
h = max(i.height for i in ims)
rows = (len(ims) + cols - 1) // cols
m = Image.new('RGB', (w * cols, h * rows))
for k, i in enumerate(ims):
    m.paste(i, ((k % cols) * w, (k // cols) * h))
m.save(out)
