"""From dumped jump-table words, compute each table's true length: consecutive entries (from 0) that point
inside the owning function's address span [start, next registered function). Compare with ReXGlue's labels.
Writes switch_fixes.toml-style blocks for tables that are really longer than ReXGlue's."""
import bisect
import glob
import os
import re
import sys

SP = sys.argv[1]
GEN = r"C:\Users\Jarrod\wwe2k14-recomp\generated\default"
words = {}
for l in open(os.path.join(SP, "dump_switches.log"), encoding="utf-8", errors="replace"):
    m = re.search(r"\[dump\] (0x[0-9A-F]{8}) \[\d+\] = (0x[0-9A-F]{8})", l)
    if m:
        words[int(m.group(1), 16)] = int(m.group(2), 16)

reg = open(os.path.join(GEN, "wwe2k14_register.cpp"), encoding="utf-8").read()
starts = sorted(int(a, 16) for a in re.findall(r"SetFunction\(0x([0-9A-Fa-f]+),", reg))

# ReXGlue's labels per table, from generated code
rex_labels = {}
for path in glob.glob(os.path.join(GEN, "wwe2k14_recomp.*.cpp")):
    text = open(path, encoding="utf-8").read()
    for fm in re.finditer(r"DEFINE_REX_FUNC\((\w+)\)\s*\{", text):
        end = text.find("\nDEFINE_REX_FUNC(", fm.end())
        body = text[fm.end(): end if end != -1 else len(text)]
        for sm in re.finditer(r"switch \(ctx\.r(\d+)\.u32\) \{(.*?)default:", body, re.S):
            pre = [l.strip()[3:] for l in body[:sm.start()].splitlines() if l.strip().startswith("// ")][-12:]
            lis, table = {}, None
            for ins in pre:
                m = re.match(r"lis (r\d+),(-?\d+)$", ins)
                if m:
                    lis[m.group(1)] = (int(m.group(2)) & 0xFFFF) << 16
                m = re.match(r"addi (r\d+),(r\d+),(-?\d+)$", ins)
                if m and m.group(2) in lis:
                    table = (lis[m.group(2)] + int(m.group(3))) & 0xFFFFFFFF
            if table is not None:
                rex_labels[table] = (fm.group(1), int(sm.group(1)),
                                     [int(x, 16) for x in re.findall(r"goto loc_([0-9A-F]+);", sm.group(2))])

fixes, same, odd = [], [], []
for l in open(os.path.join(SP, "audit_switches.txt")):
    m = re.search(r"(sub_[0-9A-F]+): bctr (0x[0-9A-F]{8}) table (0x[0-9A-F]{8}) rexglue=(\d+)", l)
    if not m:
        continue
    fname, bctr, table = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
    fstart = int(fname[4:], 16)
    fnext = starts[bisect.bisect_right(starts, fstart)]
    entries, i = [], 0
    while (table + 4 * i) in words:
        v = words[table + 4 * i]
        if not (fstart <= v < fnext and v % 4 == 0):
            break
        entries.append(v)
        i += 1
    _, regno, rl = rex_labels[table]
    prefix_ok = entries[:len(rl)] == rl
    row = (fname, bctr, table, regno, len(rl), len(entries), prefix_ok, entries, words.get(table + 4 * len(entries)))
    if len(entries) > len(rl) and prefix_ok:
        fixes.append(row)
    elif len(entries) == len(rl) and prefix_ok:
        same.append(row)
    else:
        odd.append(row)

print(f"{len(fixes)} tables truly longer than ReXGlue's, {len(same)} correct (audit false alarm), {len(odd)} odd")
for f, bctr, t, r, n, true, ok, e, stop in fixes:
    print(f"  FIX {f} bctr 0x{bctr:08X}: rexglue {n} -> true {true} (stops at word 0x{stop:08X})")
for f, bctr, t, r, n, true, ok, e, stop in odd:
    print(f"  ODD {f} bctr 0x{bctr:08X}: rexglue {n}, data-valid {true}, rexglue labels == data prefix: {ok}")

with open(os.path.join(SP, "switch_fixes.toml"), "w", newline="\n") as out:
    for f, bctr, t, r, n, true, ok, e, stop in fixes:
        out.write(f"# {f}: ReXGlue found {n} entries, table data has {true} in-function targets "
                  f"(next word 0x{stop:08X} is outside the function)\n")
        out.write(f"[[switch_tables]]\naddress = 0x{bctr:08X}\nregister = {r}\nlabels = [")
        out.write(", ".join(f"0x{x:08X}" for x in e))
        out.write("]\n\n")
