"""Audit every generated switch for a jump table that is too small.
For each switch, take the table base from the lis/addi pair feeding the lwzx before bctr. If the table lives in
the code range (inline, right after the bctr), the first case's code normally starts right where the table ends,
so expected entries = (lowest case label - table base) / 4. Flags switches where ReXGlue's case count differs.
Output: function, bctr (= table - 4), table, ReXGlue cases, expected, register."""
import glob
import os
import re

GEN = r"C:\Users\Jarrod\wwe2k14-recomp\generated\default"
pch = open(os.path.join(GEN, "wwe2k14_pch.h"), encoding="utf-8").read()
code_base = int(re.search(r"REX_CODE_BASE (0x[0-9A-Fa-f]+)", pch).group(1), 16)
code_end = code_base + int(re.search(r"REX_CODE_SIZE (0x[0-9A-Fa-f]+)", pch).group(1), 16)

flags, total, inline = [], 0, 0
for path in glob.glob(os.path.join(GEN, "wwe2k14_recomp.*.cpp")):
    text = open(path, encoding="utf-8").read()
    for fm in re.finditer(r"DEFINE_REX_FUNC\((\w+)\)\s*\{", text):
        end = text.find("\nDEFINE_REX_FUNC(", fm.end())
        body = text[fm.end(): end if end != -1 else len(text)]
        for sm in re.finditer(r"switch \(ctx\.r(\d+)\.u32\) \{(.*?)default:", body, re.S):
            total += 1
            labels = [int(x, 16) for x in re.findall(r"goto loc_([0-9A-F]+);", sm.group(2))]
            pre = [l.strip()[3:] for l in body[:sm.start()].splitlines() if l.strip().startswith("// ")][-12:]
            lis, table = {}, None
            for ins in pre:
                m = re.match(r"lis (r\d+),(-?\d+)$", ins)
                if m:
                    lis[m.group(1)] = (int(m.group(2)) & 0xFFFF) << 16
                m = re.match(r"addi (r\d+),(r\d+),(-?\d+)$", ins)
                if m and m.group(2) in lis:
                    table = (lis[m.group(2)] + int(m.group(3))) & 0xFFFFFFFF
            if table is None or not (code_base <= table < code_end) or not labels:
                continue
            inline += 1
            first = min(labels)
            if first <= table or (first - table) % 4:
                continue
            expected = (first - table) // 4
            if expected != len(labels) and expected <= 1024:
                flags.append((fm.group(1), table - 4, table, len(labels), expected, int(sm.group(1))))

print(f"{total} switches; {inline} with an inline (code-range) table; {len(flags)} size mismatches")
for f, bctr, table, n, exp, reg in sorted(flags, key=lambda x: x[1]):
    print(f"  {f}: bctr 0x{bctr:08X} table 0x{table:08X} rexglue={n} expected={exp} reg=r{reg}")
