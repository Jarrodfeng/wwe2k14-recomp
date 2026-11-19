"""Find a short sequence of PPC instructions across every recompiled function (generated/default, never committed).

Answers: "which functions do `lwz rA,48(rB)` and then, within N instructions, `lwz rC,192(rA)`?" (S103 L31B: the character ->
skeleton link). Each pattern is a Python regex matched against the PPC comment text (e.g. `lwz r(\\d+),48\\(r\\d+\\)`); a later
pattern may use \\1.. groups of the FIRST pattern via {g1}, {g2} (formatted in before matching). Prints function, guest address of
each matched instruction and the instructions.

Usage (run from the main checkout, where generated\\default lives):
  py tools/analysis/seq_find.py --within 8 "lwz r(\\d+),48\\(r\\d+\\)" "lwz r\\d+,192\\(r{g1}\\)"
"""
import argparse
import glob
import re

ap = argparse.ArgumentParser()
ap.add_argument('patterns', nargs='+')
ap.add_argument('--within', type=int, default=8, help='max instructions between consecutive patterns')
ap.add_argument('--limit', type=int, default=200)
ap.add_argument('--root', default=r'generated\default')
a = ap.parse_args()

fn_re = re.compile(r'DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)')
lab_re = re.compile(r'^loc_([0-9A-F]{8}):')
hits = 0
for path in sorted(glob.glob(a.root + r'\wwe2k14_recomp.*.cpp')):
    fn, addr, ins = None, 0, []   # ins: (addr, text) of the current function
    def scan():
        global hits
        if not fn or hits >= a.limit:
            return
        first = re.compile(a.patterns[0])
        for i, (ad, tx) in enumerate(ins):
            m = first.fullmatch(tx)
            if not m:
                continue
            groups = {f'g{k + 1}': v for k, v in enumerate(m.groups())}
            chain, j, ok = [(ad, tx)], i, True
            for p in a.patterns[1:]:
                rx = re.compile(p.format(**groups))
                found = None
                for k in range(j + 1, min(len(ins), j + 1 + a.within)):
                    if rx.fullmatch(ins[k][1]):
                        found = k
                        break
                if found is None:
                    ok = False
                    break
                chain.append(ins[found])
                j = found
            if ok:
                hits += 1
                print(f'sub_{fn}: ' + ' ; '.join(f'0x{x:08X} {t}' for x, t in chain))
                if hits >= a.limit:
                    return
    for line in open(path, encoding='utf-8'):
        s = line.strip()
        m = fn_re.match(s)
        if m:
            scan()
            fn, addr, ins = m.group(1), int(m.group(1), 16), []
            continue
        if fn is None:
            continue
        m = lab_re.match(s)
        if m:
            addr = int(m.group(1), 16)
            continue
        if s.startswith('// ') and not s.startswith('// PPC'):
            ins.append((addr, s[3:]))
            addr += 4
    scan()
print(f'-- {hits} hit(s)')
