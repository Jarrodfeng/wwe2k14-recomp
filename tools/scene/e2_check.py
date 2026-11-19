"""Lane E2 (Session 103) offline check of the rim, plane, pre-light and sweat maps of the Maps stage hook against a capture: our maps beside the game's own, one labelled sheet, counts, planted faults.

  py -I tools\\scene\\e2_check.py [--capture DIR] [--frames 20,100,180] [--out DIR] [--faults] [--maps rim,prelight,...]

Per frame: fview (the main checkout's built exe, headless, read-only) saves the game's map after its pass (picture.png of the pass's last op, as e_check.py does); maps_draw.exe draws OUR maps from the
same frame's scene record. The bars (per map, see BARS): footprint IoU of the covered pixels and the mean difference inside the common footprint; validation 0; the pass made something.
Planted faults (--faults): each must FAIL its bar. Nothing from the game is written into the repo: pictures go under out\\s103\\E2 (scratch, gitignored).
"""
import argparse, json, os, re, subprocess, sys
BS = chr(10)   # a newline
import numpy as np
from PIL import Image, ImageDraw

MAIN = r"C:\Users\Jarrod\wwe2k14-recomp"
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
FVIEW = os.path.join(MAIN, r"out\renderer\dev\fview.exe")
WRESTLERS = ["--wrestler", "0=343", "--wrestler", "6=324"]   # the live record names no model file (lane E gap 1): the rig is told who stands in which slot


def run(cmd, env=None, timeout=300):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
    return p.returncode, p.stdout + p.stderr


ENV = dict(os.environ, CLAUDECODE="1")


def ops_of(capture, frame, out):
    js = os.path.join(out, f"ops{frame}.json")
    if not os.path.exists(js):
        rc, text = run([FVIEW, "--capture", capture, "--frame", str(frame), "--ops", "--json", js, "--root", ROOT, "--game", os.path.join(MAIN, r"game_data\extracted")], ENV)
        if rc != 0 or not os.path.exists(js):
            raise SystemExit("fview --ops failed: " + text[-400:])
    return json.load(open(js))["ops"]


def passes_of(ops):
    passes, cur = [], None
    for o in ops:
        if o["kind"] == "targets":
            cur = {"op": o["op"], "surface": o["colors"][0] if o["colors"] else 0, "tech": {}, "draws": [], "last": o["op"]}
            passes.append(cur)
        elif cur is not None:
            cur["last"] = o["op"]
            if o["kind"] == "draw":
                cur["tech"][o.get("technique", "")] = cur["tech"].get(o.get("technique", ""), 0) + 1
                cur["draws"].append(o)
    return passes


def surface_after(capture, frame, p):
    """The surface the pass wrote, as it stood after its last op (a copy follows it)."""
    rc, text = run([FVIEW, "--capture", capture, "--frame", str(frame), "--surface", str(p["surface"]), "--through-op", str(p["last"]), "--save-case", "--op", str(p["last"]), "--root", ROOT, "--game", os.path.join(MAIN, r"game_data\extracted")], ENV)
    line = [l for l in text.splitlines() if "case saved:" in l]
    if not line:
        raise SystemExit("fview --save-case failed: " + text[-300:])
    return Image.open(os.path.join(line[-1].split("case saved:")[1].strip(), "picture.png")).convert("RGB")


def game_rim(capture, frame, out):
    ops = ops_of(capture, frame, out)
    ps = [p for p in passes_of(ops) if "T_RimLightMap" in p["tech"]]
    if not ps:
        raise SystemExit(f"frame {frame}: no rim pass in the stream")
    return surface_after(capture, frame, ps[-1]), ps[-1]["tech"]["T_RimLightMap"]


BONES_BACK = 1   # the game's map at stream frame F is drawn with the poses of record F-1 (SF1 run 7147), as e_check.py


def ours(capture, frame, out, faults="", told=None, extra=None):
    d = os.path.join(out, f"ours_f{frame}" + (f"_{faults}" if faults else ""))
    cmd = [os.path.join(ROOT, r"out\tools\maps_draw.exe"), capture, "--frame", str(frame), "--game", os.path.join(MAIN, r"game_data\extracted"), "--out", d] + WRESTLERS
    if BONES_BACK:
        cmd += ["--bones-frame", str(frame - BONES_BACK)]
    if faults:
        cmd += ["--fault", faults]
    if told:
        cmd += ["--told", told]
    if CONST_EYE:
        cmd += ["--const-eye"]   # S103 E4 fixture
    if MODULE_EYE:
        cmd += ["--eye-module"]   # S103 E5: the latch reader on a fake plane module (the production reader, a fixture memory)
    if extra:
        cmd += extra
    rc, text = run(cmd, dict(os.environ, WWE_RDR_VALIDATION="1"))   # the validation layer ON: "validation 0" must mean something
    counts = open(os.path.join(d, "counts.txt")).read() if os.path.exists(os.path.join(d, "counts.txt")) else text
    img = {}
    for k in ["rim", "plane", "sweat"] + [f"prelight{i}" for i in range(8)]:
        p = os.path.join(d, f"ours_{k}.png")
        if os.path.exists(p):
            img[k] = Image.open(p).convert("RGB")
    return rc, counts, img


def num(counts, key):
    m = re.search(key, counts)
    return int(m.group(1)) if m else 0


def label(img, text):
    im = img.copy()
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, im.width, 14], fill=(0, 0, 0))
    d.text((3, 2), text, fill=(255, 255, 255))
    return im


def diff_heat(a, b):
    x = np.abs(np.asarray(a).astype(int) - np.asarray(b).astype(int)).max(axis=2)
    h = np.clip(x * 3, 0, 255).astype(np.uint8)
    return Image.fromarray(np.stack([h, h // 3, 255 - h], axis=2).astype(np.uint8))


def iou(a, b):
    u = (a | b).sum()
    return float((a & b).sum()) / u if u else 1.0


def footprint_bg(img):
    """Pixels that differ from the picture's own background (its corner pixel): the pass's coverage."""
    a = np.asarray(img).astype(int)
    return (np.abs(a - a[0, 0]).max(axis=2) > 6)


def compare_footprint(g, o, channel=None):
    """IoU of the coverage and the mean |d| inside the common footprint (over `channel` only when given: the rim map's information is its green)."""
    fg, fo = footprint_bg(g), footprint_bg(o)
    both = fg & fo
    a, b = np.asarray(g).astype(int), np.asarray(o).astype(int)
    if channel is not None:
        a, b = a[..., channel], b[..., channel]
    mean_d = float(np.abs(a - b)[both].mean()) if both.any() else 255.0
    return iou(fg, fo), mean_d, int(fo.sum())


def draw_case(capture, frame, p, op, out):
    """The game's own description of draw `op` (constants by name) from fview --save-case: the rig-told inputs of lane A's missing readers (a test fixture, never a live path)."""
    rc, text = run([FVIEW, "--capture", capture, "--frame", str(frame), "--surface", str(p["surface"]), "--through-op", str(op), "--save-case", "--op", str(op), "--root", ROOT, "--game", os.path.join(MAIN, r"game_data\extracted")], ENV)
    line = [l for l in text.splitlines() if "case saved:" in l]
    if not line:
        raise SystemExit("fview --save-case failed: " + text[-300:])
    return json.load(open(os.path.join(line[-1].split("case saved:")[1].strip(), "case.json")))["draw"]


def consts(draw):
    return {c["field"]: c["values"] for c in draw.get("constants", [])}


def f4(v):
    return " ".join(repr(float(x)) for x in v)


# ---- the maps --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
RIM_IOU, RIM_MEAN_D = 0.85, 9.0
PLANTS = {"rim": ["rim_shadow_camera", "rim_no_depth", "rim_clear_black"]}
RIM_OFF_R_D = 8.0   # S103 B1F: the off-body red of the rim map (the game clears to (255,0,255,255); the skin shader adds R to the rim term)


def off_body_r(g, o):
    """Mean red of the game's and of our map where NEITHER drew a caster (outside both footprints): the clear colour. Returns (game R, our R, pixel count)."""
    off = ~(footprint_bg(g) | footprint_bg(o))
    if not off.any():
        return 0.0, 255.0, 0
    return float(np.asarray(g)[..., 0][off].mean()), float(np.asarray(o)[..., 0][off].mean()), int(off.sum())


def check_rim(capture, frame, out, rows, ok_all):
    g, g_draws = game_rim(capture, frame, out)
    rc, counts, img = ours(capture, frame, out)
    o = img.get("rim")
    made = num(counts, r"rim made (\d+)")
    val = num(counts, r"validation errors: (\d+)") + (0 if num(counts, r"validation layer active: (\d+)") else 1000)   # layer not active = a failure, never a silent pass
    s_iou, mean_d, cov = compare_footprint(g, o, 1) if o is not None else (0.0, 255.0, 0)
    gr, orr, noff = off_body_r(g, o) if o is not None else (0.0, 255.0, 0)
    good = rc == 0 and val == 0 and made > 0 and s_iou >= RIM_IOU and mean_d <= RIM_MEAN_D and abs(gr - orr) <= RIM_OFF_R_D
    print(f"frame {frame}: RIM off-body red game {gr:.1f} ours {orr:.1f} over {noff} px (bar {RIM_OFF_R_D})")
    print(f"frame {frame}: RIM game draws {g_draws}; ours made {made} (meshes {num(counts, r'rim made \d+ \(objects \d+, meshes (\d+)')}); footprint IoU {s_iou:.3f} mean|d| inside {mean_d:.1f}; validation {val}; {'PASS' if good else 'FAIL'}")
    rows.append((f"{frame} rim", g, o, s_iou))
    return good, (g, o, s_iou, mean_d)


def check_rim_plants(capture, frame, out, rows, base):
    g, o, s_iou, mean_d = base
    ok = True
    for fault in PLANTS["rim"]:
        rc, c2, img2 = ours(capture, frame, out, fault)
        o2 = img2.get("rim")
        i2, d2, cov2 = compare_footprint(g, o2, 1) if o2 is not None else (0.0, 255.0, 0)
        gr2, or2, _ = off_body_r(g, o2) if o2 is not None else (0.0, 255.0, 0)
        failed = not (i2 >= RIM_IOU and d2 <= RIM_MEAN_D and abs(gr2 - or2) <= RIM_OFF_R_D)   # the SAME bar as the clean run (incl. off-body red)
        print(f"  plant {fault}: footprint IoU {i2:.3f} mean|d| {d2:.1f} -> {'FAILS its bar (as it must)' if failed else 'DID NOT FAIL: the check is blind to it'}")
        ok = ok and failed
        if o2 is not None:
            rows.append((f"{frame} rim PLANT {fault}", g, o2, i2))
    return ok


PLANE_PREC, PLANE_MEAN_D = 0.60, 9.0
CONST_EYE = "--const-eye" in sys.argv   # S103 E4
MODULE_EYE = "--module-eye" in sys.argv   # S103 E5
PLANTS["plane"] = ["plane_identity_fit", "plane_stale_camera"] + (["plane_eye_ring"] if (CONST_EYE or MODULE_EYE) else [])


def game_plane_and_told(capture, frame, out):
    ops = ops_of(capture, frame, out)
    ps = [p for p in passes_of(ops) if "T_PlaneShadowMap" in p["tech"]]
    if not ps:
        raise SystemExit(f"frame {frame}: no plane pass in the stream")
    p = ps[0]
    first = next(d for d in p["draws"] if d.get("technique") == "T_PlaneShadowMap")
    c = consts(draw_case(capture, frame, p, first["op"], out))
    fade = [c["ShadowDistance"][0], c["ShadowDistance"][1], c["ShadowDistance"][2], c["ShadowDistance"][3], c["ShadowRevision"][3], c["ShadowMiddleRev"][3], c["ShadowLeastRev"][3], c["ShadowDeep"][3], c["ShadowMiddleDeep"][3], c["ShadowLeastDeep"][3]]
    told = os.path.join(out, f"told_plane_f{frame}.txt")
    with open(told, "w") as f:
        print("    stream first mask draw: EyePos " + f4(c["EyePos"]) + " PlanePos " + f4(c["PlanePos"]) + ("  (S103 E4 --const-eye / S103 E5 --module-eye: NOT told)" if (CONST_EYE or MODULE_EYE) else ""))
        if not (CONST_EYE or MODULE_EYE):   # S103 E5: with --module-eye the record carries the eye (the latch reader); S103 E4: with --const-eye the rig tells neither: the hook constants (wrestler eye, PlanePos (0, -120, 0)) are what is proved
            f.write("plane_eye " + f4(c["EyePos"]) + BS)
            f.write("plane_pos " + f4(c["PlanePos"]) + BS)
        f.write("plane_fade " + f4(fade) + BS)
        f.write("plane_fit " + f4(c["PlaneInv"]) + BS)
        f.write("plane_camera " + f4(c["CameraViewProj"]) + BS)
    return surface_after(capture, frame, p), first["op"], told


def plane_good(g, o, rc, val, made, cov, mean_d):
    """THE plane predicate: clean runs must satisfy it, planted runs must not."""
    prec = 0.0
    if o is not None:
        fg, fo = footprint_bg(g), footprint_bg(o)
        prec = float((fg & fo).sum()) / max(1, int(fo.sum()))
    return (rc == 0 and val == 0 and made > 0 and cov > 500 and prec >= PLANE_PREC and mean_d <= PLANE_MEAN_D), prec


def check_plane(capture, frame, out, rows, ok_all):
    g, _, told = game_plane_and_told(capture, frame, out)
    rc, counts, img = ours(capture, frame, out, "", told)
    o = img.get("plane")
    made = num(counts, r"plane made (\d+)")
    val = num(counts, r"validation errors: (\d+)") + (0 if num(counts, r"validation layer active: (\d+)") else 1000)   # layer not active = a failure, never a silent pass
    s_iou, mean_d, cov = compare_footprint(g, o) if o is not None else (0.0, 255.0, 0)
    # The game's plane map holds the ring's rope proxies always, and a wrestler's proxy only while its own gate lets it in (sub_827EE380's distance test: not in the record, LIVE_MAPS.md E2 gaps);
    # ours draws every wrestler. So the bar is PRECISION on frames where the game has the wrestlers in (the ropes are not ours to draw: recall is printed, not judged).
    good, prec = plane_good(g, o, rc, val, made, cov, mean_d)
    print(f"frame {frame}: PLANE ours made {made} (draws {num(counts, r'plane made \d+ \(draws (\d+)')}); covered {cov} px; precision {prec:.3f} (recall {s_iou:.3f} IoU incl. ropes) mean|d| inside {mean_d:.1f}; validation {val}; {'PASS' if good else 'FAIL'}")
    rows.append((f"{frame} plane", g, o, s_iou))
    return good, (g, o, prec, mean_d, told)


EYE_WRESTLER = (-450.0, -1400.0, 0.0)
EYE_RING = ((0.0, -800.0, 1000.0), (-1000.0, -800.0, 0.0), (0.0, -800.0, -1000.0), (1000.0, -800.0, 0.0))
POS_CONST = (0.0, -120.0, 0.0)


def check_eye_exact(capture, frame, out):
    """S103 E4 review F2: EVERY mask draw of the frame carries a PlanePos equal to the fixture's and an EyePos that is the wrestler proxy's (-450, -1400, 0) or one of the ring proxy's four points
    (|diff| < 0.01), and the wrestler eye occurs. This proves the fixture values for THIS arena only: EyePos is per-arena module data (one arena of evidence on disk)."""
    ops = ops_of(capture, frame, out)
    ps = [p for p in passes_of(ops) if "T_PlaneShadowMap" in p["tech"]]
    p = ps[0]
    draws = [d for d in p["draws"] if d.get("technique") == "T_PlaneShadowMap"]
    bad, n_w = 0, 0
    for d in draws:
        c = consts(draw_case(capture, frame, p, d["op"], out))
        eye, pos = tuple(c["EyePos"][:3]), tuple(c["PlanePos"][:3])
        is_w = all(abs(a - b) < 0.01 for a, b in zip(eye, EYE_WRESTLER))
        is_r = any(all(abs(a - b) < 0.01 for a, b in zip(eye, r)) for r in EYE_RING)
        n_w += int(is_w)
        if not (is_w or is_r) or not all(abs(a - b) < 0.01 for a, b in zip(pos, POS_CONST)):
            bad += 1
    ok = bad == 0 and n_w > 0 and len(draws) > 0
    print(f"frame {frame}: PLANE EYE EXACT: {len(draws)} mask draws, wrestler-proxy eye (-450,-1400,0) on {n_w}, {bad} draws with an unknown eye or a PlanePos != (0,-120,0): {'PASS' if ok else 'FAIL'}  [one arena only: per-arena data, LIVE_MAPS.md E4]")
    return ok


def eyes_of_counts(counts):
    """S103 E5: the eyes the latch reader gave maps_draw (the line 'plane eyes from module:')."""
    m = re.search(r"plane eyes from module: why (\d+) have 0x([0-9a-f]+) wrestler (\S+) (\S+) (\S+) ring4 (\S+) (\S+) (\S+) ring5 (\S+) (\S+) (\S+) ring6 (\S+) (\S+) (\S+) ring7 (\S+) (\S+) (\S+) planepos (\S+)", counts)
    if not m:
        return None
    v = [float(x) for x in m.groups()[2:]]
    return {"why": int(m.group(1)), "have": int(m.group(2), 16), "wrestler": tuple(v[0:3]), "ring": [tuple(v[3:6]), tuple(v[6:9]), tuple(v[9:12]), tuple(v[12:15])], "pos_y": v[15]}


def stream_eyes(capture, frame, out):
    ops = ops_of(capture, frame, out)
    ps = [p for p in passes_of(ops) if "T_PlaneShadowMap" in p["tech"]]
    p = ps[0]
    res = []
    for d in [d for d in p["draws"] if d.get("technique") == "T_PlaneShadowMap"]:
        c = consts(draw_case(capture, frame, p, d["op"], out))
        res.append((tuple(c["EyePos"][:3]), tuple(c["PlanePos"][:3])))
    return res


def close3(a, b, tol=0.01):
    return all(abs(x - y) < tol for x, y in zip(a, b))


def check_eye_module(capture, frame, out, extra=None, label="clean"):
    """S103 E5: the eyes the LATCH READER recomputes from the (fixture) plane module equal the stream's EyePos, both ways: every mask draw of the frame carries the wrestler kind's eye or one of the four ring kinds'
    eyes (|d| < 0.01), the wrestler kind's eye occurs on at least one draw, EACH of the four ring kinds' eyes occurs on at least one draw, and PlanePos y equals the stream's. The module's tables are a fixture
    (no capture holds them); the formula and the stream's values are [C]."""
    rc, counts, _ = ours(capture, frame, out, "", None, extra)
    ey = eyes_of_counts(counts)
    if ey is None or ey["why"] != 0:
        print(f"frame {frame}: PLANE EYE MODULE ({label}): the reader gave no eyes (rc {rc}): FAIL")
        return False
    draws = stream_eyes(capture, frame, out)
    known = [ey["wrestler"]] + ey["ring"]
    bad = sum(1 for e, pos in draws if not any(close3(e, k) for k in known) or abs(pos[1] - ey["pos_y"]) > 0.01)
    seen_w = sum(1 for e, pos in draws if close3(e, ey["wrestler"]))
    ring_seen = [any(close3(e, k) for e, pos in draws) for k in ey["ring"]]
    # the rotation sense: the ring set and the wrestler's x are symmetric, so the SIGN of the wrestler eye's z residue is the only trace of it in the stream: z' = z cos - x sin with theta = -pi gives a tiny
    # NEGATIVE z (the stream's -1.7e-5; ours -3.9e-5, the library's sin differs from the game's series in magnitude, not in sign). A reversed rotation gives a positive one.
    sz = next((e[2] for e, pos in draws if close3(e, ey["wrestler"])), 0.0)
    sign_ok = ey["wrestler"][2] * sz > 0
    ok = rc == 0 and len(draws) > 0 and bad == 0 and seen_w > 0 and all(ring_seen) and sign_ok
    print(f"frame {frame}: PLANE EYE MODULE ({label}): reader eyes wrestler {ey['wrestler']} ring {ey['ring']} planepos y {ey['pos_y']}; {len(draws)} mask draws, wrestler eye on {seen_w}, ring eyes seen {ring_seen}, {bad} draws with an eye or PlanePos the reader did not give, z-residue sign reader {ey['wrestler'][2]:.2e} stream {sz:.2e} {'same' if sign_ok else 'DIFFERENT'}: {'PASS' if ok else 'FAIL'}")
    return ok


def check_eye_module_plants(capture, frame, out):
    ok = True
    for name, n in (("eye_no_pi", 1), ("eye_rot_sign", 2), ("eye_degrees", 4)):
        good = check_eye_module(capture, frame, out, ["--eye-plant", str(n)], "plant " + name)
        print(f"  plant {name}: {'DID NOT FAIL: the check is blind to it' if good else 'FAILS its bar (as it must)'}")
        ok = ok and not good
    return ok


def check_plane_plants(capture, frame, out, rows, base):
    g, o, s_iou, mean_d, told = base
    ok = True
    for fault in PLANTS["plane"]:
        rc, c2, img2 = ours(capture, frame, out, fault, told)
        o2 = img2.get("plane")
        i2, d2, cov2 = compare_footprint(g, o2) if o2 is not None else (0.0, 255.0, 0)
        made2 = num(c2, r"plane made (\d+)")
        good2, prec2 = plane_good(g, o2, rc, num(c2, r"validation errors: (\d+)"), made2, cov2, d2)
        failed = not good2   # the SAME predicate as the clean run
        print(f"  plant {fault}: precision {prec2:.3f} mean|d| {d2:.1f} -> {'FAILS its bar (as it must)' if failed else 'DID NOT FAIL: the check is blind to it'}")
        ok = ok and failed
        if o2 is not None:
            rows.append((f"{frame} plane PLANT {fault}", g, o2, i2))
    return ok


PL_MEAN_D = 4.0
PLANTS["prelight"] = ["prelight_wrong_light", "prelight_no_faces"]


def game_prelight_and_told(capture, frame, out):
    ops = ops_of(capture, frame, out)
    ps = [p for p in passes_of(ops) if any(d.get("effect") == "yPreLight" for d in p["draws"])]
    if not ps:
        raise SystemExit(f"frame {frame}: no pre-light pass in the stream")
    told, strips = os.path.join(out, f"told_prelight_f{frame}.txt"), []
    with open(told, "w") as f:
        for i, p in enumerate(ps):
            d = next(x for x in p["draws"] if x.get("effect") == "yPreLight")
            c = consts(draw_case(capture, frame, p, d["op"], out))
            f.write("prelight_slot " + f4([i] + c["DifLghVec"] + c["DifLghCol"] + c["DifLghMaxD"] + c["DifLghPrm"] + c["DifLghMaxA"] + c["SssLobeA"] + c["SssLobeB"]) + BS)
            strips.append(surface_after(capture, frame, p))
    return strips, told


def mean_diff(a, b):
    return float(np.abs(np.asarray(a).astype(int) - np.asarray(b).astype(int)).mean())


def check_prelight(capture, frame, out, rows, ok_all):
    gs, told = game_prelight_and_told(capture, frame, out)
    rc, counts, img = ours(capture, frame, out, "", told)
    made = num(counts, r"pre-light made (\d+)")
    val = num(counts, r"validation errors: (\d+)") + (0 if num(counts, r"validation layer active: (\d+)") else 1000)   # layer not active = a failure, never a silent pass
    ds = []
    for i, g in enumerate(gs):
        o = img.get(f"prelight{i}")
        ds.append(mean_diff(g.resize((384, 64)), o.resize((384, 64))) if o is not None else 255.0)
        rows.append((f"{frame} prelight slot {i}", g.resize((384, 64)).resize((512, 85)), o.resize((512, 85)) if o is not None else None, max(0.0, 1 - ds[-1] / 255)))
    good = rc == 0 and val == 0 and made > 0 and len(ds) == len(gs) and max(ds) <= PL_MEAN_D
    print(f"frame {frame}: PRE-LIGHT game slots {len(gs)}; ours made {made}; mean|d| per slot {', '.join(f'{x:.2f}' for x in ds)} (bar {PL_MEAN_D}); validation {val}; {'PASS' if good else 'FAIL'}")
    return good, (gs, told, max(ds))


def check_prelight_plants(capture, frame, out, rows, base):
    gs, told, worst = base
    ok = True
    for fault in PLANTS["prelight"]:
        rc, c2, img2 = ours(capture, frame, out, fault, told)
        d2 = [mean_diff(g.resize((384, 64)), img2[f"prelight{i}"].resize((384, 64))) if f"prelight{i}" in img2 else 255.0 for i, g in enumerate(gs)]
        failed = max(d2) > PL_MEAN_D
        print(f"  plant {fault}: worst mean|d| {max(d2):.2f} -> {'FAILS its bar (as it must)' if failed else 'DID NOT FAIL: the check is blind to it'}")
        ok = ok and failed
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", default=os.path.join(MAIN, r"out\d3caster\z7147\capture"))
    ap.add_argument("--frames", default="20,100,180")
    ap.add_argument("--out", default=os.path.join(MAIN, r"out\s103\E2"))
    ap.add_argument("--faults", action="store_true")
    ap.add_argument("--maps", default="rim,plane,prelight")
    ap.add_argument("--plane-frames", default="240,280,320", help="frames where the game's plane chain holds the wrestlers (see check_plane)")
    ap.add_argument("--module-eye", action="store_true", help="S103 E5: the plane eye / PlanePos come from the latch reader (plane_eye_read.h) over a fixture plane module, recorded as PlaneEyes; the stream's EyePos must equal them")
    ap.add_argument("--const-eye", action="store_true", help="S103 E4: do not tell the plane eye / plane position: the hook constants make the map (plant plane_eye_ring must fail)")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    frames = [int(x) for x in a.frames.split(",")]
    maps = a.maps.split(",")
    rows, ok = [], True
    plane_frames = [int(x) for x in a.plane_frames.split(",")]
    for m in maps:
        for f in (plane_frames if m == "plane" else frames):
            fr = plane_frames if m == "plane" else frames
            good, base = {"rim": check_rim, "plane": check_plane, "prelight": check_prelight}[m](a.capture, f, a.out, rows, ok)
            ok = ok and good
            if m == "plane" and CONST_EYE and f == fr[len(fr) // 2]:
                ok = ok and check_eye_exact(a.capture, f, a.out)
            if m == "plane" and MODULE_EYE and f == fr[len(fr) // 2]:   # S103 E5
                ok = ok and check_eye_module(a.capture, f, a.out)
                if a.faults:
                    ok = ok and check_eye_module_plants(a.capture, f, a.out)
            if a.faults and f == fr[len(fr) // 2]:
                ok = ok and {"rim": check_rim_plants, "plane": check_plane_plants, "prelight": check_prelight_plants}[m](a.capture, f, a.out, rows, base)
    W = 512
    sheet = Image.new("RGB", (W * 3, (256 + 14) * len(rows)), (40, 40, 40))
    for r, (name, g, o, s_iou) in enumerate(rows):
        y = r * (256 + 14)
        if o is None:
            continue
        tiles = [label(g, f"frame {name}: GAME map"), label(o, f"OURS (IoU {s_iou:.2f})"), label(diff_heat(g, o), "difference x3")]
        for i, t in enumerate(tiles):
            sheet.paste(t.resize((W, 256)), (i * W, y))
    path = os.path.join(a.out, "sheet_maps.png")
    sheet.save(path)
    print("sheet:", path)
    print("RESULT:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":   # S103 L7: importable (l7_check.py uses its fview helpers)
    main()
