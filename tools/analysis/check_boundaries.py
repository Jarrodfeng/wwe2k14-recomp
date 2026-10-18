"""For candidates in candidates_*.tsv: check the code just before each one can't fall through into it.
GAP_START: previous registered function's last listed instruction must be an unconditional transfer
           (blr / b <addr> / bctr / bctrl? no: bctrl returns) and its labels must all be below the candidate.
IN_GAP:    there must be another candidate earlier in the same uncovered gap (a chain of tiny functions).
Prints a verdict per candidate and the final accepted list.
Usage: check_boundaries.py <candidates.tsv> <exclude hex,...>"""
import bisect
import glob
import os
import re
import sys

GEN = r"C:\Users\Jarrod\wwe2k14-recomp\generated\default"
rows = [l.rstrip("\n").split("\t")[:4] for l in open(sys.argv[1]).readlines()[1:]]
exclude = {int(x, 16) for x in sys.argv[2].split(",")} if len(sys.argv) > 2 and sys.argv[2] else set()

reg = open(os.path.join(GEN, "wwe2k14_register.cpp"), encoding="utf-8").read()
start_of = {n: int(a, 16) for a, n in re.findall(r"SetFunction\(0x([0-9A-Fa-f]+),\s*(\w+)\)", reg)}
bodies = {}
for path in glob.glob(os.path.join(GEN, "wwe2k14_recomp.*.cpp")):
    text = open(path, encoding="utf-8").read()
    for m in re.finditer(r"DEFINE_REX_FUNC\((\w+)\)\s*\{", text):
        end = text.find("\nDEFINE_REX_FUNC(", m.end())
        chunk = text[m.end(): end if end != -1 else len(text)]
        ins = [l.strip()[3:] for l in chunk.splitlines() if l.strip().startswith("// ")]
        labels = [int(x, 16) for x in re.findall(r"^\s*loc_([0-9A-Fa-f]+):", chunk, re.M)]
        bodies[m.group(1)] = (ins, labels)

UNCOND = re.compile(r"^(blr|bctr|b 0x[0-9a-f]+)$")
cands = sorted(int(r[0], 16) for r in rows if r[2] in ("GAP_START", "IN_GAP"))
accepted, rejected = [], []
rows.sort(key=lambda r: int(r[0], 16))  # address order, so chains see their predecessors' verdicts
for target_s, ptr, cls, info in rows:
    t = int(target_s, 16)
    if cls not in ("GAP_START", "IN_GAP"):
        continue
    if t in exclude:
        rejected.append((t, "excluded (exception-table region)"))
        continue
    prev_name = re.search(r"after (\w+)", info).group(1)
    ins, labels = bodies.get(prev_name, ([], []))
    prev_end = start_of[prev_name] + 4 * len(ins)
    if cls == "GAP_START":
        last = ins[-1] if ins else ""
        if not UNCOND.match(last):
            rejected.append((t, f"previous fn {prev_name} ends with '{last}' (could fall through)"))
        elif labels and max(labels) >= t:
            rejected.append((t, f"previous fn {prev_name} has a label at/after the candidate"))
        else:
            accepted.append((t, f"GAP_START, {prev_name} ends with '{last}'"))
    else:
        ok_so_far = {a for a, _ in accepted}
        earlier = [c for c in cands if prev_end <= c < t and c in ok_so_far]
        if earlier:
            accepted.append((t, f"IN_GAP, chained after candidate 0x{earlier[-1]:08X}"))
        else:
            rejected.append((t, f"IN_GAP with no earlier ACCEPTED candidate in the gap after {prev_name}"))

print(f"accepted {len(accepted)}, rejected {len(rejected)}")
for t, why in rejected:
    print(f"  REJECT 0x{t:08X}: {why}")
last_kinds = {}
for t, why in accepted:
    k = why.split("'")[1] if "'" in why else "chain"
    k = "b <addr>" if k.startswith("b 0x") else k
    last_kinds[k] = last_kinds.get(k, 0) + 1
print("accepted, by what precedes them:", last_kinds)
with open(os.path.join(os.path.dirname(sys.argv[1]), "accepted.txt"), "w") as f:
    for t, why in sorted(accepted):
        f.write(f"0x{t:08X}\t{why}\n")
