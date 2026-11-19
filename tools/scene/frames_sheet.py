"""A labelled contact sheet of presented pictures (Session 103, lane S): the live_<frame>.png pictures a run writes with
--wwe2k14_rdr_live_png_every (in mode 2: what the player sees, our frame + the game's 2D remnant).

  py -I tools\\scene\\frames_sheet.py FOLDER --out SHEET.png [--frames 4069,4815,...] [--cols 3] [--width 520] [--title TEXT]
"""
import argparse, os, re, sys
from PIL import Image, ImageDraw, ImageFont


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder"); ap.add_argument("--out", required=True)
    ap.add_argument("--frames", default=""); ap.add_argument("--cols", type=int, default=3)
    ap.add_argument("--width", type=int, default=520); ap.add_argument("--title", default="")
    a = ap.parse_args()
    found = {}
    for n in os.listdir(a.folder):
        m = re.fullmatch(r"live_(\d+)\.png", n)
        if m: found[int(m.group(1))] = os.path.join(a.folder, n)
    frames = [int(x) for x in a.frames.split(",") if x] if a.frames else sorted(found)
    frames = [f for f in frames if f in found]
    if not frames:
        print("frames_sheet: no live_<frame>.png pictures", file=sys.stderr); return 2
    font = ImageFont.load_default()
    tiles = []
    for f in frames:
        im = Image.open(found[f]).convert("RGB")
        h = int(im.size[1] * a.width / im.size[0])
        t = Image.new("RGB", (a.width, h + 16), (16, 16, 20))
        t.paste(im.resize((a.width, h)), (0, 16))
        ImageDraw.Draw(t).text((4, 2), f"frame {f}", fill=(255, 255, 255), font=font)
        tiles.append(t)
    rows = (len(tiles) + a.cols - 1) // a.cols
    th = tiles[0].size[1]
    sheet = Image.new("RGB", (a.width * a.cols, 22 + th * rows), (8, 8, 10))
    ImageDraw.Draw(sheet).text((6, 5), a.title, fill=(255, 255, 255), font=font)
    for i, t in enumerate(tiles):
        sheet.paste(t, ((i % a.cols) * a.width, 22 + (i // a.cols) * th))
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    sheet.save(a.out)
    print(f"wrote {a.out}: {len(tiles)} picture(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
