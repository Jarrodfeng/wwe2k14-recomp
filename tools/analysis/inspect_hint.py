"""Inspect a codegen function-entry hint using ReXGlue's generated C++ (PPC instructions are in // comments).

Usage: inspect_hint.py <target hex> <source hex>
Prints: the hinted function's instructions, the tail of the preceding function, and the function containing
the branch source (its instructions, with the branch to the target marked).
"""
import bisect
import glob
import os
import re
import sys

GEN = os.environ.get("GEN", r"C:\Users\Jarrod\wwe2k14-recomp\generated\default")
PREFIX = os.environ.get("PREFIX", "wwe2k14")

reg = open(os.path.join(GEN, f"{PREFIX}_register.cpp"), encoding="utf-8").read()
funcs = sorted((int(a, 16), n) for a, n in re.findall(r"SetFunction\(0x([0-9A-Fa-f]+),\s*(\w+)\)", reg))
starts = [a for a, _ in funcs]

_cache = {}


def body(name):
    """Return the list of PPC instruction comments / labels for a generated function."""
    if name in _cache:
        return _cache[name]
    for path in glob.glob(os.path.join(GEN, f"{PREFIX}_recomp.*.cpp")):
        text = open(path, encoding="utf-8").read()
        m = re.search(r"DEFINE_REX_FUNC\(" + re.escape(name) + r"\)\s*\{", text)
        if not m:
            continue
        end = text.find("\nDEFINE_REX_FUNC(", m.end())
        chunk = text[m.end(): end if end != -1 else len(text)]
        lines = []
        for line in chunk.splitlines():
            s = line.strip()
            if s.startswith("// ") or re.match(r"^loc_[0-9A-Fa-f]+:", s):
                lines.append(s)
        _cache[name] = (os.path.basename(path), lines)
        return _cache[name]
    return (None, [])


def containing(addr):
    i = bisect.bisect_right(starts, addr) - 1
    return funcs[i], (funcs[i + 1] if i + 1 < len(funcs) else None)


target, source = int(sys.argv[1], 16), int(sys.argv[2], 16)
print(f"######## target 0x{target:08X}  <- b from 0x{source:08X}  (delta {source - target:+#x})")

(ta, tn), nxt = containing(target)
print(f"\n[1] hinted function {tn} @0x{ta:08X} (next function @0x{nxt[0]:08X}, size 0x{nxt[0] - ta:X})")
f, lines = body(tn)
for l in lines[:24]:
    print("    " + l)

i = starts.index(ta)
pa, pn = funcs[i - 1]
f, plines = body(pn)
print(f"\n[2] preceding function {pn} @0x{pa:08X} (size 0x{ta - pa:X}) -- last instructions:")
for l in plines[-6:]:
    print("    " + l)

(sa, sn), snxt = containing(source)
f, slines = body(sn)
print(f"\n[3] source function {sn} @0x{sa:08X}..0x{snxt[0]:08X} (size 0x{snxt[0] - sa:X}); source is at +0x{source - sa:X}")
tgt_pat = re.compile(r"\bb\s+0x0*%x\b" % target, re.I)
hits = [k for k, l in enumerate(slines) if tgt_pat.search(l)]
print(f"    branch-to-target comment lines: {hits} of {len(slines)} lines")
show = slines if len(slines) <= 40 else (slines[:6] + ["    ..."] + slines[max(0, (hits[0] if hits else len(slines)) - 8):(hits[0] if hits else len(slines)) + 4])
for l in show:
    mark = "  <==" if tgt_pat.search(l) else ""
    print("    " + l + mark)
