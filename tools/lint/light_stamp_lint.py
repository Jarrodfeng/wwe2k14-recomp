"""S103 RML lint (review F1): every Bound flag that makes a draw read ctx.shadow_m / shadow_prm (the receivers' and the ring mat's shadow constants) must also be in the kept draw lists' light stamp
(front_end.cpp `st.h[4] = HashBytes(rd_map->prm ...`), or the kept block's TexProjLight / ViewLight / ShadowPrm go stale on the live path (the ring mat's light pass did, until the stamp took b.props_reader).

  py -I tools\\lint\\light_stamp_lint.py [--root DIR] [--plant]      exit 0 = good; --plant removes a flag from the stamp text: exit 0 = the lint caught it (the plant convention), 1 = it is blind
"""
import argparse, os, re, sys


def check(text):
    reads = [l for l in text.splitlines() if re.search(r"ctx\.shadow_m\s*=", l)]
    stamps = [l for l in text.splitlines() if "st.h[4] = HashBytes(rd_map->prm" in l]
    if not reads or not stamps:
        return [f"cannot find the ctx.shadow_m assignment ({len(reads)}) or the light stamp ({len(stamps)}) in front_end.cpp: the lint must follow the code"]
    flags = set()
    for l in reads:
        cond = l.split("ctx.shadow_m")[0]
        flags |= set(re.findall(r"\bb\.(\w+)", cond))
    stamp = " ".join(stamps)
    # S103 XLF (review B2): every nullable CharCtx level pointer the join sets (cc.wkl, cc.idmap) is folded into the kStMaterial stamp (st.h[5]) on its own: the call must name both (wrk::StampLevels does each one under its own null test)
    lv = [l for l in text.splitlines() if "StampLevels(" in l and "st.h[5]" in l]
    out_lv = []
    if not lv:
        out_lv.append("cannot find the wrk::StampLevels call on st.h[5] in front_end.cpp: the lint must follow the code")
    else:
        out_lv += [f"CharCtx level pointer cc.{p} is not folded into the kStMaterial stamp" for p in ("wkl", "idmap") if f"cc.{p}" not in lv[0]]
    # the stamp line is guarded by `if ((b.shadow || ...) && rd_map)`: look at the guard on the line itself
    return out_lv + [f"Bound flag b.{f} makes the draw read ctx.shadow_m but is not in the light stamp guard" for f in sorted(flags) if f"b.{f}" not in stamp]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
    ap.add_argument("--plant", action="store_true")
    a = ap.parse_args()
    text = open(os.path.join(a.root, "src", "renderer_scene", "front_end.cpp"), encoding="utf-8", errors="replace").read()
    if a.plant:
        text = text.replace("b.props_reader && !options_.plant_light_stamp_no_mat", "false").replace("StampLevels(cc.wkl, cc.idmap", "StampLevels(cc.wkl, nullptr")
    bad = check(text)
    for b in bad:
        print("FAIL", b)
    if a.plant:
        print("light_stamp_lint plant:", "caught (as it must)" if bad else "DID NOT FAIL: the lint is blind to it")
        return 0 if bad else 1
    print("light_stamp_lint:", "FAIL" if bad else "ok")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
