"""Mirror-mode sheet (Session 103, lane S; docs\\renderer\\FRAME_CONTRACT.md 2): pairs the two pictures a --wwe2k14_rdr_scene=1 run writes
per sheet frame (live_scene_game_f<N>.png = the game's own picture, live_scene_ours_f<N>.png = the same frame's scene record drawn ALONE by
our record -> frame path) into one labelled sheet: game | ours | difference heat, one row per frame, plus the [scene][mirror] log line of that
frame when a log is given. Both sides carry the game's display curve (the drawer applies the same table).

  py -I tools\\scene\\mirror_sheet.py PICTURE_FOLDER --out SHEET.png [--log RUN.log] [--width 480] [--max 8]

Exit 0 = a sheet written; 2 = no pairs found (the run had no sheet frame, or mirror mode was off: check the log for [scene][mirror]).
"""
import argparse
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont
import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--out", required=True)
    ap.add_argument("--log", default="")
    ap.add_argument("--width", type=int, default=480)
    ap.add_argument("--max", type=int, default=8)
    ap.add_argument("--title", default="")
    a = ap.parse_args()

    pairs = []
    if not os.path.isdir(a.folder):
        print(f"mirror_sheet: no folder {a.folder}", file=sys.stderr)
        return 2
    for name in os.listdir(a.folder):
        m = re.fullmatch(r"live_scene_game_f(\d+)\.png", name)
        if m and os.path.exists(os.path.join(a.folder, f"live_scene_ours_f{m.group(1)}.png")):
            pairs.append(int(m.group(1)))
    pairs.sort()
    if not pairs:
        print(f"mirror_sheet: no live_scene_game_f*/live_scene_ours_f* pairs in {a.folder}", file=sys.stderr)
        return 2
    if len(pairs) > a.max:  # spread over the run
        step = len(pairs) / a.max
        pairs = [pairs[int(i * step)] for i in range(a.max)]

    lines = {}
    if a.log and os.path.exists(a.log):
        for line in open(a.log, encoding="utf-8", errors="replace"):
            m = re.search(r"\[scene\]\[mirror\] live_scene_ours_f(\d+)\.png:(.*)", line)
            if m:
                lines[int(m.group(1))] = m.group(2).strip()

    w = a.width
    font = ImageFont.load_default()
    rows = []
    for f in pairs:
        g = Image.open(os.path.join(a.folder, f"live_scene_game_f{f}.png")).convert("RGB")
        o = Image.open(os.path.join(a.folder, f"live_scene_ours_f{f}.png")).convert("RGB")
        if o.size != g.size:
            o = o.resize(g.size)
        h = int(g.size[1] * w / g.size[0])
        gs, os_ = g.resize((w, h)), o.resize((w, h))
        d = np.abs(np.asarray(gs, dtype=np.int16) - np.asarray(os_, dtype=np.int16)).max(axis=2)
        heat = np.zeros((h, w, 3), dtype=np.uint8)
        heat[..., 0] = np.clip(d * 3, 0, 255)
        heat[..., 1] = np.clip(255 - d * 3, 0, 255) // 4
        hm = Image.fromarray(heat)
        mean = float(d.mean())
        row = Image.new("RGB", (w * 3, h + 30), (20, 20, 24))
        row.paste(gs, (0, 30)), row.paste(os_, (w, 30)), row.paste(hm, (2 * w, 30))
        dr = ImageDraw.Draw(row)
        dr.text((4, 2), f"frame {f}  GAME (its own renderer)", fill=(255, 255, 255), font=font)
        dr.text((w + 4, 2), "OURS (scene record alone, our renderer)", fill=(255, 255, 255), font=font)
        dr.text((2 * w + 4, 2), f"|game - ours| max channel, mean {mean:.1f}", fill=(255, 255, 255), font=font)
        if f in lines:
            dr.text((4, 15), lines[f][:200], fill=(200, 200, 120), font=font)
        rows.append(row)
    title_h = 24
    sheet = Image.new("RGB", (w * 3, title_h + sum(r.size[1] for r in rows)), (10, 10, 12))
    ImageDraw.Draw(sheet).text((6, 6), a.title or f"S103 mirror sheet: {a.folder}", fill=(255, 255, 255), font=font)
    y = title_h
    for r in rows:
        sheet.paste(r, (0, y))
        y += r.size[1]
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    sheet.save(a.out)
    print(f"wrote {a.out}: {len(rows)} frame(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
