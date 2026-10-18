"""Find code that materialises a 32-bit address with lis+addi/ori (as seen in generated PPC comments).
Usage: find_addr_refs.py <hex address> [more...]"""
import glob
import os
import re
import sys

GEN = r"C:\Users\Jarrod\wwe2k14-recomp\generated\default"
targets = [int(a, 16) for a in sys.argv[1:]]


def halves(addr):
    lo = addr & 0xFFFF
    hi = (addr >> 16) & 0xFFFF
    if lo >= 0x8000:  # addi sign-extends, so the high half is pre-incremented
        hi = (hi + 1) & 0xFFFF
    hi_s = hi - 0x10000 if hi >= 0x8000 else hi
    lo_s = lo - 0x10000 if lo >= 0x8000 else lo
    return hi_s, lo_s, lo


for t in targets:
    hi_s, lo_s, lo_u = halves(t)
    print(f"=== 0x{t:08X}: lis ..,{hi_s} + addi ..,{lo_s} (or ori ..,{lo_u}) ===")
    hits = 0
    for path in glob.glob(os.path.join(GEN, "wwe2k14_recomp.*.cpp")):
        text = open(path, encoding="utf-8").read()
        func = None
        lis_regs = {}
        for line in text.splitlines():
            m = re.match(r"DEFINE_REX_FUNC\((\w+)\)", line)
            if m:
                func, lis_regs = m.group(1), {}
                continue
            s = line.strip()
            if not s.startswith("// "):
                continue
            ins = s[3:]
            m = re.match(r"lis (r\d+),(-?\d+)$", ins)
            if m:
                lis_regs[m.group(1)] = int(m.group(2))
                continue
            m = re.match(r"(addi|ori) (r\d+),(r\d+),(-?\d+)$", ins)
            if m and lis_regs.get(m.group(3)) == hi_s:
                val = int(m.group(4))
                if (m.group(1) == "addi" and val == lo_s) or (m.group(1) == "ori" and val == lo_u):
                    hits += 1
                    print(f"  {os.path.basename(path)} {func}: lis {m.group(3)},{hi_s} ; {ins}")
    print(f"  -> {hits} reference(s)")
