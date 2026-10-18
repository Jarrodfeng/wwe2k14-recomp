"""Classify [wwe2k14][scan] candidates from a run log against the current generated code.

For each registered function, extent = start + 4 * (number of PPC instruction comments in its body).
Candidate classes:
  GAP_START   - not covered by any function, and exactly where the preceding function's code ends
  IN_GAP      - not covered, but some uncovered bytes sit between the preceding function's end and it
  INSIDE_FUNC - inside a registered function's instruction range (mid-function pointer: do NOT hint)
Usage: classify_candidates.py <run log> [out.tsv]
"""
import bisect
import glob
import os
import re
import sys

GEN = r"C:\Users\Jarrod\wwe2k14-recomp\generated\default"
log_path = sys.argv[1]
out_path = sys.argv[2] if len(sys.argv) > 2 else None

cands = []
for line in open(log_path, encoding="utf-8", errors="replace"):
    m = re.search(r"\[scan\] (candidate|lone) 0x([0-9A-F]{8}) \(pointer at 0x([0-9A-F]{8})\)", line)
    if m:
        cands.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1)))

reg = open(os.path.join(GEN, "wwe2k14_register.cpp"), encoding="utf-8").read()
starts_by_name = {n: int(a, 16) for a, n in re.findall(r"SetFunction\(0x([0-9A-Fa-f]+),\s*(\w+)\)", reg)}
sizes = {}
for path in glob.glob(os.path.join(GEN, "wwe2k14_recomp.*.cpp")):
    text = open(path, encoding="utf-8").read()
    for m in re.finditer(r"DEFINE_REX_FUNC\((\w+)\)\s*\{", text):
        end = text.find("\nDEFINE_REX_FUNC(", m.end())
        chunk = text[m.end(): end if end != -1 else len(text)]
        sizes[m.group(1)] = sum(1 for l in chunk.splitlines() if l.strip().startswith("// ")) * 4

funcs = sorted((a, n, sizes.get(n, 0)) for n, a in starts_by_name.items())
starts = [a for a, _, _ in funcs]


def classify(t):
    i = bisect.bisect_right(starts, t) - 1
    if i < 0:
        return "BEFORE_CODE", None
    a, n, size = funcs[i]
    if a == t:
        return "REGISTERED", n
    if t < a + size:
        return "INSIDE_FUNC", f"{n}+0x{t - a:X} (size 0x{size:X})"
    gap = t - (a + size)
    return ("GAP_START" if gap == 0 else "IN_GAP"), f"after {n} (ends 0x{a + size:08X}, gap 0x{gap:X})"


counts = {}
rows = []
for t, where, kind in cands:
    cls, info = classify(t)
    counts[(kind, cls)] = counts.get((kind, cls), 0) + 1
    rows.append((t, where, cls, info, kind))

print(f"{len(cands)} candidates from {os.path.basename(log_path)}: {counts}")

for cls in ("INSIDE_FUNC", "IN_GAP"):
    print(f"--- {cls} ---")
    for t, where, c, info, kind in rows:
        if c == cls and kind == 'candidate':
            print(f"  0x{t:08X} (ptr at 0x{where:08X}) {info}")
if out_path:
    with open(out_path, "w") as f:
        f.write("target\tpointer_at\tclass\tinfo\tkind\n")
        for t, where, cls, info, kind in rows:
            f.write(f"0x{t:08X}\t0x{where:08X}\t{cls}\t{info}\t{kind}\n")
