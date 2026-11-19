"""Lane PM2 (Session 103) offline check of the props map's PROP CASTERS (docs\\research\\s103\\PMC.md, PMW.md, RMP.md 2; slice 2 of PMF.md). 0 game runs.

  py -I tools\\scene\\pm2_check.py [--capture DIR] [--frame 100] [--out DIR] [--faults]

No offline record carries weapon rows (sections 59-61 are written by the live hooks), so the props are a labelled FIXTURE, as lane MBC's (frame_rig.cpp InjectMapBFixture): a box given to the front end the way
ApplyWeaponRows gives a weapon (PosedModel + SetPropCpu), tagged by the SAME production filter (weapons::CastsPropsMap), plus a second box that is a gm-set item (the filter must refuse it). NO capture of
the repo holds the game's own weapon rows: the footprint of the GAME's props is not proved here; the first live prop-bearing frame's census line `[scene][maps][props] casters N ...` settles it.

Per frame (the REAL front end, frame_rig.exe, on the main checkout's z7147 capture: its record carries the circle rows and the props light, so the props map is made):
 - BASE : the run with no fixture hands over a props map, and its RED channel is the clear everywhere (blobs live in blue): the casters add nothing by themselves.
 - COVER: with the fixture box, the props map's red channel is lit over the box's footprint: the footprint (pixels whose red is not the clear) against the box's ANALYTIC shadow = its 8 corners projected through the floor's
   own TexProjLight rows (the matrix the floor reads the map with), convex hull, IoU >= PM2_IOU (0.80). The gm-set box's analytic footprint stays EMPTY (gm sets never cast: categories 23 / 36).
 - MODE : the same with scene word 1 + 3 participants (map B's mode 1 = wrestlers only): the props map STILL holds the prop (the props pass has no mode gate; PMC section 3).
 - CENSUS: `[scene][maps][props] casters` says >= 1 object, and validation_errors 0.
Planted twins (--faults), each must FAIL its bar: props_no_props (the map without its props: PM2 NONE), props_casts_gm (the gm box casts: PM2 GM), the switch off (WWE_RDR_PROPS_CASTERS=0: PM2 NONE).
Nothing from the game is written into the repo: pictures under out\\s103\\PM2 (scratch).
"""
import argparse, os, re, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw

MAIN = r"C:\Users\Jarrod\wwe2k14-recomp"
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
PM2_IOU = 0.80
PM2_DEPTH_TOL = 2.0   # review 2: 8-bit steps of the stored depth
RIG_LINKS = ["--link", "actor:slot:0=343:0", "--link", "actor:slot:6=324:0", "--body-scale-one"]   # as pms_check.py: z7147's two wrestlers, TOLD (the record names no model file)


def run(cmd, env=None, timeout=1800):
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=timeout, env=env)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def arm(capture, frame, out, name, rig_args="", maps_fault="", env_extra=None):
    """One frame through the REAL front end; returns (report text, props-map PNG path or None, rc)."""
    d = os.path.join(out, f"{name}_f{frame}")
    os.makedirs(d, exist_ok=True)
    env = dict(os.environ, WWE_RDR_VALIDATION="1", WWE_RDR_PROPS_MAP="1", CLAUDECODE="1", WWE_FVIEW_ALLOW_STALE="1")   # (fview only replays the capture to a record; the main checkout's build predates this lane's one new flag bit)
    env.update(env_extra or {})
    cmd = ["py", "-I", os.path.join(ROOT, "tools", "scene", "frame_rig.py"), "--capture", capture, "--frames", str(frame), "--out", d, "--no-sheet", "--repeat", "1"] + RIG_LINKS
    if maps_fault:
        cmd += ["--maps-fault", maps_fault]
    if rig_args:
        cmd += ["--rig-args", rig_args]
    rc, text = run(cmd, env)
    rep = os.path.join(d, f"frame_{frame:03d}_report.txt")
    body = open(rep).read() if os.path.exists(rep) else text
    png = os.path.join(d, "work_capture", f"ours_{frame}_map_circles.png")   # frame_rig.py: prefix = <out>/work_<capture>/ours_<frame>
    return body, (png if os.path.exists(png) else None), rc


def parse(body):
    r = {}
    m = re.search(r"pm2_props_matrix((?: \S+){16})", body)
    r["matrix"] = np.array([float(x) for x in m.group(1).split()]).reshape(4, 4) if m else None   # register i = row i (VS c68-c71 layout)
    m = re.search(r"pm2_props_prm((?: \S+){12})", body)
    r["prm"] = np.array([float(x) for x in m.group(1).split()]).reshape(3, 4) if m else None   # PS c168-c170 as the floor reads them
    m = re.search(r"pm2_anchor lo (\S+) (\S+) (\S+) hi (\S+) (\S+) (\S+) viewer_space (\d)", body)
    r["lo"], r["hi"] = (np.array([float(m.group(i)) for i in (1, 2, 3)]), np.array([float(m.group(i)) for i in (4, 5, 6)])) if m else (None, None)
    m = re.search(r"\[scene\]\[maps\]\[props\] casters (\d+) objects / (\d+) meshes \(weapons (\d+) / event (\d+) / stage (\d+), ids 59/68/71/96 (\d+)\), skipped: no model (\d+), piece 43 assumed (\d+) \(the re-key condition is not recorded\), no props light (\d+); prop min light-depth < near (\d+); (.*)", body)
    r["census"] = dict(objects=int(m.group(1)), meshes=int(m.group(2)), weapons=int(m.group(3)), no_model=int(m.group(7)), no_light=int(m.group(9)), why=m.group(11).strip()) if m else None
    r["tags"] = {int(s): int(t) for s, t in re.findall(r"pm2_fixture slot (\d) \S+(?: \S+)? props_tagged (\d)", body)}
    v = re.search(r"validation_errors (\d+)", body)
    r["validation"] = int(v.group(1)) if v else -1
    return r


def hull_mask(pts, W, H):
    """Pixel centres inside the convex hull of pts (Andrew's monotone chain), as a bool (H, W) mask."""
    P = sorted(map(tuple, pts))
    def half(seq):
        h = []
        for p in seq:
            while len(h) >= 2 and (h[-1][0] - h[-2][0]) * (p[1] - h[-2][1]) - (h[-1][1] - h[-2][1]) * (p[0] - h[-2][0]) <= 0:
                h.pop()
            h.append(p)
        return h
    Hh = np.array(half(P)[:-1] + half(P[::-1])[:-1])
    xs, ys = np.meshgrid(np.arange(W) + 0.5, np.arange(H) + 0.5)
    ok = np.ones((H, W), bool)
    for i in range(len(Hh)):
        a, b = Hh[i], Hh[(i + 1) % len(Hh)]
        ok &= (b[0] - a[0]) * (ys - a[1]) - (b[1] - a[1]) * (xs - a[0]) >= 0
    return ok


def analytic(matrix, c, half, W=512, H=512):
    """The box's shadow in the props map: its 8 corners (the game's space) through the floor's TexProjLight rows (u = r0 . p / r3 . p, v = r1 . p / r3 . p), convex hull."""
    corners = np.array([[c[0] + sx * half, c[1] + sy * half, c[2] + sz * half, 1.0] for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)])
    d = corners @ matrix.T   # (8, 4): registers 0..3 dotted with each corner
    if np.any(d[:, 3] <= 0):
        return None
    uv = np.stack([d[:, 0] / d[:, 3] * W, d[:, 1] / d[:, 3] * H], axis=1)
    if np.any(uv < 0) or np.any(uv > [W, H]):
        return None   # the box is not wholly in the map: the fixture is misplaced (say so, never a quiet pass)
    return hull_mask(uv, W, H)


def red_mask(png):
    a = np.asarray(Image.open(png).convert("RGB")).astype(int)
    return a[..., 0] != 255


def label(img, text):
    im = img.copy()
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, im.width, 14], fill=(0, 0, 0))
    d.text((3, 2), text, fill=(255, 255, 255))
    return im


def judge(png, r, fx_c, gx_c, half):
    """-> (tokens that failed, text, iou, gm_pixels, observed count)."""
    if png is None or r["matrix"] is None:
        return ["PM2 NO MAP"], "no props map / matrix in the report", 0.0, 0, 0
    obs = red_mask(png)
    exp = analytic(r["matrix"], fx_c, half)
    gm = analytic(r["matrix"], gx_c, half)
    if exp is None or gm is None:
        return ["PM2 PLACEMENT"], "the fixture box is not wholly inside the props map (move it)", 0.0, 0, int(obs.sum())
    inter, union = int((obs & exp).sum()), int((obs | exp).sum())
    iou = inter / union if union else 0.0
    gm_px = int((obs & gm).sum())
    fails = []
    if int(obs.sum()) == 0:
        fails.append("PM2 NONE")
    elif iou < PM2_IOU:
        fails.append("PM2 IOU")
    if gm_px > 0:
        fails.append("PM2 GM")
    extra = ""
    if int(obs.sum()) > 0 and r.get("prm") is not None:
        # DEPTH: the red value inside the footprint = the box's nearest point to the light, (row3 . p - near) / (far - near) * 255 (the floor's own depth row: ViewLight[2] = TexProjLight row 3), +-2
        rgb = np.asarray(Image.open(png).convert("RGB")).astype(int)
        M, prm = r["matrix"], r["prm"]
        near, far = float(prm[0][0]), float(prm[0][1])
        corners = np.array([[fx_c[0] + sx * half, fx_c[1] + sy * half, fx_c[2] + sz * half, 1.0] for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)])
        top = float((corners @ M[3]).min())
        want = (top - near) / (far - near) * 255.0
        inside = obs & exp
        got = float(np.median(rgb[..., 0][inside])) if inside.any() else -1.0
        extra += f"; red in the footprint {got:.0f} vs the box top's analytic depth {want:.1f} (+-{PM2_DEPTH_TOL:.0f})"
        if abs(got - want) > PM2_DEPTH_TOL:
            fails.append("PM2 DEPTH")
        # RECEIVER: bg_shadow_amount's depth term (fx_bg.glsl) for the floor point under the box (must be > 0) and for one well outside it (must be 0), read from the map the way the floor reads it
        def amount(p):
            d = M @ np.array([p[0], p[1], p[2], 1.0])
            u, v = d[0] / d[3] * 512.0, d[1] / d[3] * 512.0
            ix, iy = min(max(int(u), 0), 511), min(max(int(v), 0), 511)
            mx = rgb[iy, ix, 0] / 255.0
            rng, soft, farp = prm[0], prm[1], prm[2]
            rec = min(max((d[3] - rng[0]) / (rng[1] - rng[0]) - rng[2], 0.0), 1.0)
            gap = rec - mx
            near_part = 1.0 - (1.0 - min(max(gap * soft[2], 0.0), 1.0)) ** soft[3]
            far_part = (1.0 - min(max((gap - farp[0]) * farp[1], 0.0), 1.0)) ** farp[2]
            layer = near_part + (near_part * far_part - near_part) * farp[3]
            return rng[3] * layer
        under = amount([fx_c[0], fx_c[1] + half, fx_c[2]])           # the floor under the box's centre (its bottom face is the floor)
        away = amount([fx_c[0] + 3.0 * half * 2, fx_c[1] + half, fx_c[2]])   # clear of the box and of the wrestlers' side
        extra += f"; floor shadow amount under the box {under:.4f} (> 0), outside {away:.4f} (= 0)"
        if not (under > 0.0 and away == 0.0):
            fails.append("PM2 RECEIVER")
    return fails, f"expected (analytic) {int(exp.sum())} px, observed {int(obs.sum())}, both {inter}, IoU {iou:.3f} (bar {PM2_IOU}); gm box pixels {gm_px} (must be 0)" + extra, iou, gm_px, int(obs.sum())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", default=os.path.join(MAIN, r"out\d3caster\z7147\capture"))
    ap.add_argument("--frame", type=int, default=100)
    ap.add_argument("--out", default=os.path.join(MAIN, r"out\s103\PM2"))
    ap.add_argument("--faults", action="store_true", help="also run the planted twins (props_no_props, props_casts_gm, the switch off): each must FAIL its bar")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    ok_all = True
    base_txt, base_png, rc0 = arm(a.capture, a.frame, a.out, "base")
    b = parse(base_txt)
    if b["matrix"] is None or b["lo"] is None or base_png is None:
        print(f"FAIL  frame {a.frame}: PM2 NO MAP: the baseline run made no props map (exit {rc0}); see {a.out}")
        sys.exit(1)
    base_red = red_mask(base_png)
    base_ok = int(base_red.sum()) == 0
    print(f"{'PASS' if base_ok else 'FAIL'}  frame {a.frame}: BASE: props map handed over, red channel lit pixels without a fixture {int(base_red.sum())} (must be 0: blobs are blue only)")
    ok_all = ok_all and base_ok
    lo, hi = b["lo"], b["hi"]
    R = float(max(hi - lo)) / 2.0 or 1.0
    cx, cz, floor_y = float((lo[0] + hi[0]) / 2), float((lo[2] + hi[2]) / 2), float(hi[1])
    half = 0.18 * R
    fx_c = [cx + 1.3 * R, floor_y - half, cz]
    gx_c = [cx - 1.3 * R, floor_y - half, cz]
    fix = f"--prop-fixture {fx_c[0]:.6g},{fx_c[1]:.6g},{fx_c[2]:.6g},{half:.6g} --gm-fixture {gx_c[0]:.6g},{gx_c[1]:.6g},{gx_c[2]:.6g},{half:.6g}"
    print(f"  fixture: weapon box centre {fx_c} half {half:.3f}, gm box centre {gx_c} (wrestler 0 spans lo {lo.round(2).tolist()} hi {hi.round(2).tolist()}; the game's space, y down)")

    def one(name, extra="", fault="", env_extra=None, must_fail=None, mode=""):
        nonlocal ok_all
        txt, png, rc = arm(a.capture, a.frame, a.out, name, rig_args=fix + " --scene-word 4 " + extra, maps_fault=fault, env_extra=env_extra)
        r = parse(txt)
        fails, text, iou, gm_px, nobs = judge(png, r, fx_c, gx_c, half)
        cen = r["census"]
        extra_fail = []
        if must_fail is None:   # a positive arm: also the census, the tags and the validation layer
            if cen is None or cen["objects"] < 1:
                extra_fail.append("PM2 CENSUS")
            if r["tags"].get(0, 0) != 1 or r["tags"].get(1, 1) != 0:
                extra_fail.append("PM2 TAG")
            if r["validation"] != 0:
                extra_fail.append("PM2 VALIDATION")
        allf = fails + extra_fail
        cen_s = f"casters {cen['objects']} objects / {cen['meshes']} meshes ({cen['why']})" if cen else "no [scene][maps][props] census line"
        if must_fail is None:
            ok = not allf
            print(f"{'PASS' if ok else 'FAIL'}  frame {a.frame}: {name}: {text}; {cen_s}; tags {r['tags']}; validation errors {r['validation']}" + ("" if ok else f"  -> {', '.join(allf)}"))
            ok_all = ok_all and ok
        else:
            caught = must_fail in allf
            print(f"{'PASS' if caught else 'FAIL'}  frame {a.frame}: plant {name}: {text}; {cen_s} -> {'FAILS its bar (' + must_fail + ') as it must' if caught else 'DID NOT FAIL ' + must_fail + ': the check is blind to it'}")
            ok_all = ok_all and caught
        return png, iou

    png_on, iou_on = one("cover", "", "")
    png_m1, _ = one("mode1", " --scene-word 1 --participants 3", "")   # (the later --scene-word wins over the 4 in `fix`)
    if a.faults:
        png_np, _ = one("plant_no_props", "", "props_no_props", must_fail="PM2 NONE")
        png_gm, _ = one("plant_casts_gm", "", "props_casts_gm", must_fail="PM2 GM")
        png_vd, _ = one("plant_viewdepth_row2", "", "props_viewdepth_row2", must_fail="PM2 DEPTH")
        png_off, _ = one("plant_switch_off", "", "", env_extra={"WWE_RDR_PROPS_CASTERS": "0"}, must_fail="PM2 NONE")
    try:
        S = 384
        tiles = [label(Image.open(base_png).convert("RGB").resize((S, S)), "props map, no fixture (R/G clear, blobs blue)", ), ]
        for nm, pth in (("cover", png_on), ("mode1", png_m1)):
            if pth:
                tiles.append(label(Image.open(pth).convert("RGB").resize((S, S)), f"props map + fixture ({nm})"))
        Image.fromarray(np.concatenate([np.asarray(t) for t in tiles], axis=1)).save(os.path.join(a.out, "sheet_pm2.png"))
        print("sheet:", os.path.join(a.out, "sheet_pm2.png"))
    except Exception as ex:   # the sheet is a courtesy
        print(f"(no sheet: {ex})")
    print("PM2 RESULT:", "PASS" if ok_all else "FAIL")
    sys.exit(0 if ok_all else 1)


if __name__ == "__main__":
    main()
