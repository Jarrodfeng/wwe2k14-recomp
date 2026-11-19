"""Lane E (Session 103) offline check of the Maps stage hook against a capture: our maps beside the game's own, on one labelled sheet, with counts.

  py -I tools\\scene\\e_check.py [--capture DIR] [--frames 20,100,180] [--out DIR] [--faults]

Per frame: fview (the main checkout's built exe, headless, read-only) saves the game's wrestler map B and the props map (the circle blobs are in its blue
channel) after their passes; maps_draw.exe draws OUR maps from the same frame's scene record; the sheet is game | ours | difference for each map. The bars:
 - shadow: the casters' footprint (pixels that are not the clear) overlaps the game's map B by IoU >= SHADOW_IOU, mean |d| inside the common footprint small;
 - circles: the blobs' footprint (blue < 250) overlaps the game's props map by IoU >= CIRCLE_IOU;
 - validation errors 0; casters > 0 and circles drawn > 0.
Planted faults (--faults): each must FAIL its bar: transpose_light (shadow IoU), no_casters (casters 0), circles_wrong_frame (circles drawn 0).
Nothing from the game is written into the repo: pictures go under out\\s103\\E (scratch, gitignored).
"""
import argparse, json, os, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw

MAIN = r"C:\Users\Jarrod\wwe2k14-recomp"
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
FVIEW = os.path.join(MAIN, r"out\renderer\dev\fview.exe")
SHADOW_IOU, CIRCLE_IOU, MEAN_D = 0.90, 0.88, 2.0
# who stands in which record slot: the live record names no model file (model_key only: a known gap, lane A / B). Told to the rig.
WRESTLERS = ["--wrestler", "0=343", "--wrestler", "6=324"]


def run(cmd, env=None, timeout=300):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
    return p.returncode, p.stdout + p.stderr


def game_maps(capture, frame, out):
    """The game's map B and props map of `frame`: saved by fview as picture.png after the passes."""
    env = dict(os.environ, CLAUDECODE="1")
    js = os.path.join(out, f"ops{frame}.json")
    rc, text = run([FVIEW, "--capture", capture, "--frame", str(frame), "--ops", "--json", js, "--root", ROOT, "--game", os.path.join(MAIN, r"game_data\extracted")], env)
    if rc != 0 or not os.path.exists(js):
        raise SystemExit("fview --ops failed: " + text[-400:])
    ops = json.load(open(js))["ops"]
    passes, cur = [], None
    for o in ops:
        if o["kind"] == "targets":
            cur = {"op": o["op"], "surface": o["colors"][0] if o["colors"] else 0, "tech": {}, "last": o["op"]}
            passes.append(cur)
        elif cur is not None:
            cur["last"] = o["op"]
            if o["kind"] == "draw":
                cur["tech"][o.get("technique", "")] = cur["tech"].get(o.get("technique", ""), 0) + 1
    props = next((p for p in passes if "T_CircleShadow" in p["tech"]), None)
    maps = [p for p in passes if "T_ShadowMap" in p["tech"] and "T_CircleShadow" not in p["tech"]]
    if not props or not maps:
        raise SystemExit(f"frame {frame}: no shadow passes found in the stream")
    mapb = maps[-1]   # map A comes first (the collapse-A casters), map B last (all casters)
    got = {}
    for name, p in (("mapB", mapb), ("props", props)):
        # the surface as the pass left it: the pass's last op (a copy follows it)
        rc, text = run([FVIEW, "--capture", capture, "--frame", str(frame), "--surface", str(p["surface"]), "--through-op", str(p["last"]), "--save-case", "--op", str(p["last"]), "--root", ROOT, "--game", os.path.join(MAIN, r"game_data\extracted")], env)
        line = [l for l in text.splitlines() if "case saved:" in l]
        if not line:
            raise SystemExit("fview --save-case failed: " + text[-300:])
        got[name] = Image.open(os.path.join(line[-1].split("case saved:")[1].strip(), "picture.png")).convert("RGB")
    return got, mapb["tech"].get("T_ShadowMap", 0), props["tech"].get("T_CircleShadow", 0)


BONES_BACK = 1   # the game's map at stream frame F is drawn with the poses of record F-1 (SF1 run 7147)


LATCH = "--latch" in sys.argv   # S103 E4


def ours(capture, frame, out, faults="", other=None):
    d = os.path.join(out, f"ours_f{frame}" + (f"_{faults}" if faults else ""))
    cmd = [os.path.join(ROOT, r"out\tools\maps_draw.exe"), capture, "--frame", str(frame), "--game", os.path.join(MAIN, r"game_data\extracted"), "--out", d] + WRESTLERS
    if BONES_BACK:
        cmd += ["--bones-frame", str(frame - BONES_BACK)]
    if faults:
        cmd += ["--fault", faults]
    if other is not None:
        cmd += ["--other-frame", str(other)]
    if LATCH:
        cmd += ["--circles-as-latch"]
    rc, text = run(cmd, dict(os.environ, WWE_RDR_VALIDATION="1"))   # S103 E2 review: the validation layer ON, so "validation 0" means something
    counts = open(os.path.join(d, "counts.txt")).read() if os.path.exists(os.path.join(d, "counts.txt")) else text
    sh = Image.open(os.path.join(d, "ours_shadow.png")).convert("RGB") if os.path.exists(os.path.join(d, "ours_shadow.png")) else None
    ci = Image.open(os.path.join(d, "ours_circles.png")).convert("RGB") if os.path.exists(os.path.join(d, "ours_circles.png")) else None
    return rc, counts, sh, ci


def num(counts, key):
    import re
    m = re.search(key, counts)
    return int(m.group(1)) if m else 0


def footprint_shadow(img):
    a = np.asarray(img).astype(int)
    return (a[..., 0] < 250) | (a[..., 1] < 250)


def footprint_circles(img):
    a = np.asarray(img).astype(int)
    return a[..., 2] < 250   # the blobs darken blue only


def iou(a, b):
    u = (a | b).sum()
    return float((a & b).sum()) / u if u else 1.0


def label(img, text):
    im = img.copy()
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, im.width, 14], fill=(0, 0, 0))
    d.text((3, 2), text, fill=(255, 255, 255))
    return im


def mean_diff(game, ours_img):
    """Mean |difference| (0..255) over the pixels both maps cover; 255 when they share none."""
    if ours_img is None:
        return 255.0
    both = footprint_shadow(game) & footprint_shadow(ours_img)
    if not both.any():
        return 255.0
    return float(np.abs(np.asarray(game).astype(int) - np.asarray(ours_img).astype(int))[both].mean())


def diff_heat(a, b):
    x = np.abs(np.asarray(a).astype(int) - np.asarray(b).astype(int)).max(axis=2)
    h = np.clip(x * 3, 0, 255).astype(np.uint8)
    return Image.fromarray(np.stack([h, h // 3, 255 - h], axis=2).astype(np.uint8))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", default=os.path.join(MAIN, r"out\d3caster\z7147\capture"))
    ap.add_argument("--frames", default="20,100,180")
    ap.add_argument("--out", default=os.path.join(MAIN, r"out\s103\E"))
    ap.add_argument("--faults", action="store_true")
    ap.add_argument("--latch", action="store_true", help="S103 E4: the circle rows are flagged as A3's latch rows (kCircleFromLatch): the same pictures; plant circles_no_mesh must become a counted hole")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    frames = [int(x) for x in a.frames.split(",")]
    rows, ok = [], True
    for f in frames:
        gm, g_casters, g_circles = game_maps(a.capture, f, a.out)
        rc, counts, sh, ci = ours(a.capture, f, a.out)
        casters, drawn, val = num(counts, r"casters (\d+) objects"), num(counts, r"circles made \d+ \(drawn (\d+)"), num(counts, r"validation errors: (\d+)")
        s_iou = iou(footprint_shadow(gm["mapB"]), footprint_shadow(sh)) if sh else 0.0
        c_iou = iou(footprint_circles(gm["props"]), footprint_circles(ci)) if ci else 0.0
        mean_d = mean_diff(gm["mapB"], sh)
        good = rc == 0 and val == 0 and casters > 0 and 0 < drawn <= g_circles and s_iou >= SHADOW_IOU and mean_d <= MEAN_D and c_iou >= CIRCLE_IOU
        ok = ok and good
        print(f"frame {f}: game casters {g_casters} circles {g_circles}; ours casters {casters} circles drawn {drawn}; shadow IoU {s_iou:.3f} mean|d| {mean_d:.1f}; circles IoU {c_iou:.3f}; validation {val}; {'PASS' if good else 'FAIL'}")
        rows.append((f, gm, sh, ci, s_iou, c_iou))
        if a.faults and f == frames[len(frames) // 2]:
            for fault, bar in (("transpose_light", "shadow"), ("no_casters", "shadow"), ("scale_prm", "mean"), ("circles_wrong_frame", "circles"), ("circles_all_stamps", "circles")) + ((("circles_no_mesh", "circles"),) if LATCH else ()):
                other = frames[0] if frames[0] != f else frames[-1]
                rc2, c2, sh2, ci2 = ours(a.capture, f, a.out, fault, other)
                cas2, dr2 = num(c2, r"casters (\d+) objects"), num(c2, r"circles made \d+ \(drawn (\d+)")
                si2 = iou(footprint_shadow(gm["mapB"]), footprint_shadow(sh2)) if sh2 else 0.0
                md2 = mean_diff(gm["mapB"], sh2) if sh2 else 255.0
                ci_iou2 = iou(footprint_circles(gm["props"]), footprint_circles(ci2)) if ci2 else 0.0
                # judged on the PICTURE bars (and, for the duplicates, on "drawn <= the game's distinct circles"), never on the counter the plant zeroes
                failed = (si2 < SHADOW_IOU) if bar == "shadow" else (md2 > MEAN_D) if bar == "mean" else (ci_iou2 < CIRCLE_IOU or dr2 > g_circles)
                print(f"  plant {fault}: casters {cas2}, circles drawn {dr2}, shadow IoU {si2:.3f} mean|d| {md2:.1f} circles IoU {ci_iou2:.3f} -> {'FAILS its bar (as it must)' if failed else 'DID NOT FAIL: the check is blind to it'}")
                ok = ok and failed
                if sh2 is not None:
                    rows.append((f"{f} PLANT {fault}", gm, sh2, ci2, si2, 0.0))
    # the sheet: per row game | ours | diff for map B, then props
    W = 512
    sheet = Image.new("RGB", (W * 6, (W + 14) * len(rows)), (40, 40, 40))
    for r, (f, gm, sh, ci, s_iou, c_iou) in enumerate(rows):
        y = r * (W + 14)
        if sh is None:
            continue
        tiles = [label(gm["mapB"], f"frame {f}: GAME map B (wrestlers)"), label(sh, f"OURS shadow map (IoU {s_iou:.2f})"), label(diff_heat(gm["mapB"], sh), "difference x3"),
                 label(gm["props"], f"frame {f}: GAME props map (blue = circles)"), label(ci if ci else gm["props"], "OURS circles map"), label(diff_heat(gm["props"], ci) if ci else gm["props"], "difference x3")]
        for i, t in enumerate(tiles):
            sheet.paste(t.resize((W, W)), (i * W, y))
    path = os.path.join(a.out, "sheet_maps.png")
    sheet.save(path)
    print("sheet:", path)
    print("RESULT:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


main()
