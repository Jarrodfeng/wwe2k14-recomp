"""Session 98: resolve the guest return addresses in probe-plugin log lines ("chain A B C ..." and "lr A") to the
containing sub_ function, so a guest back chain reads as a call path.

Usage: py tools/analysis/resolve_probe_chain.py LOG [--grep TEXT] [--out FILE]
Only lines containing "[wwe2k14][probe]" (and TEXT, if given) are printed, each followed by one indented line per
resolved address. Function starts come from generated/default/wwe2k14_funcs.h (local codegen output, never committed),
as in resolve_backtrace.py.
"""
import argparse
import bisect
import glob
import re
import sys

ap = argparse.ArgumentParser()
ap.add_argument('log')
ap.add_argument('--grep', default='')
ap.add_argument('--out', default='')
a = ap.parse_args()

starts = sorted(
    int(m, 16)
    for path in glob.glob(r'generated\default\wwe2k14_funcs.h')
    for m in re.findall(r'DECLARE_REX_FUNC\(sub_([0-9A-Fa-f]{8})\)', open(path, encoding='utf-8').read()))
if not starts:
    sys.exit('no generated\\default\\wwe2k14_funcs.h: run from the repo root after codegen')


def name(addr):
    i = bisect.bisect_right(starts, addr) - 1
    if i < 0 or not addr:
        return '?'
    return f'sub_{starts[i]:08X}+0x{addr - starts[i]:X}'


out = open(a.out, 'w', encoding='utf-8') if a.out else sys.stdout
for line in open(a.log, encoding='utf-8', errors='replace'):
    if '[wwe2k14][probe]' not in line or (a.grep and a.grep not in line):
        continue
    out.write(line)
    addrs = []
    m = re.search(r'chain ((?:[0-9A-F]{8} ?)+)', line)
    if m:
        addrs = [int(x, 16) for x in m.group(1).split()]
    else:
        m = re.search(r'\blr ([0-9A-F]{8})', line)
        if m:
            addrs = [int(m.group(1), 16)]
    for x in addrs:
        out.write(f'      {x:08X} {name(x)}\n')
