"""Session 79 (HD 520 arc): side-by-side review pairs for Jarrod's eye (look levers, off vs on, same replay frame).
Read-only on the captures; writes PNGs under OUT (out\\ is gitignored - never commit game pictures).
    py tools/perf/review_pairs.py CAPTURES RUN_OFF RUN_ON OUT LABEL
Each pair: left = RUN_OFF, right = RUN_ON, with a label strip, plus a third panel marking the pixels that differ
(white where any channel differs by more than 16 levels)."""
import os
import sys

from PIL import Image, ImageChops, ImageDraw


def main():
    cap, off, on, out, label = sys.argv[1:6]
    os.makedirs(out, exist_ok=True)
    names = sorted(f[len(off) + 1:] for f in os.listdir(cap) if f.startswith(off + "_") and f.endswith(".png"))
    for name in names:
        b = os.path.join(cap, f"{on}_{name}")
        if not os.path.exists(b):
            continue
        ia = Image.open(os.path.join(cap, f"{off}_{name}")).convert("RGB")
        ib = Image.open(b).convert("RGB")
        diff = ImageChops.difference(ia, ib).convert("L").point(lambda v: 255 if v > 16 else 0)
        w, h = ia.size
        sheet = Image.new("RGB", (w * 3, h + 28), (0, 0, 0))
        sheet.paste(ia, (0, 28))
        sheet.paste(ib, (w, 28))
        sheet.paste(diff.convert("RGB"), (2 * w, 28))
        d = ImageDraw.Draw(sheet)
        d.text((8, 6), f"OFF (run {off})", fill=(255, 255, 255))
        d.text((w + 8, 6), f"ON: {label} (run {on})", fill=(255, 255, 255))
        d.text((2 * w + 8, 6), "differs by > 16 levels (white)", fill=(255, 255, 255))
        dest = os.path.join(out, f"{label}_{os.path.splitext(name)[0]}.png")
        sheet.save(dest)
        print(dest)


if __name__ == "__main__":
    main()
