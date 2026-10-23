"""Group one frame of a frame-anatomy capture into render passes (native renderer research, Session 38).

A "pass" here is a run of consecutive draws with the same render-target registers (RB_SURFACE_INFO, colour 0-3,
depth). For each pass: draws, draws that write depth, screen-space draws, distinct shaders, and the resolves (copies)
that end it. Numbers only; reads the CSV written by `cvar gpu_anatomy_capture N`.

Usage: py tools\\native\\anatomy_passes.py logs\\anatomy_NNN_K.csv [--frame 1] [--all]
"""
import argparse
import csv
from collections import Counter


def u(v):
    return int(v, 16) if v else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--frame", type=int, default=1)
    ap.add_argument("--all", action="store_true", help="print every pass, not only those with 5+ draws or a resolve")
    a = ap.parse_args()
    rows = [r for r in csv.DictReader(open(a.csv, newline="")) if int(r["frame"]) == a.frame]
    passes = []
    cur = None
    for r in rows:
        if r["kind"] == "copy":
            if cur is None:
                cur = {"key": None, "draws": [], "copies": []}
                passes.append(cur)
            cur["copies"].append(r)
            continue
        key = (r["rb_surface_info"], r["rb_color_info0"], r["rb_color_info1"], r["rb_depth_info"])
        if cur is None or cur["key"] != key or cur["copies"]:
            cur = {"key": key, "draws": [], "copies": []}
            passes.append(cur)
        cur["draws"].append(r)
    tot = Counter()
    print(f"frame {a.frame}: {sum(len(p['draws']) for p in passes)} draws, "
          f"{sum(len(p['copies']) for p in passes)} copies, {len(passes)} passes")
    print(" #  surface    color0     depth      msaa pitch draws zwrite ztest skip  scr  vs  ps  copies(dest info)")
    for i, p in enumerate(passes):
        d = p["draws"]
        surf = u(p["key"][0]) if p["key"] else 0
        zwrite = sum(1 for r in d if u(r["rb_depthcontrol"]) & 4)
        ztest = sum(1 for r in d if u(r["rb_depthcontrol"]) & 2)
        skipped = sum(1 for r in d if r["skipped"] not in ("", "0"))
        # screen space: rectangle list (prim 8) or no viewport scale (x scale 0 written as 0x00000000 / 1.0)
        scr = sum(1 for r in d if r["prim"] == "8")
        vs = len({r["vs_hash"] for r in d})
        ps = len({r["ps_hash"] for r in d})
        tot["draws"] += len(d)
        tot["zwrite"] += zwrite
        if not a.all and len(d) < 5 and not p["copies"]:
            continue
        cps = ", ".join(f"{c['copy_dest']} {c['copy_info']}" for c in p["copies"][:3])
        if len(p["copies"]) > 3:
            cps += f" (+{len(p['copies']) - 3})"
        k = p["key"] or ("", "", "", "")
        print(f"{i:2d}  {k[0]:10s} {k[1]:10s} {k[3]:10s} {(surf >> 16) & 3:4d} {surf & 0x3FFF:5d} {len(d):5d} {zwrite:6d}"
              f" {ztest:5d} {skipped:4d} {scr:4d} {vs:3d} {ps:3d}  {cps}")
    print(f"total draws {tot['draws']}, depth-writing {tot['zwrite']}")


if __name__ == "__main__":
    main()
