"""Session 67: the launcher's Xbox 360 controller pictures for KEYBOARD CONTROLS (embedded in wwe2k14_launcher.exe).

Source: assets/launcher/360_controller.svg, "360 controller.svg" by Alphathon (Wikimedia Commons, own work, 2010-03-05;
CC BY-SA 3.0 or GFDL 1.2+: https://commons.wikimedia.org/wiki/File:360_controller.svg), added by Jarrod 2026-10-26
("remove the overlay text and use that instead"). Changes made here: the labels (all languages) and their leader
lines removed, the view cropped to the pad. The pictures made from it stay under CC BY-SA 3.0 (credited in the
launcher's About page and THIRD_PARTY_NOTICES.md).

Writes:
  assets/launcher/pad_360.png        the pad, transparent background, 2x (2340 x 1450 = the view box below, doubled)
  assets/launcher/pad_360_parts.png  1x: each control filled with its own colour (PART_COLOUR), the rest transparent;
                                     the launcher lights a control by tinting its pixels (launcher_main.cpp DrawPad)
Both cover the same view box, so the launcher maps SVG coordinates onto them directly (kPadBox there).

Renders with headless Microsoft Edge (part of Windows 11). usage: py tools/launcher/make_pad_png.py
"""
import os
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET

from PIL import Image

SRC = "assets/launcher/360_controller.svg"
OUT = "assets/launcher/pad_360.png"
OUT_PARTS = "assets/launcher/pad_360_parts.png"
VIEW = (415, 100, 1170, 725)  # x, y, w, h in SVG units (launcher_main.cpp kPadBox)
SCALE = 2

SVG_NS = "http://www.w3.org/2000/svg"
ET.register_namespace("", SVG_NS)
for p, u in {"dc": "http://purl.org/dc/elements/1.1/", "cc": "http://creativecommons.org/ns#",
             "rdf": "http://www.w3.org/1999/02/22-rdf-syntax-ns#", "svg": SVG_NS}.items():
    ET.register_namespace(p, u)

# The labels' leader lines (thin rects and paths; checked against a render, run in Session 67) and two empty groups.
LEADERS = {"Shape_17_1_", "Shape_17_copy_1_", "Shape_17_copy_2_1_", "Shape_17_copy_3_1_", "Shape_17_copy_4_1_",
           "Shape_18_1_", "Shape_19_1_", "Shape_19_copy_1_", "Shape_20_1_", "Shape_20_copy_1_", "Shape_20_copy_2_1_",
           "Shape_22_1_", "Shape_23_1_", "Shape_24_1_", "Shape_25_1_", "Shape_26_1_", "Shape_27_1_", "Right_Trigger",
           "g4810"}

# The controls, in the launcher's part order (launcher_main.cpp PartOf): their top-level groups in the SVG.
PARTS = [
    ["Shape_11_copy_1_", "A"],                                                           # 0 A
    ["Shape_11_copy_2_1_", "B"],                                                         # 1 B
    ["Shape_11_copy_3_1_", "X"],                                                         # 2 X
    ["Shape_11_1_", "Y"],                                                                # 3 Y
    ["Shape_2_copy_3_1_", "Shape_2_copy_1_", "g336", "LT"],                              # 4 LT
    ["Shape_2_copy_4_1_", "Shape_2_copy_5_1_", "g4804", "RT"],                           # 5 RT
    ["Shape_4_1_", "Shape_4_copy_1_", "LB"],                                             # 6 LB
    ["Shape_4_copy_2_1_", "Shape_4_copy_3_1_", "RB"],                                    # 7 RB
    ["Shape_12_copy_9_1_", "Shape_12_copy_10_1_", "Shape_12_copy_11_1_", "Shape_12_copy_12_1_",
     "Shape_12_copy_13_1_", "Shape_13_copy_4_1_", "Shape_13_copy_8_1_", "Shape_13_copy_9_1_", "Shape_13_copy_10_1_",
     "Shape_13_copy_11_1_"],                                                             # 8 left stick
    ["Shape_12_1_", "Shape_12_copy_6_1_", "Shape_12_copy_1_", "Shape_12_copy_7_1_", "Shape_12_copy_8_1_",
     "Shape_13_1_", "Shape_13_copy_1_", "Shape_13_copy_2_1_", "Shape_13_copy_3_1_"],     # 9 right stick
    ["Shape_16_1_", "Shape_16_copy_1_", "Shape_12_copy_4_1_", "Shape_12_copy_5_1_", "Shape_15_1_",
     "Shape_15_copy_1_", "Shape_14_1_", "Shape_14_copy_1_"],                             # 10 D-pad
    ["Shape_5_1_", "Shape_5_copy_1_"],                                                   # 11 BACK
    ["Shape_5_copy_2_1_", "Shape_5_copy_3_1_"],                                          # 12 START
    ["Shape_6_copy_2_1_", "Shape_6_1_", "Shape_6_copy_1_", "Shape_28_1_", "Shape_28_copy_1_", "Shape_29_1_",
     "Shape_29_copy_1_"],                                                                # 13 GUIDE
]


def part_colour(i):
    """Part i's colour in the parts picture (the launcher decodes it the same way: R = 10 + 17 i, G 200, B 100)."""
    return (10 + 17 * i, 200, 100)


def local(tag):
    return tag.split("}")[-1]


def clean_tree():
    tree = ET.parse(SRC)
    root = tree.getroot()
    for e in list(root):
        if local(e.tag) == "switch" or e.get("id") in LEADERS:
            root.remove(e)
    x, y, w, h = VIEW
    root.set("viewBox", f"{x} {y} {w} {h}")
    root.set("width", str(w * SCALE))
    root.set("height", str(h * SCALE))
    return tree


def parts_tree():
    tree = clean_tree()
    root = tree.getroot()
    owner = {gid: i for i, ids in enumerate(PARTS) for gid in ids}
    missing = {gid for ids in PARTS for gid in ids} - {e.get("id") for e in root}
    if missing:
        sys.exit(f"groups not found in {SRC}: {sorted(missing)}")
    root.set("shape-rendering", "crispEdges")  # flat colours, no blended edges
    root.set("width", str(VIEW[2]))
    root.set("height", str(VIEW[3]))
    for e in list(root):
        if local(e.tag) in ("metadata", "defs"):
            continue
        i = owner.get(e.get("id"))
        if i is None:
            root.remove(e)
            continue
        r, g, b = part_colour(i)
        c = f"#{r:02x}{g:02x}{b:02x}"
        for k in e.iter():
            if local(k.tag) in ("path", "rect", "ellipse", "circle", "polygon", "line", "polyline"):
                k.set("style", f"fill:{c};stroke:{c};stroke-width:3;opacity:1;fill-opacity:1;stroke-opacity:1")
    return tree


def find_edge():
    for base in (os.environ.get("ProgramFiles(x86)", ""), os.environ.get("ProgramFiles", "")):
        p = os.path.join(base, "Microsoft", "Edge", "Application", "msedge.exe")
        if os.path.isfile(p):
            return p
    sys.exit("Microsoft Edge not found (it renders the SVG)")


def render(tree, w, h, out, tmp):
    svg = os.path.join(tmp, os.path.basename(out) + ".svg")
    tree.write(svg, encoding="utf-8", xml_declaration=True)
    html = os.path.join(tmp, os.path.basename(out) + ".html")
    with open(html, "w", encoding="utf-8") as f:
        f.write(f'<html><body style="margin:0;background:transparent">'
                f'<img src="{os.path.basename(svg)}" width="{w}" height="{h}" style="display:block"></body></html>')
    shot = os.path.join(tmp, os.path.basename(out) + ".shot.png")
    subprocess.run([find_edge(), "--headless=new", "--disable-gpu", "--hide-scrollbars",
                    f"--user-data-dir={os.path.join(tmp, 'edge')}", "--default-background-color=00000000",
                    f"--window-size={w + 200},{h + 300}", f"--screenshot={shot}", "file:///" + html.replace("\\", "/")],
                   check=True, capture_output=True)
    # (msedge.exe can return before its child process has written the file: wait for it, then for its size to settle)
    last = -1
    for _ in range(120):
        size = os.path.getsize(shot) if os.path.isfile(shot) else -1
        if size > 0 and size == last:
            break
        last = size
        time.sleep(0.25)
    else:
        sys.exit(f"Edge wrote no screenshot ({shot})")
    im = Image.open(shot).convert("RGBA")
    if im.width < w or im.height < h:
        sys.exit(f"render {im.width}x{im.height} smaller than {w}x{h}")
    return im.crop((0, 0, w, h))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        pad = render(clean_tree(), VIEW[2] * SCALE, VIEW[3] * SCALE, OUT, tmp)
        pad.save(OUT, optimize=True)
        parts = render(parts_tree(), VIEW[2], VIEW[3], OUT_PARTS, tmp)
        # Keep only exact part colours (anything else, e.g. a stray blended pixel, becomes transparent).
        colours = {part_colour(i) for i in range(len(PARTS))}
        px = parts.load()
        counts = {}
        for yy in range(parts.height):
            for xx in range(parts.width):
                r, g, b, a = px[xx, yy]
                if a == 255 and (r, g, b) in colours:
                    counts[r] = counts.get(r, 0) + 1
                else:
                    px[xx, yy] = (0, 0, 0, 0)
        parts.save(OUT_PARTS, optimize=True)
    print(f"{OUT}: {pad.width}x{pad.height}; {OUT_PARTS}: {parts.width}x{parts.height}")
    for i in range(len(PARTS)):
        print(f"  part {i:2d}: {counts.get(part_colour(i)[0], 0)} px")


if __name__ == "__main__":
    main()
