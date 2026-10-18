"""Side-by-side PPC instruction comparison of two generated functions (from possibly different gen dirs).
Usage: compare_funcs.py <genA> <prefixA> <nameA> <genB> <prefixB> <nameB>"""
import glob
import os
import re
import sys


def body(gen, prefix, name):
    for path in glob.glob(os.path.join(gen, f"{prefix}_recomp.*.cpp")):
        text = open(path, encoding="utf-8").read()
        m = re.search(r"DEFINE_REX_FUNC\(" + re.escape(name) + r"\)\s*\{", text)
        if not m:
            continue
        end = text.find("\nDEFINE_REX_FUNC(", m.end())
        out = []
        for line in text[m.end(): end if end != -1 else len(text)].splitlines():
            s = line.strip()
            if s.startswith("// "):
                out.append(s[3:])
            elif re.match(r"^loc_[0-9A-Fa-f]+:", s):
                out.append(s)
        return out
    return []


a = body(*sys.argv[1:4])
b = body(*sys.argv[4:7])
print(f"{sys.argv[3]:<40} | {sys.argv[6]}")
for i in range(max(len(a), len(b))):
    x = a[i] if i < len(a) else ""
    y = b[i] if i < len(b) else ""
    # Normalise addresses so structurally identical code shows as '='
    nx = re.sub(r"0x[0-9a-fA-F]+|loc_[0-9A-Fa-f]+", "ADDR", x)
    ny = re.sub(r"0x[0-9a-fA-F]+|loc_[0-9A-Fa-f]+", "ADDR", y)
    print(f"{x:<40} {'=' if nx == ny else '≠'} {y}")
print(f"instructions: {sum(1 for l in a if not l.startswith('loc_'))} vs {sum(1 for l in b if not l.startswith('loc_'))}")
