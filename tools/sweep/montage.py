"""Session 10 (M5 sweep): one contact-sheet image per screenshot label + a brightness check.

  py tools/sweep/montage.py LIST LABEL COLS     e.g. arenas a_match600 7 / roster r_tron800 10

Reads out/harness/sweep/results/LIST.json (tools/sweep/results.py), tiles the LABEL thumbnails (240x135, run name
in the corner) into out/harness/review/session10/LIST/montage_LABEL.jpg (local only: game images) and prints the
darkest shots and those with mean luminance < 25 or more than half the pixels near-black (a black-screen hint;
entrance shots are dark by design).
"""
import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageStat

REPO = Path(__file__).resolve().parents[2]
lst = sys.argv[1]; label = sys.argv[2]; cols = int(sys.argv[3])
rows = json.load(open(REPO / f"out/harness/sweep/results/{lst}.json", encoding="utf-8"))
review = REPO / f"out/harness/review/session10/{lst}"
tiles, stats = [], []
for r in rows:
    imgs = {i["label"]: i for i in (r.get("images") or [])}
    i = imgs.get(label)
    name = r["name"]
    if not i or not i.get("thumb"):
        stats.append((name, None)); continue
    im = Image.open(review / i["thumb"]).convert("RGB")
    st = ImageStat.Stat(im.convert("L"))
    dark = sum(im.convert("L").histogram()[:16]) / (im.width * im.height)
    stats.append((name, round(st.mean[0], 1), round(dark, 3)))
    d = ImageDraw.Draw(im); d.rectangle([0, 0, 120, 14], fill=(0, 0, 0)); d.text((2, 1), name, fill=(255, 255, 0))
    tiles.append(im.resize((240, 135)))
W = 240 * cols; H = 135 * ((len(tiles) + cols - 1) // cols)
sheet = Image.new("RGB", (W, H))
for k, t in enumerate(tiles):
    sheet.paste(t, ((k % cols) * 240, (k // cols) * 135))
out = REPO / f"out/harness/review/session10/{lst}/montage_{label}.jpg"
sheet.save(out, quality=80)
print(out)
low = [s for s in stats if s[1] is None or s[1] < 25 or s[2] > 0.5]
print("darkest:", sorted([s for s in stats if s[1] is not None], key=lambda s: s[1])[:6])
print("flag (mean<25 or >50% near-black or missing):", low)
