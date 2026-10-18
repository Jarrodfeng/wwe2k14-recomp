"""Summarise fixed-seed arena-grid runs (Session 8 task 8a). Usage: py tools/determinism/arena_check.py NNN [NNN ...]

For each run (logs/wwe2k14_NNN.log + captures/NNN_t8a_grid.bmp from tools/input_scripts/determinism_arena.txt):
prints the [wwe2k14][seed] log lines, which arena tile is pre-selected on the grid and an RGB hash of the grid
screenshot (identical hash = identical frame). Also writes a half-size PNG of the grid to out/harness/t8a/ (game
image: stays under out/harness, never committed).

Selected tile: the pre-selected tile's frame is dark red instead of white. The 1-px left frame column of each tile
(coordinates measured on the 1280x720 capture of run 211) is sampled over its middle half; the tile whose frame has
the lowest green value is the selected one. Checked by eye on runs 208 (Over the Limit), 211 (NXT) and 218
(Elimination Chamber).

Grid check: on the arena grid every unselected tile frame is white (green 254 in all grid captures 208-225) and the
selected one is below 70. Another screen (title, main menu: no tile above 200, run 216) is reported as "not the
arena grid" with no tile name and no hash, so a missed screenshot cannot pass for a seed result.
"""
import hashlib
import os
import sys

from PIL import Image

EXE = r"C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release"
OUT = r"C:\Users\Jarrod\wwe2k14-recomp\out\harness\t8a"

# (top, bottom, [(left frame x, name), ...]) per row of the ARENAS page.
ROWS = [(125, 245, [(145, "RAW"), (345, "SMACKDOWN"), (545, "MAIN EVENT"), (745, "SUPERSTARS"), (945, "NXT")]),
        (255, 365, [(263, "SUMMERSLAM"), (453, "SURVIVOR SERIES"), (645, "ROYAL RUMBLE"), (837, "LOCKED")]),
        (375, 485, [(263, "OVER THE LIMIT"), (453, "NO WAY OUT"), (645, "MONEY IN THE BANK"),
                    (837, "NIGHT OF CHAMPIONS")]),
        (495, 605, [(165, "HELL IN A CELL"), (357, "TLC"), (549, "ELIMINATION CHAMBER"), (741, "EXTREME RULES"),
                    (933, "WRESTLEMANIA MOMENT")])]


def selected_tile(im):
    scores = []
    for y0, y1, tiles in ROWS:
        q = (y1 - y0) // 4
        for x, name in tiles:
            best = 0.0
            for dx in range(-3, 4):  # find the frame column near x (the brightest one)
                px = [im.getpixel((x + dx, y)) for y in range(y0 + q, y1 - q)]
                best = max(best, sum(p[1] for p in px) / len(px))
            scores.append((best, name))
    scores.sort()
    return scores


WHITE = 200     # unselected frames measure 254 on the grid
SELECTED = 150  # selected frames measured 1-68 (runs 208-225)


def looks_like_grid(scores):
    """True when every tile but the lowest has a white frame and the lowest is clearly darker."""
    white = sum(1 for g, _ in scores[1:] if g >= WHITE)
    return white == len(scores) - 1 and scores[0][0] < SELECTED


def main():
    os.makedirs(OUT, exist_ok=True)
    for n in sys.argv[1:]:
        log = os.path.join(EXE, "logs", "wwe2k14_%s.log" % n)
        print("== run %s" % n)
        if not os.path.exists(log):
            print("   no log %s: skipped" % log)
            continue
        lines = open(log, encoding="utf-8", errors="replace").read().splitlines()
        for l in lines:
            if "[wwe2k14][seed]" in l:
                print("   " + l.split("[wwe2k14][seed] ", 1)[1])
        for l in lines:
            if "script FAILED" in l or "][crash]" in l:
                print("   FAIL: " + l.split("] ", 4)[-1][:160])
        bmp = os.path.join(EXE, "captures", "%s_t8a_grid.png" % n)  # Session 17: PNG; older runs BMP
        if not os.path.exists(bmp):
            bmp = bmp[:-4] + '.bmp'
        if not os.path.exists(bmp):
            print("   no grid screenshot")
            continue
        im = Image.open(bmp).convert("RGB")
        if im.size != (1280, 720):
            print("   grid screenshot is %dx%d; tile coordinates are for 1280x720" % im.size)
            continue
        scores = selected_tile(im)
        if not looks_like_grid(scores):
            print("   not the arena grid: %d of %d tile frames white (>= %d), lowest %.0f; no tile, no hash" % (
                sum(1 for g, _ in scores if g >= WHITE), len(scores), WHITE, scores[0][0]))
            continue
        print("   selected tile: %s (frame green %.0f; next lowest %s %.0f)" % (scores[0][1], scores[0][0],
                                                                            scores[1][1], scores[1][0]))
        print("   grid hash: " + hashlib.sha1(im.tobytes()).hexdigest()[:12])
        im.resize((640, 360)).save(os.path.join(OUT, "%s_t8a_grid.png" % n))


if __name__ == "__main__":
    main()
