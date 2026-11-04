# Session 85: draws of a frame-anatomy CSV whose screen extent (ext_*) lies inside a pixel box - which shaders draw
# the thing at that spot. py tools\native\s85_anat_region.py anatomy.csv x0 y0 x1 y1 [frame]
import csv
import sys
from collections import Counter

path, x0, y0, x1, y1 = sys.argv[1], *map(float, sys.argv[2:6])
only = int(sys.argv[6]) if len(sys.argv) > 6 else None
rows = list(csv.DictReader(open(path, newline="")))
c = Counter()
for r in rows:
    if r["kind"] != "draw" and r["kind"] != "0" and "draw" not in r["kind"].lower():
        continue
    if only is not None and int(r["frame"]) != only:
        continue
    try:
        ok = r["ext_ok"] not in ("0", "")
        mnx, mxx, mny, mxy = (float(r[k]) for k in ("ext_minx", "ext_maxx", "ext_miny", "ext_maxy"))
    except ValueError:
        continue
    if not ok:
        continue
    if mnx >= x0 - 20 and mxx <= x1 + 20 and mny >= y0 - 20 and mxy <= y1 + 20 and r["skipped"] in ("0", ""):
        c[(r["vs_hash"], r["ps_hash"], r["prim"], r["count"], r["rb_surface_info"])] += 1
for k, n in c.most_common(30):
    print(n, *k)
print("rows", len(rows), "kinds", Counter(r["kind"] for r in rows).most_common(4))
