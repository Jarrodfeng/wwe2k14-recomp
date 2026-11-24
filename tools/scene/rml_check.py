"""Lane RML (Session 103) offline check of the ring mat's props-map pass (docs\\research\\s103\\RMP.md sections 1 and 3). 0 game runs.

  py -I tools\\scene\\rml_check.py [--wm30 DIR] [--capture DIR --frame N] [--stream-offset N] [--out DIR] [--faults]

REFERENCE (stream only, wm30 = out\\captures\\s100_wm30_300 frames 0 / 1: a 3-wrestler match, so the game built no plane module): the game's ring-mat draw (op 348, sub_82608CB0 pass 2 / 3) is
yProp_PSM skinned_shadow_light + light_shadow and its VS c68-c71 / c164-c166 / c168-c169 and PS c168-c170 are BIT-EQUAL to every floor receiver's of the same frame; VS c168-c169 = (0, 0, 0, 1) x 2 [C RMP.md 1.1 / 1.2].
wm30 has no scene records (the rig cannot draw it), so OURS runs on a capture that has them (z7147 f100: the props map exists) with the rig told the game built no plane module, and the floor receivers' constants of
that stream frame (F + --stream-offset, as pms_check.py) are the mat's reference (the equality above is what makes them one). The REAL front end (frame_rig.exe: record_frame + FrontEnd::RenderToPixels, maps hook attached):
 - PASS: ours binds the mat to the light_shadow fragment + a shadow_light vertex (the game's is the skinned twin: the mat is a rigid mesh here, its palette is the identity [L]); props_mat_bound > 0, plane_wanted_plain 0.
 - SLOT 12: the mat's slot 12 is the props map's image, never map B's.
 - CONSTANTS: the mat's RESOLVED VS c68-c71 (TexProjLight), c166 (ViewLight row 2, the only row the vertex stage reads: fx_prop.glsl `ViewLight[2]`; rows 0-1 are printed, not judged), c168-c169
   (ShadowPrm: (0, 0, 0, 1) x 2) and PS c168-c170 (ShadowPrm) against the stream's: max relative error <= 1e-5.
 - validation layer: 0 errors.
Planted faults (--faults), each must FAIL: mat_plain_when_no_module (today's binding: the plain variant, no shadow at all: the past behaviour re-found), mat_reads_mapb (slot 12 = the wrestlers' map B),
mat_vs_prm_ps_rows (the vertex ShadowPrm block fed the pixel rows: the face-away term 0.2 x (1 - N.L) instead of 0). Controls that must pass: the module present (not told it is absent): never light_shadow
(check B keeps screen_shadow); the switch wwe2k14_rdr_props_map off: never light_shadow.
Nothing from the game is written into the repo: reports under out\\s103\\RML (scratch, gitignored).
"""
import argparse, os, re, subprocess, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
MAIN = r"C:\Users\Jarrod\wwe2k14-recomp"
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools", "shadow"))
ERR = 1e-5
RIG_LINKS = ["--link", "actor:slot:0=343:0", "--link", "actor:slot:6=324:0", "--body-scale-one"]   # z7147's two wrestlers, TOLD (the record names no model file), as pms_check.py; the wrestlers are not what this check judges
FAULTS = {"mat_plain_when_no_module": "WWE_RDR_PLANT_MAT_PLAIN", "mat_reads_mapb": "WWE_RDR_PLANT_MAT_MAPB", "mat_vs_prm_ps_rows": "WWE_RDR_PLANT_VS_PRM_PS"}


def run(cmd, env=None, timeout=900):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
    return p.returncode, p.stdout + p.stderr


def stream_draws(cap, fi, want_floor=False):
    """The stream's ring-mat draws of frame `fi` (vs effect yProp_PSM / yProp_PSMLM; with want_floor the yBG_* shadow readers instead): [dict(vs, ps, vc68 (4x4), vc164 (3x4), vc168 (2x4), pc168 (3x4))]."""
    import shadow_census as sc
    f = cap.frame(fi)
    d = f.draws
    o = f.origins   # None on an older capture (wm30 has no origins): the effect names alone pick the draws then
    kl = o["klass"].astype(np.int64) if o is not None else None
    vs_eff, ps_eff = cap.effs(d["vs_hash"]), cap.effs(d["ps_hash"])
    out = []
    for i in (np.nonzero(kl == sc.pcap.K_MODEL_PART)[0] if kl is not None else range(len(vs_eff))):
        e = vs_eff[i]
        if want_floor != e.startswith("yBG_") or (not want_floor and not e.startswith("yProp_PSM")):
            continue
        if "shadow" not in e:
            continue
        vb, pb = f.bank(int(d[i]["vs_bank"])), f.bank(int(d[i]["ps_bank"]))
        out.append({"vs": e, "ps": ps_eff[i], "vc68": sc.regs(vb, 68, 4).astype(np.float64), "vc164": sc.regs(vb, 164, 3).astype(np.float64), "vc168": sc.regs(vb, 168, 2).astype(np.float64), "pc168": sc.regs(pb, 168, 3).astype(np.float64)})
    return out


def variant_name(stream_name, stage):
    return stream_name.replace("/", "_") + ("_vertex" if stage == "vs" else "_fragment")


def rig_arm(capture, frame, out, fault="", extra_env=None, plane_off=True):
    """One frame through the real front end; returns (rc, report text, parsed ours dict)."""
    d = os.path.join(out, f"rig_f{frame}" + (f"_{fault}" if fault else ""))
    os.makedirs(d, exist_ok=True)
    env = dict(os.environ, WWE_RDR_VALIDATION="1", WWE_RDR_PROPS_MAP="1", CLAUDECODE="1", WWE_FVIEW_ALLOW_STALE="1")   # (the game's picture frame_rig.py also makes is not read here: only the rig's report lines)
    if plane_off:
        env["WWE_RDR_PLANE_GAME_OFF"] = "1"
    if fault:
        env[FAULTS[fault]] = "1"
    env.update(extra_env or {})
    cmd = ["py", "-I", os.path.join(ROOT, "tools", "scene", "frame_rig.py"), "--capture", capture, "--frames", str(frame), "--out", d, "--no-sheet", "--repeat", "1"] + RIG_LINKS
    rc, text = run(cmd, env)
    rep = os.path.join(d, f"frame_{frame:03d}_report.txt")
    body = open(rep).read() if os.path.exists(rep) else text
    return rc, body, parse_report(body)


def parse_report(body):
    r = {"draws": {}, "census": None, "validation": -1}
    m = re.search(r"rml_census props_mat_bound (\d+) plane_bound (\d+) plane_wanted_plain (\d+) plane_game_off (\d) probes (\d+) viewer_space (\d)", body)
    if m:
        r["census"] = tuple(int(x) for x in m.groups())
    v = re.search(r"validation_errors (\d+)", body)
    r["validation"] = int(v.group(1)) if v else -1
    for m in re.finditer(r"rml_draw (\d+) kind (\w+) vs (\S+) ps (\S+) slot12_is_props_image (\d) slot12_is_shadow_image (\d)", body):
        r["draws"][int(m.group(1))] = {"kind": m.group(2), "vs": m.group(3), "ps": m.group(4), "slot12_props": int(m.group(5)), "slot12_mapb": int(m.group(6)), "fields": {}}
    for m in re.finditer(r"rml_field (\d+) (vs|ps) (\S+) (\d+)((?: \S+)+)", body):
        i = int(m.group(1))
        if i in r["draws"]:
            r["draws"][i]["fields"][(m.group(2), m.group(3))] = np.array([float(x) for x in m.group(5).split()], dtype=np.float64).reshape(int(m.group(4)), 4)
    return r


def err(a, b):
    """max |a - b| / max |b| (the stream is the reference), 9.9 when the shapes differ or a side is missing."""
    if a is None or b is None or a.shape != b.shape:
        return 9.9
    return float(np.abs(a - b).max() / max(float(np.abs(b).max()), 1e-9))


def judge(ours, floors):
    """(ok, text) of one arm: ours' props-pass mat draws against the stream's floor receivers of the same stream frame (the constants the mat reads: reference() proves they are the game's mat's)."""
    c = ours["census"]
    props = [d for d in ours["draws"].values() if d["kind"] == "props"]
    if not floors:
        return False, "no floor receiver in the stream frame (the check has no reference)"
    if c is None or not props or c[0] == 0:
        return False, f"ours bound NO draw to the props-map pass (census {c}): the mat is drawn plain, no shadow"
    if c[2] != 0:
        return False, f"{c[2]} ring-mat draw(s) still counted plane_wanted_plain"
    if ours["validation"] != 0:
        return False, f"validation errors {ours['validation']}"
    d = props[0]
    if c[5]:   # the viewer's space is the game's with y negated: the matrix takes the viewer's y (column 1 of TexProjLight and of the light row), as the floor's does (front_end.cpp shadow_bg_m); back to the game's space for the compare
        d = dict(d, fields=dict(d["fields"]))
        for k in (("vs", "TexProjLight"), ("vs", "ViewLight")):
            if d["fields"].get(k) is not None:
                m = d["fields"][k].copy()
                m[:, 1] = -m[:, 1]
                d["fields"][k] = m
    if not (d["vs"].startswith("yProp_PSM") and d["vs"].endswith("shadow_light_vertex") and d["ps"] == "yProp_PSM_light_shadow_fragment"):
        return False, f"ours' pass is {d['vs']} + {d['ps']}, not the game's skinned_shadow_light + light_shadow family (a rigid twin is accepted: the mat is a rigid mesh here, its palette is the identity)"
    best = None
    for g in floors:
        e = {
            "TexProjLight c68-71": err(d["fields"].get(("vs", "TexProjLight")), g["vc68"]),
            "ViewLight c166": err(None if d["fields"].get(("vs", "ViewLight")) is None else d["fields"][("vs", "ViewLight")][2:3], g["vc164"][2:3]),
            "VS ShadowPrm c168-169": err(d["fields"].get(("vs", "ShadowPrm")), g["vc168"]),
            "PS ShadowPrm c168-170": err(d["fields"].get(("ps", "ShadowPrm")), g["pc168"]),
        }
        m = max(e.values())
        if best is None or m < best[0]:
            best = (m, e, g)
    m, e, g = best
    vl = d["fields"].get(("vs", "ViewLight"))
    txt = f"pass {d['vs']} + {d['ps']}; " + "; ".join(f"{k} {v:.2e}" for k, v in e.items()) + f"; ViewLight rows 0-1 (not read, not judged): ours {'none' if vl is None else vl[:2].round(3).tolist()} stream {g['vc164'][:2].round(3).tolist()}"
    if not d["slot12_props"] or d["slot12_mapb"]:
        return False, txt + f"; slot 12 is the props image {d['slot12_props']}, map B's image {d['slot12_mapb']}"
    return m <= ERR, txt + f"; slot 12 = the props image; worst {m:.2e} (bar {ERR:.0e})"


def reference(cap, frames):
    """The reference itself, stream only (RMP.md 1.1 / 1.2 [C]): on the wm30 frames the game's ring-mat draw (op 348) is yProp_PSM skinned_shadow_light + light_shadow and its VS c68-c71 / c164-c166 / c168-c169 and PS c168-c170
    are BIT-EQUAL to every floor receiver's of the same frame. That is what lets the check below (a capture with a scene record: wm30 has none) take the floor receivers' constants as the mat's reference."""
    ok = True
    for fi in frames:
        mats = stream_draws(cap, fi)
        floors = stream_draws(cap, fi, want_floor=True)
        names = sorted({(g["vs"], g["ps"]) for g in mats})
        same = bool(mats) and bool(floors) and all(np.array_equal(m[k].astype(np.float32), g[k].astype(np.float32)) for m in mats for g in floors for k in ("vc68", "vc164", "vc168", "pc168"))
        names_ok = names == [("yProp_PSM/skinned_shadow_light", "yProp_PSM/light_shadow")]
        vs_prm = all(np.array_equal(m["vc168"], np.array([[0, 0, 0, 1], [0, 0, 0, 1]], dtype=np.float64)) for m in mats)
        print(f"reference wm30 stream frame {fi}: {len(mats)} ring-mat draw(s) {names}; {len(floors)} floor receiver(s); mat constants bit-equal to every floor receiver's {same}; VS c168-169 = (0, 0, 0, 1) x 2 {vs_prm} -> {'PASS' if same and names_ok and vs_prm else 'FAIL'}")
        ok = ok and same and names_ok and vs_prm
    return ok


def lists_arm(capture, frame, out, plant=False):
    """S103 RML (review F1): the kept draw lists must follow the props map. frame_rig --lists 4 draws the frame four times with the props map's matrix nudged every repeat, once WITH the lists and once without, and
    compares the ring mat's resolved light-pass constants (a stale kept block = the bug: the mat's TexProjLight froze at its first value). Returns (compared, stale, rc)."""
    exe = os.path.join(ROOT, "out", "tools", "frame_rig.exe")
    prefix = os.path.join(out, "lists_f%d%s" % (frame, "_plant" if plant else ""))
    env = dict(os.environ, WWE_RDR_VALIDATION="1", WWE_RDR_PROPS_MAP="1", WWE_RDR_PLANE_GAME_OFF="1", WWE_RDR_RML_LISTS_HOOKS="1", CLAUDECODE="1")
    if plant:
        env["WWE_RDR_PLANT_LIGHT_STAMP"] = "1"
    cmd = [exe, "--synth-camera", "--game", os.path.join(MAIN, r"game_data\extracted"), "--size", "960x540", "--record", os.path.join(capture, "scene", "frame_%06d.rsc" % frame), "--prefix", prefix, "--lists", "4",
           "--link", "actor:slot:0=343:0", "--link", "actor:slot:6=324:0", "--body-scale-one"]
    rc, text = run(cmd, env)
    rep_path = prefix + "_report.txt"
    body = open(rep_path).read() if os.path.exists(rep_path) else text
    m = re.search(r"rml_lists mat_compared (\d+) mat_stale (\d+)", body)
    return (int(m.group(1)), int(m.group(2)), rc) if m else (0, 0, rc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wm30", default=os.path.join(MAIN, r"out\captures\s100_wm30_300"))
    ap.add_argument("--wm30-frames", default="0,1")
    ap.add_argument("--capture", default=os.path.join(MAIN, r"out\d3caster\z7147\capture"), help="a capture WITH scene records (wm30 has none): the rig draws it told the game built no plane module")
    ap.add_argument("--frame", type=int, default=100)
    ap.add_argument("--stream-offset", type=int, default=1, help="the stream frame the record frame F is compared with: F + this (pms_check.py: the circle rows are stamped one guest frame before the stream shows the light)")
    ap.add_argument("--out", default=os.path.join(ROOT, r"out\s103\RML"))
    ap.add_argument("--faults", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    import shadow_census as sc
    ok_all = reference(sc.Cap(a.wm30), [int(x) for x in a.wm30_frames.split(",")])
    cap = sc.Cap(a.capture)
    floors = stream_draws(cap, a.frame + a.stream_offset, want_floor=True)
    rc, body, ours = rig_arm(a.capture, a.frame, a.out)
    ok, txt = judge(ours, floors)
    print(f"ours {os.path.basename(os.path.dirname(a.capture.rstrip(chr(92))))} frame {a.frame} told 'no plane module' (stream {a.frame + a.stream_offset}: {len(floors)} floor receiver(s)): {txt}; rig exit {rc} (1 = the rig's own wrestler-hole verdict on its fixture, not judged here; 2+ = it did not draw) -> {'PASS' if ok and rc in (0, 1) else 'FAIL'}")
    ok_all = ok_all and ok and rc in (0, 1)
    comp, stale, rcl = lists_arm(a.capture, a.frame, a.out)
    lok = comp > 0 and stale == 0 and rcl == 0
    print(f"kept lists (frame_rig --lists 4, the props map's matrix nudged every repeat): the mat's light-pass constants compared {comp} times, stale {stale}, rig exit {rcl} -> {'PASS' if lok else 'FAIL'}")
    ok_all = ok_all and lok
    if a.faults:
        comp, stale, rcl = lists_arm(a.capture, a.frame, a.out, plant=True)
        pok = comp > 0 and stale == 0
        print(f"  plant light_stamp_no_mat (the kept lists' light stamp leaves the mat out: review F1): compared {comp}, stale {stale} -> {'DID NOT FAIL: the check is blind to it' if pok else 'FAILS its bar (as it must)'}")
        ok_all = ok_all and not pok
        for fault in FAULTS:
            rcf, bf, of = rig_arm(a.capture, a.frame, a.out, fault)
            okf, tf = judge(of, floors)
            print(f"  plant {fault}: {tf} -> {'DID NOT FAIL: the check is blind to it' if okf else 'FAILS its bar (as it must)'}")
            ok_all = ok_all and not okf
        rcz, bz, oz = rig_arm(a.capture, a.frame, a.out, plane_off=False)   # the module present (check B's case): never the light pass; screen_shadow when ours made the plane map, plain otherwise
        c = oz["census"]
        zok = c is not None and c[0] == 0
        print(f"  control module present (the rig not told the module is absent): props_mat_bound {c[0] if c else None}, plane_bound {c[1] if c else None}, plane_wanted_plain {c[2] if c else None} -> {'PASS: never light_shadow there' if zok else 'FAIL: the light pass was bound with a plane module present'}")
        ok_all = ok_all and zok
        rcs, bs, os_ = rig_arm(a.capture, a.frame, a.out, extra_env={"WWE_RDR_PROPS_MAP": "0"})   # the switch off: never the light pass
        c = os_["census"]
        sok = c is not None and c[0] == 0
        print(f"  control switch wwe2k14_rdr_props_map OFF: props_mat_bound {c[0] if c else None} -> {'PASS: never light_shadow with the switch off' if sok else 'FAIL'}")
        ok_all = ok_all and sok
    print("RESULT:", "PASS" if ok_all else "FAIL")
    sys.exit(0 if ok_all else 1)


if __name__ == "__main__":
    main()
