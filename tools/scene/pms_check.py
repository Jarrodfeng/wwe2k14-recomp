"""Lane PMS (Session 103) offline check of the props map the shadowed FLOOR reads (docs\\research\\s103\\PMF.md section 4, slice 1). 0 game runs.

  py -I tools\\scene\\pms_check.py [--capture DIR] [--frames 20,100,180] [--out DIR] [--faults] [--require-floor]

Per frame (fview headless + maps_draw.exe, as e_check.py):
 - MATRIX: our props map's floor TexProjLight (MapRole::PropsMap matrix, VS c68-c71 register layout) against EVERY stream floor draw (yBG_* shadow readers, slot 12 = the copy of the props map) of the stream frame F+1 (--stream-offset):
   max relative error <= MATRIX_ERR (1e-5). The reversed multiplication order's error is printed beside it (it must be far above the bar: the order is settled by this check, not assumed).
 - PRM: our ShadowPrm rows (PS c168-c170) equal the stream's floor draws' (float32 equal).
 - PICTURE: blob footprint (blue < 250) IoU of our props map against the game's props map after its pass >= CIRCLE_IOU (0.88, lane E's bar).
 - validation layer active, 0 errors.
Planted faults (--faults), each must FAIL its bar: props_reader_mapb (the old binding: the floor gets the wrestlers' light = the S98 'long stretched shadows on the floor'), props_bias_order (the other order of the
product), props_no_blobs (the blobs in their own image again: no props map for the floor). A fault that does not fail = the check is blind to it = FAIL.
Nothing from the game is written into the repo: pictures under out\\s103\\PMS (scratch, gitignored).
"""
import argparse, json, os, re, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw

MAIN = r"C:\Users\Jarrod\wwe2k14-recomp"
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
FVIEW = os.path.join(MAIN, r"out\renderer\dev\fview.exe")
MATRIX_ERR, CIRCLE_IOU = 1e-5, 0.88
WRESTLERS = ["--wrestler", "0=343", "--wrestler", "6=324"]
BONES_BACK = 1
sys.path.insert(0, os.path.join(ROOT, "tools", "shadow"))
sys.path.insert(0, HERE)   # S103 RML: rml_check.parse_report (the rig's resolved-constant lines)


def run(cmd, env=None, timeout=300):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
    return p.returncode, p.stdout + p.stderr


def game_props(capture, frame, out):
    """The game's props map of `frame` after its pass (fview picture.png), as e_check.py game_maps."""
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
    if not props:
        raise SystemExit(f"frame {frame}: no props pass in the stream")
    rc, text = run([FVIEW, "--capture", capture, "--frame", str(frame), "--surface", str(props["surface"]), "--through-op", str(props["last"]), "--save-case", "--op", str(props["last"]), "--root", ROOT, "--game", os.path.join(MAIN, r"game_data\extracted")], env)
    line = [l for l in text.splitlines() if "case saved:" in l]
    if not line:
        raise SystemExit("fview --save-case failed: " + text[-300:])
    return Image.open(os.path.join(line[-1].split("case saved:")[1].strip(), "picture.png")).convert("RGB"), props["tech"].get("T_CircleShadow", 0)


def stream_floor(cap, fi):
    """The stream's floor draws that read the props map at slot 12: [(effect, c68-c71 (4x4), PS c168-c170 (3x4), VS c168-c169 (2x4): S103 RML)], from the colour draws of frame `fi`."""
    import shadow_census as sc
    f = cap.frame(fi)
    d = f.draws
    o = f.origins
    ps, kl = (o[k].astype(np.int64) for k in ("pass_site", "klass"))
    cs = sc.pcap.queue_site(o)
    vs_eff = cap.effs(d["vs_hash"])
    copies = sc.table(f, sc.SEC_COPIES, sc.COPY_DT)
    # the props pass = the pass holding a circle call; its copy destinations are what the floor reads (shadow_census.py label_of 'props_map')
    dest = set()
    for p in sc.walk(f, copies):
        if p.draws and (cs[np.array(p.draws)] == sc.CALL_CIRCLE).any():
            for ci in p.copies:
                dest.add(int(copies[ci]["d"]["image"]))
    tf, tc = d["tex_first"].astype(np.int64), d["tex_count"].astype(np.int64)
    r_prm = sc.FXR.get(("yBG_DefaultLM", "fragment", "ShadowPrm"), sc.R_SHADOWPRM)
    out = []
    ci = np.nonzero(((ps == sc.SITE_COLOUR) | (ps == sc.SITE_MIRROR)) & (kl == sc.pcap.K_MODEL_PART))[0]
    for i in ci:
        e = vs_eff[i]
        if not e.startswith("yBG_"):
            continue
        sl = [int(s["image"]) for s in f.slots[int(tf[i]):int(tf[i]) + int(tc[i])] if int(s["slot"]) == 12]
        if not sl or sl[0] not in dest:
            continue
        vb, pb = f.bank(int(d[i]["vs_bank"])), f.bank(int(d[i]["ps_bank"]))
        out.append((e, sc.regs(vb, sc.R_FLOOR_TEX, 4).astype(np.float64), sc.regs(pb, r_prm, 3).astype(np.float32), sc.regs(vb, 168, 2).astype(np.float32)))
    return out, len(dest)


def ours(capture, frame, out, faults=""):
    d = os.path.join(out, f"ours_f{frame}" + (f"_{faults}" if faults else ""))
    cmd = [os.path.join(ROOT, r"out\tools\maps_draw.exe"), capture, "--frame", str(frame), "--game", os.path.join(MAIN, r"game_data\extracted"), "--out", d] + WRESTLERS
    if BONES_BACK:
        cmd += ["--bones-frame", str(frame - BONES_BACK)]
    if faults:
        cmd += ["--fault", faults]
    rc, text = run(cmd, dict(os.environ, WWE_RDR_VALIDATION="1"))
    cp = os.path.join(d, "counts.txt")
    counts = open(cp).read() if os.path.exists(cp) else text
    props = None
    pp = os.path.join(d, "props.txt")
    if os.path.exists(pp):
        kv = {}
        for line in open(pp):
            w = line.split()
            if w:
                kv[w[0]] = w[1:]
        if kv.get("props_present") == ["1"]:
            props = (np.array(kv["props_matrix"], dtype=np.float64).reshape(4, 4), np.array(kv["props_prm"], dtype=np.float32).reshape(3, 4))
    ip = os.path.join(d, "ours_props.png")
    img = Image.open(ip).convert("RGB") if os.path.exists(ip) else None
    return rc, counts, props, img


def footprint_blue(img):
    return np.asarray(img).astype(int)[..., 2] < 250


def iou(a, b):
    u = (a | b).sum()
    return float((a & b).sum()) / u if u else 1.0


def mat_err(ours_m, got):
    scale = max(float(np.abs(got).max()), 1e-9)
    return float(np.abs(ours_m - got).max() / scale)


def label(img, text):
    im = img.copy()
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, im.width, 14], fill=(0, 0, 0))
    d.text((3, 2), text, fill=(255, 255, 255))
    return im


def judge(capture_cap, fi, floors, props, img, gprops, counts):
    """(ok, text, matrix_err_max, prm_equal, iou) of one arm against the stream's floor draws and the game's props map."""
    val = int(re.search(r"validation errors: (\d+)", counts).group(1)) if re.search(r"validation errors: (\d+)", counts) else -1
    act = int(re.search(r"validation layer active: (\d+)", counts).group(1)) if re.search(r"validation layer active: (\d+)", counts) else 0
    if props is None:
        m_err, prm_eq = 9.9, False
    else:
        m_err = max((mat_err(props[0], g[1]) for g in floors), default=9.9)
        prm_eq = bool(floors) and all(np.array_equal(props[1], g[2]) for g in floors)
    c_iou = iou(footprint_blue(gprops), footprint_blue(img)) if img is not None else 0.0
    ok = props is not None and bool(floors) and m_err <= MATRIX_ERR and prm_eq and c_iou >= CIRCLE_IOU and val == 0 and act == 1
    return ok, m_err, prm_eq, c_iou, val, act


RIG_LINKS = ["--link", "actor:slot:0=343:0", "--link", "actor:slot:6=324:0", "--body-scale-one"]   # (body scale 1: a rig-only fixture, the wrestlers are not what this check judges) z7147's two wrestlers, TOLD (the record names no model file), as e_check's WRESTLERS


def rig_arm(capture, frame, out, props_map_on=True, maps_fault="", plant_vs_prm=False):
    """One frame through a REAL front end (frame_rig.exe: record_frame + FrontEnd::RenderToPixels with the maps hook attached, as the live drawer does), 0 game runs. Returns the parsed `pms_floor` line + validation."""
    d = os.path.join(out, f"rig_f{frame}" + ("" if props_map_on else "_off") + (f"_{maps_fault}" if maps_fault else "") + ("_vsprm" if plant_vs_prm else ""))
    os.makedirs(d, exist_ok=True)
    env = dict(os.environ, WWE_RDR_VALIDATION="1", WWE_RDR_PROPS_MAP="1" if props_map_on else "0", CLAUDECODE="1")
    if plant_vs_prm:
        env["WWE_RDR_PLANT_VS_PRM_PS"] = "1"   # S103 RML planted twin: the vertex ShadowPrm block gets the pixel rows (the old Resolve)
    cmd = ["py", "-I", os.path.join(ROOT, "tools", "scene", "frame_rig.py"), "--capture", capture, "--frames", str(frame), "--out", d, "--no-sheet", "--repeat", "1"] + RIG_LINKS
    if maps_fault:
        cmd += ["--maps-fault", maps_fault]
    rc, text = run(cmd, env, timeout=900)
    rep = os.path.join(d, f"frame_{frame:03d}_report.txt")
    body = open(rep).read() if os.path.exists(rep) else text
    m = re.search(r"pms_floor props_map (\d) props_bound (\d+) d1_shadow_nomap (\d+) sites (\d+) (\d+) (\d+) (\d+) (\d+) (\d+) slot12_is_props_image (\d) slot12_is_shadow_image (\d) props_holes (.*)", body)
    v = re.search(r"validation_errors (\d+)", body)
    r = {"rc": rc, "text": body, "validation": int(v.group(1)) if v else -1}
    if m:
        g = m.groups()
        r.update(props_map=int(g[0]), props_bound=int(g[1]), nomap=int(g[2]), sites=[int(x) for x in g[3:9]], slot12_props=int(g[9]), slot12_shadow=int(g[10]), holes=g[11].strip())
    return r


def rig_ok(r):
    return bool("props_map" in r and r["props_map"] == 1 and r["props_bound"] > 0 and r["nomap"] == 0 and all(s > 0 for s in r["sites"]) and r["slot12_props"] == 1 and r["slot12_shadow"] == 0 and r["validation"] == 0)


def vs_prm_judge(rig, floors):
    """S103 RML (RMP.md 3): the VERTEX ShadowPrm block of ours' floor receivers (resolved in the real front end, rml_check.parse_report) against the stream floor draws' VS c168-c169: float32-equal."""
    import rml_check
    ours = [d["fields"].get(("vs", "ShadowPrm")) for d in rml_check.parse_report(rig["text"])["draws"].values() if d["kind"] == "floor"]
    if not ours or not floors:
        return False, f"no resolved floor block ({len(ours)}) or no stream floor draw ({len(floors)})"
    bad = sum(1 for o in ours for g in floors if o is None or not np.array_equal(o.astype(np.float32), g[3]))
    return bad == 0, f"{len(ours)} resolved floor block(s) x {len(floors)} stream draw(s): {bad} unequal; ours {None if ours[0] is None else ours[0].tolist()} stream {floors[0][3].tolist()}"


def rig_main(a):
    frames = [int(x) for x in a.frames.split(",")]
    ok_all = True
    import shadow_census as sc
    cap = sc.Cap(a.capture)
    for fi in frames:
        on = rig_arm(a.capture, fi, a.out)
        ok = rig_ok(on)
        print(f"rig frame {fi}: props map {on.get('props_map')} floor draws bound to it {on.get('props_bound')} (no-map {on.get('nomap')}); the six re-pointed sites ran {on.get('sites')}; slot 12 = the props image {on.get('slot12_props')} (= map B's image {on.get('slot12_shadow')}); validation errors {on['validation']}; rc {on['rc']} -> {'PASS' if ok else 'FAIL'}")
        ok_all = ok_all and ok
        floors, _nd = stream_floor(cap, fi + a.stream_offset)   # S103 RML: the vertex ShadowPrm registers of the matrix proof (the receivers get (0, 0, 0, 1) x 2, never the pixel rows)
        vok, vtxt = vs_prm_judge(on, floors)
        print(f"  VS ShadowPrm c168-c169 (stream frame {fi + a.stream_offset}): {vtxt} -> {'PASS' if vok else 'FAIL'}")
        ok_all = ok_all and vok
        if fi == frames[len(frames) // 2]:
            vp = rig_arm(a.capture, fi, a.out, plant_vs_prm=True)
            vpok, vptxt = vs_prm_judge(vp, floors)
            print(f"  plant vs_prm_ps_rows (the vertex block fed the pixel rows): {vptxt} -> {'DID NOT FAIL: the check is blind to it' if vpok else 'FAILS its bar (as it must)'}")
            ok_all = ok_all and not vpok
            off = rig_arm(a.capture, fi, a.out, props_map_on=False)   # the switch off: the old binding, the check must see the difference
            offok = rig_ok(off)
            print(f"  control switch OFF: props_bound {off.get('props_bound')} sites {off.get('sites')} slot 12 = props image {off.get('slot12_props')} -> {'DID NOT FAIL: the check is blind to the switch' if offok else 'FAILS the bar (as it must)'}")
            ok_all = ok_all and not offok
            nb = rig_arm(a.capture, fi, a.out, maps_fault="props_no_blobs")
            nbok = rig_ok(nb)
            print(f"  plant props_no_blobs: props map {nb.get('props_map')} floor draws bound {nb.get('props_bound')} (no-map {nb.get('nomap')}) -> {'DID NOT FAIL: the check is blind to it' if nbok else 'FAILS its bar (as it must)'}")
            ok_all = ok_all and not nbok
    print("RIG RESULT:", "PASS" if ok_all else "FAIL")
    sys.exit(0 if ok_all else 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rig", action="store_true", help="the REAL front end (frame_rig.exe) on the capture: the floor bound to the props map, the six re-pointed sites run, slot 12 = the props image, validation 0; no fview, no props-map picture compare")
    ap.add_argument("--capture", default=os.path.join(MAIN, r"out\d3caster\z7147\capture"))
    ap.add_argument("--frames", default="20,100,180")
    ap.add_argument("--out", default=os.path.join(MAIN, r"out\s103\PMS"))
    ap.add_argument("--faults", action="store_true")
    ap.add_argument("--stream-offset", type=int, default=1, help="the stream frame whose floor / props map record F is compared with: F+1 (the record's circle rows are stamped with the guest frame the game ran the props light in, which the stream shows one frame later: F+0 gives 5e-4, F+1 5.6e-10, F-1 9e-4, F+2 5e-4 on z7147 f100)")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    if a.rig:
        rig_main(a)
    import shadow_census as sc
    cap = sc.Cap(a.capture)
    frames = [int(x) for x in a.frames.split(",")]
    rows, ok_all = [], True
    for fi in frames:
        floors, ndest = stream_floor(cap, fi + a.stream_offset)
        gprops, g_circles = game_props(a.capture, fi + a.stream_offset, a.out)
        rc, counts, props, img = ours(a.capture, fi, a.out)
        ok, m_err, prm_eq, c_iou, val, act = judge(cap, fi, floors, props, img, gprops, counts)
        # the reversed order's number, for the record: the bias on the components of each register (props_bias_order's matrix) against the same stream draws
        rc2, c2, p2, _ = ours(a.capture, fi, a.out, "props_bias_order")
        rev_err = max((mat_err(p2[0], g[1]) for g in floors), default=9.9) if p2 is not None else 9.9
        eff = sorted({g[0] for g in floors})
        print(f"frame {fi}: stream floor draws reading the props map {len(floors)} ({', '.join(eff) or 'none'}); props dest copies {ndest}; matrix max rel err {m_err:.2e} (bar {MATRIX_ERR:.0e}; reversed order {rev_err:.2e}); "
              f"ShadowPrm equal {prm_eq}; blob IoU {c_iou:.3f} (bar {CIRCLE_IOU}); validation active {act} errors {val}; exit {rc}; {'PASS' if ok else 'FAIL'}")
        if props is not None and floors:
            print(f"   ours c168-170 {props[1].round(4).tolist()}  stream {floors[0][2].round(4).tolist()}")
        ok_all = ok_all and ok
        rows.append((fi, gprops, img))
        if a.faults and fi == frames[len(frames) // 2]:
            for fault in ("props_reader_mapb", "props_bias_order", "props_no_blobs"):
                rcf, cf, pf, imf = ours(a.capture, fi, a.out, fault)
                okf, mf, peq, cif, valf, actf = judge(cap, fi, floors, pf, imf, gprops, cf)
                print(f"  plant {fault}: props map handed over {pf is not None}; matrix err {mf:.2e}; ShadowPrm equal {peq}; blob IoU {cif:.3f} -> {'DID NOT FAIL: the check is blind to it' if okf else 'FAILS its bar (as it must)'}")
                ok_all = ok_all and not okf
                rows.append((f"{fi} PLANT {fault}", gprops, imf))
    W = 512
    sheet = Image.new("RGB", (W * 2, (W + 14) * len(rows)), (40, 40, 40))
    for r, (fi, gprops, img) in enumerate(rows):
        y = r * (W + 14)
        sheet.paste(label(gprops, f"frame {fi}: GAME props map (blue = blobs)").resize((W, W)), (0, y))
        sheet.paste(label(img if img is not None else Image.new("RGB", (W, W), (90, 0, 0)), "OURS props map" if img is not None else "OURS: none handed over").resize((W, W)), (W, y))
    path = os.path.join(a.out, "sheet_props.png")
    sheet.save(path)
    print("sheet:", path)
    print("RESULT:", "PASS" if ok_all else "FAIL")
    sys.exit(0 if ok_all else 1)


if __name__ == "__main__":
    main()
