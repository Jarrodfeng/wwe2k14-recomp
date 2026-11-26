"""S111 RIGFLOOR offline check (0 game runs): the ring mat's plane shadow under a subject camera (docs\\research\\s111\\RIGFLOOR.md).

The mat reads the plane shadow mask through a SCREEN lookup: fx_prop.glsl FX_PSM_VERT hands the pixel stage (u * clip.w, clip.w), u = the mask's projection
M x world (M = the mat draw's PlaneInv rows; CameraViewProj is the identity there). The divide gives back u only where M's w row IS the drawn camera's clip w row:
the game's own case (one camera). The old rig drew a subject shot with its own camera but kept the game camera's mask, so the boots' shadows slid across the mat
(~60 units at w0bell) and the wrestlers looked lifted off it.

  py -I tools\\scene\\rigfloor_check.py [--exe PATH] [--record PATH] [--game DIR] [--out DIR]

Arms (one frame_rig run each, the real front end with the maps hook): the game camera (control), subject wrestler:0, wrestler:0 from=left, wrestler:1.
Each arm must show, for every rml_draw of kind plane: PlaneInv row 3 == ViewProj row 3 (max abs difference <= 1e-4 x the row's largest magnitude), and the plane
chain made with no blank and no chain-failed frame. The subject arms also print the rig's 'subject plane mask' line (absent = an old rig: FAIL).
On the old rig the three subject arms FAIL (row 3 of PlaneInv is the game camera's). Outputs under --out (scratch, gitignored); nothing is written into the repo.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
MAIN = r"C:\Users\Jarrod\wwe2k14-recomp"
RECORD = os.path.join(MAIN, "out", "exe", "tip-e5600cc95", "captures", "001_p5968", "w0bell.rsc")
ARMS = [("game_camera", None), ("subject_w0", "wrestler:0"), ("subject_w0_left", "wrestler:0,from=left"), ("subject_w1", "wrestler:1")]
TOL = 1e-4


def floats(text):
    return [float(x) for x in text.split()]


def judge(body):
    """[(draw, ok, why)] for the plane draws, and the plane chain verdict (ok, why)."""
    plane, vp, inv = [], {}, {}
    for line in body.splitlines():
        line = line.strip()
        m = re.match(r"rml_draw (\d+) kind (\w+) ", line)
        if m and m.group(2) == "plane" and int(m.group(1)) not in plane:
            plane.append(int(m.group(1)))
        m = re.match(r"rml_field (\d+) vs (ViewProj|PlaneInv) 4 (.*)$", line)
        if m:
            (vp if m.group(2) == "ViewProj" else inv)[int(m.group(1))] = floats(m.group(3))
    rows = []
    for d in plane:
        a, b = inv.get(d), vp.get(d)
        if not a or not b or len(a) < 16 or len(b) < 16:
            rows.append((d, False, "the draw's PlaneInv / ViewProj rows are not in the report"))
            continue
        ra, rb = a[12:16], b[12:16]
        scale = max(1.0, max(abs(x) for x in rb))
        diff = max(abs(x - y) for x, y in zip(ra, rb))
        rows.append((d, diff <= TOL * scale, "PlaneInv row 3 %s vs ViewProj row 3 %s: max diff %.6g (limit %.3g)" % (
            " ".join("%.6g" % x for x in ra), " ".join("%.6g" % x for x in rb), diff, TOL * scale)))
    m = re.search(r"plane made (\d+) \(draws \d+; \w+; no-draw frames \d+; blank-plane frames (\d+), passes skipped \d+; no-caster frames \d+; chain-failed frames (\d+)", body)
    if not m:
        chain = (False, "no plane chain census line")
    else:
        made, blank, failed = (int(x) for x in m.groups())
        chain = (made > 0 and blank == 0 and failed == 0, "plane made %d, blank-plane frames %d, chain-failed frames %d" % (made, blank, failed))
    return rows, chain


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join(ROOT, "out", "tools", "frame_rig.exe"))
    ap.add_argument("--record", default=RECORD)
    ap.add_argument("--game", default=os.path.join(MAIN, "game_data", "extracted"))
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "s111", "rigfloor"))
    a = ap.parse_args()
    a.exe, a.record, a.game, a.out = (os.path.abspath(x) for x in (a.exe, a.record, a.game, a.out))
    for p, what in ((a.exe, "the rig (tools\\scene\\build_rig.ps1)"), (a.record, "the run-2 record"), (a.game, "the game files")):
        if not os.path.exists(p):
            print("rigfloor_check: FAIL missing %s: %s" % (what, p))
            return 2
    os.makedirs(a.out, exist_ok=True)
    bad = 0
    for name, subject in ARMS:
        prefix = os.path.join(a.out, name)
        cmd = [a.exe, "--record", a.record, "--game", a.game, "--size", "1024x1024", "--no-family-png", "--prefix", prefix]
        if subject:
            cmd += ["--subject", subject]
        p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=600)
        rep = prefix + "_report.txt"
        body = open(rep, encoding="utf-8", errors="replace").read() if os.path.exists(rep) else p.stdout
        if p.returncode != 0:
            print("%s: FAIL frame_rig exit %d: %s" % (name, p.returncode, (p.stderr or p.stdout)[-400:].strip()))
            bad += 1
            continue
        rows, chain = judge(body)
        ok = bool(rows) and all(r[1] for r in rows) and chain[0]
        if subject:
            m = re.search(r"^subject plane mask: .*$", body, re.M)
            print("  %s: %s" % (name, m.group(0) if m else "no 'subject plane mask' line (an old rig: the mask keeps the game camera)"))
            ok = ok and m is not None
        for d, rok, why in rows:
            print("  %s: plane draw %d %s: %s" % (name, d, "ok" if rok else "WRONG", why))
        if not rows:
            print("  %s: no plane draw of the mat in the report" % name)
        print("  %s: plane chain %s: %s" % (name, "ok" if chain[0] else "WRONG", chain[1]))
        print("%s: %s" % (name, "PASS" if ok else "FAIL"))
        bad += 0 if ok else 1
    print("rigfloor_check: %s (%d of %d arms failed)" % ("PASS" if bad == 0 else "FAIL", bad, len(ARMS)))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
