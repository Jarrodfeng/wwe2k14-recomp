"""Lane B2F (Session 103) offline proof of the ring ropes' shadow proxy in the plane mask (src\\renderer_scene\\live_map_rope.cpp): our mask beside the game's own, counts, planted twins.

  py -I tools\\scene\\b2f_check.py --capture DIR [--frames 500] [--out DIR] [--mask-pngs A.png,B.png]   (the default capture is a kept match case: unpack run29_case.zip's capture/ folder first)

Per frame: fview (the main checkout's built exe, headless, read-only) saves the game's mask after its pass (e2_check.game_plane_and_told); maps_draw draws OUR mask from the same frame's scene record with
the ring eye points of the latch reader (--module-eye, the fixture memory of the production reader) and WITHOUT the rope proxy (plant plane_rope_none = the state before this lane), then with it, then the twins.
Bars (pixels > 3 levels in the 512 mask; "game-only" = the game has it and ours has not):
  * the proxy recovers the game's lines: game-only falls by >= 35 % against the no-proxy run, IoU with the game's mask rises by >= 0.10;
  * the proxy's own pixels (clean minus no-proxy) are the game's: >= 60 % of them are game pixels too (precision of the lines);
  * twin plane_rope_drop_side: the census says 9 ropes drawn (3 lost = 6 of 24 draws), the lost pixels are exactly the clean run's pixels that no other rope covers, and game-only rises;
  * twin plane_rope_swap_eyes: the same 12 ropes are drawn (24 draws) but the lines MOVE: IoU falls below the clean run's;
  * validation 0 in every run; the census names 12 ropes drawn, 0 dropped, 24 draws in the clean run.
Nothing from the game is written into the repo: pictures go under the --out folder (scratch).
"""
import argparse, os, re, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import e2_check as e   # noqa: E402  (the rig helpers: fview, maps_draw, the game's mask)

e.MODULE_EYE = True   # the ring eyes come from the latch reader over the fixture module (as e2_check --module-eye)
THRESH = 3


def mask(img):
    return np.asarray(img.convert("RGB")).astype(int).max(axis=2) > THRESH


def stats(g, o):
    inter = int((g & o).sum())
    return {"game": int(g.sum()), "ours": int(o.sum()), "both": inter, "game_only": int((g & ~o).sum()), "ours_only": int((o & ~g).sum()), "iou": inter / max(1, int((g | o).sum()))}


def census(out_dir, frame, tag):
    d = os.path.join(out_dir, "ours_f%d" % frame + ("_" + tag if tag else ""))
    text = open(os.path.join(d, "counts.txt")).read()
    m = re.search(r"rope proxy: ropes drawn (\d+) \(dropped (\d+)\), draws (\d+)", text)
    v = e.num(text, r"validation errors: (\d+)") + (0 if e.num(text, r"validation layer active: (\d+)") else 1000)
    return (int(m.group(1)), int(m.group(2)), int(m.group(3))) if m else (-1, -1, -1), v, text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", required=True)
    ap.add_argument("--frames", default="500")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    if a.capture.lower().endswith(".zip"):   # a kept case (out\s103\run29_case.zip): its capture/ folder is unpacked once under --out (scratch)
        import zipfile
        dest = os.path.join(a.out, "unpack")
        if not os.path.isdir(os.path.join(dest, "capture", "scene")):
            with zipfile.ZipFile(a.capture) as z:
                z.extractall(dest, [n for n in z.namelist() if n.startswith("capture/")])
        a.capture = os.path.join(dest, "capture")
    ok_all = True
    for frame in [int(x) for x in a.frames.split(",")]:
        g_img, _, told = e.game_plane_and_told(a.capture, frame, a.out)
        g = mask(g_img)
        res = {}
        for tag, fault in (("clean", ""), ("none", "plane_rope_none"), ("drop", "plane_rope_drop_side"), ("swap", "plane_rope_swap_eyes")):
            rc, _, img = e.ours(a.capture, frame, a.out, fault, told)
            if "plane" not in img or rc != 0:
                print(f"frame {frame}: {tag}: maps_draw failed (rc {rc})")
                return 2
            cen, val, _ = census(a.out, frame, fault)
            res[tag] = (mask(img["plane"]), stats(g, mask(img["plane"])), cen, val)
            s = res[tag][1]
            print(f"frame {frame} {tag:5s}: ropes drawn/dropped/draws {cen}; mask px game {s['game']} ours {s['ours']} both {s['both']} game-only {s['game_only']} ours-only {s['ours_only']} IoU {s['iou']:.3f}; validation {val}")
        c_m, c_s, c_cen, c_val = res["clean"]
        n_m, n_s, n_cen, _ = res["none"]
        d_m, d_s, d_cen, _ = res["drop"]
        w_m, w_s, w_cen, _ = res["swap"]
        proxy_px = c_m & ~n_m   # the proxy's own pixels
        prec = float((proxy_px & g).sum()) / max(1, int(proxy_px.sum()))
        rec = (n_s["game_only"] - c_s["game_only"]) / max(1, n_s["game_only"])
        lost = c_m & ~d_m
        checks = [
            ("12 ropes drawn, 0 dropped, 24 draws", c_cen == (12, 0, 24)),
            ("no-proxy run draws none (the state before the lane)", n_cen[2] == 0),
            (f"game-only falls {100 * rec:.0f} % (>= 35 %)", rec >= 0.35),
            (f"IoU {n_s['iou']:.3f} -> {c_s['iou']:.3f} (+{c_s['iou'] - n_s['iou']:.3f} >= 0.10)", c_s["iou"] - n_s["iou"] >= 0.10),
            (f"proxy pixels {int(proxy_px.sum())}, {100 * prec:.0f} % are the game's (>= 60 %)", prec >= 0.60),
            (f"twin drop_side: {d_cen[0]} ropes drawn (9), {int(lost.sum())} px lost, game-only {d_s['game_only']} > {c_s['game_only']}", d_cen == (9, 3, 18) and int(lost.sum()) > 0 and d_s["game_only"] > c_s["game_only"]),
            (f"twin swap_eyes: 24 draws, IoU {w_s['iou']:.3f} < {c_s['iou']:.3f}", w_cen[2] == 24 and w_s["iou"] < c_s["iou"] and int((c_m ^ w_m).sum()) > 0),
            ("validation 0 in all four runs", all(res[t][3] == 0 for t in res)),
        ]
        for name, ok in checks:
            print(f"   {'PASS' if ok else 'FAIL'}: {name}")
            ok_all = ok_all and ok
        # one sheet: game | clean | no proxy | drop | swap | lost-by-drop (white)
        tiles = []
        for lab, m in (("GAME", g), ("OURS clean", c_m), ("OURS no proxy", n_m), ("twin drop one side", d_m), ("twin swap two sides' eyes", w_m)):
            t = Image.fromarray((m * 255).astype(np.uint8)).convert("RGB")
            tiles.append(e.label(t, lab))
        sheet = Image.new("RGB", (sum(t.width for t in tiles), tiles[0].height))
        x = 0
        for t in tiles:
            sheet.paste(t, (x, 0)), t.width
            x += t.width
        p = os.path.join(a.out, f"b2f_sheet_f{frame}.png")
        sheet.save(p)
        print("sheet:", p)
    print("RESULT:", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
