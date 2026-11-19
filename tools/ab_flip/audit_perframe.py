#!/usr/bin/env python3
"""S102 M7 (review F5): audit every PerFrame row of src/renderer_adapter/rdr_switches.h for START-TIME copies: a switch the table calls PerFrame but whose cvar is also read by
REXCVAR_GET outside the table (typically reader.h StartLive copying it into LiveOptions, or a device / pipeline built once). Prints one line per switch:
  NAME | cvar reads outside the table (file:line) | table reads (rdr_sw::Bool / Int / Seen)
A switch with a cvar read (REXCVAR_GET or a literal name) in reader.h, device.cpp or live.cpp is a candidate 'armed at start' switch (the flip only works if the value at start
already arms it): the bench passes the non-reference phase's values on the command line in both arms, and ab_flip_math.h refuses a spec for a switch in ARMED unless its start value
equals the non-reference phase's. Run:  py -I tools\\ab_flip\\audit_perframe.py [--root DIR]"""
import glob
import os
import re
import sys

root = sys.argv[sys.argv.index('--root') + 1] if '--root' in sys.argv else '.'
t = open(os.path.join(root, 'src', 'renderer_adapter', 'rdr_switches.h'), encoding='utf-8').read()
rows = re.findall(r'X\((\w+),\s*"(\w+)",\s*(PerFrame\w*|StartOnly)', t)
per = [(i, n) for i, n, k in rows if k.startswith('PerFrame')]
files = [f for f in glob.glob(os.path.join(root, 'src', '**', '*'), recursive=True) if f.endswith(('.h', '.cpp', '.inc'))]
txt = {f: open(f, encoding='utf-8', errors='replace').read().split('\n') for f in files}
print('%d table rows, %d PerFrame' % (len(rows), len(per)))
for ident, name in per:
    outside, table = [], 0
    for f, ls in txt.items():
        if f.endswith('rdr_switches.h'):
            continue
        for ln, l in enumerate(ls, 1):
            if ('REXCVAR_GET(' + name + ')' in l or '"' + name + '"' in l) and 'REXCVAR_DEFINE' not in l and not re.search(r'rdr_sw::\w*Seen\(', l):   # S103 RLF: a BoolSeen(k..., REXCVAR_GET(x), frame) line is the per-frame read with the cvar as its fallback, not a start-time read
                outside.append('%s:%d' % (os.path.basename(f), ln))
            if re.search(r'rdr_sw::\w*\(\s*(rdr_sw::)?k' + ident + r'\b', l):
                table += 1
    print('%s | outside the table: %s | table reads: %d' % (name, ', '.join(outside[:8]) or '-', table))
