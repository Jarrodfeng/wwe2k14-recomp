"""Session 24 (DLSS): list the tail of one captured frame from an anatomy CSV (the draws and resolves from a given
resolve on, in submission order), to find where the post chain ends and the HUD starts.

Usage: py tools\\dlss\\frame_tail.py ANATOMY.csv [--frame N] [--from-resolve K]
Consecutive draws with the same shaders / surface / colour / depth / blend state / textures are printed as one line.
"""
import argparse
import csv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--frame", type=int, default=None, help="frame number (default: the first in the file)")
    ap.add_argument("--from-resolve", type=int, default=44, help="start listing at this resolve index")
    a = ap.parse_args()
    rows = list(csv.DictReader(open(a.csv, newline="")))
    frames = sorted({int(r["frame"]) for r in rows})
    f = a.frame if a.frame is not None else frames[0]
    rows = [r for r in rows if int(r["frame"]) == f]
    print(f"frame {f}: {len(rows)} rows (frames in file: {frames[:6]}{'...' if len(frames) > 6 else ''})")
    state = {"res": 0, "prev": None, "run": 0, "start": None, "verts": 0}

    def flush():
        p = state["prev"]
        if p:
            print(f"    x{state['run']:4d} idx {state['start']}.. vs {p[0][:8]} ps {p[1][:8]} surf {p[2]} col0 {p[3]} "
                  f"dctl {p[4]} colctl {p[5]} mask {p[6]} ps_tex {p[7]} verts {state['verts']}")
        state["prev"], state["run"], state["verts"] = None, 0, 0

    for r in rows:
        if r["kind"] == "draw":
            if state["res"] < a.from_resolve:
                continue
            k = (r["vs_hash"], r["ps_hash"], r["rb_surface_info"], r["rb_color_info0"], r["rb_depthcontrol"],
                 r["rb_colorcontrol"], r["rb_color_mask"], r["ps_tex"])
            if k != state["prev"]:
                flush()
                state["prev"], state["start"] = k, r["idx"]
            state["run"] += 1
            state["verts"] += int(r["count"] or 0)
        else:
            if state["res"] >= a.from_resolve:
                flush()
                print(f"  resolve {state['res']} idx {r['idx']} dest {r['copy_dest']} info {r['copy_info']} surf "
                      f"{r['rb_surface_info']} col0 {r['rb_color_info0']} depth {r['rb_depth_info']}")
            state["res"] += 1
    flush()


if __name__ == "__main__":
    main()
