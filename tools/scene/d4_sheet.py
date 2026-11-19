#!/usr/bin/env python3
"""Lane D4 sheet (after d4_check.py): one picture to look at. Panels (frame 110, 1280x720 each, shown at half size): ours = the props alone from the record rows / the independent oracle's
silhouette / the planted props_transposed twin / the single person (the row alone, in place) over its no-row picture. Written to out\\s103\\D4\\d4_sheet.png."""
import os, sys
from PIL import Image, ImageDraw
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import frame_rig as fr  # noqa: E402
import d4_check as d4  # noqa: E402

main_root = fr.main_checkout()
work = os.path.join(main_root, 'out', 's103', 'D4', 'd4_work')
panels = [('ours: signs / foam hands from the record rows (props alone)', os.path.join(work, 'props_110_mask16.png')),
          ('planted twin: props_transposed', os.path.join(work, 'plant_props_transposed_mask16.png')),
          ('planted twin: props_wrong_key', os.path.join(work, 'plant_props_wrong_key_mask16.png')),
          ('single person row alone (a moved clone of a stands person)', os.path.join(work, 'single_on_mask16.png'))]
ims = [Image.open(p).convert('RGB').resize((640, 360)) for _, p in panels]
sheet = Image.new('RGB', (1280, 2 * 380 + 20), (20, 20, 24))
d = ImageDraw.Draw(sheet)
for i, ((label, _), im) in enumerate(zip(panels, ims)):
    x, y = (i % 2) * 640, (i // 2) * 380
    sheet.paste(im, (x, y + 20))
    d.text((x + 6, y + 4), label, fill=(230, 230, 230))
out = os.path.join(main_root, 'out', 's103', 'D4', 'd4_sheet.png')
sheet.save(out)
print(out)
