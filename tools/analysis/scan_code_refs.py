"""Static scan of generated code: find lis+addi/ori pairs that build an address inside the code range which is
not a registered function, then classify each target (GAP_START / IN_GAP / INSIDE_FUNC) like the runtime scan.
Writes a TSV compatible with check_boundaries.py (target, source_fn, class, info, kind=code).
Usage: scan_code_refs.py <out.tsv>"""
import bisect
import glob
import os
import re
import sys

GEN = r"C:\Users\Jarrod\wwe2k14-recomp\generated\default"
pch = open(os.path.join(GEN, "wwe2k14_pch.h"), encoding="utf-8").read()
code_base = int(re.search(r"REX_CODE_BASE (0x[0-9A-Fa-f]+)", pch).group(1), 16)
code_end = code_base + int(re.search(r"REX_CODE_SIZE (0x[0-9A-Fa-f]+)", pch).group(1), 16)

reg = open(os.path.join(GEN, "wwe2k14_register.cpp"), encoding="utf-8").read()
name_at = {int(a, 16): n for a, n in re.findall(r"SetFunction\(0x([0-9A-Fa-f]+),\s*(\w+)\)", reg)}
starts = sorted(name_at)

sizes, refs = {}, {}
for path in glob.glob(os.path.join(GEN, "wwe2k14_recomp.*.cpp")):
    text = open(path, encoding="utf-8").read()
    for m in re.finditer(r"DEFINE_REX_FUNC\((\w+)\)\s*\{", text):
        end = text.find("\nDEFINE_REX_FUNC(", m.end())
        chunk = text[m.end(): end if end != -1 else len(text)]
        ins = [l.strip()[3:] for l in chunk.splitlines() if l.strip().startswith("// ")]
        sizes[m.group(1)] = 4 * len(ins)
        lis = {}
        for i in ins:
            mm = re.match(r"lis (r\d+),(-?\d+)$", i)
            if mm:
                lis[mm.group(1)] = (int(mm.group(2)) & 0xFFFF) << 16
                continue
            mm = re.match(r"(addi|ori) (r\d+),(r\d+),(-?\d+)$", i)
            if mm and mm.group(3) in lis:
                imm = int(mm.group(4))
                v = (lis[mm.group(3)] + imm) & 0xFFFFFFFF if mm.group(1) == "addi" else lis[mm.group(3)] | (imm & 0xFFFF)
                if code_base <= v < code_end and v % 4 == 0 and v not in name_at:
                    refs.setdefault(v, m.group(1))
                continue
            mm = re.match(r"\w+ (r\d+),", i)  # any other write to a tracked register invalidates it
            if mm and mm.group(1) in lis:
                del lis[mm.group(1)]


def classify(t):
    i = bisect.bisect_right(starts, t) - 1
    a = starts[i]
    n = name_at[a]
    size = sizes.get(n, 0)
    if t < a + size:
        return "INSIDE_FUNC", f"{n}+0x{t - a:X} (size 0x{size:X})"
    gap = t - (a + size)
    return ("GAP_START" if gap == 0 else "IN_GAP"), f"after {n} (ends 0x{a + size:08X}, gap 0x{gap:X})"


counts = {}
with open(sys.argv[1], "w") as f:
    f.write("target\tpointer_at\tclass\tinfo\tkind\n")
    for v, src in sorted(refs.items()):
        cls, info = classify(v)
        counts[cls] = counts.get(cls, 0) + 1
        f.write(f"0x{v:08X}\t{src}\t{cls}\t{info}\tcode\n")
print(f"code-built addresses into code, unregistered: {len(refs)} -> {counts}")
