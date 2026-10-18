"""Resolve [wwe2k14][ftrace] backtraces (from --wwe2k14_file_trace=true, src/file_trace.h) to host symbols.

Usage: py tools/analysis/resolve_ftrace.py <run log or extracted ftrace lines> [map file] > out.txt

Each backtrace frame is an exe offset (+0x...); it is matched to the nearest symbol at or below it in the
linker map (sub_XXXXXXXX = the recompiled guest function of that name). The map must come from the same link
as the exe that produced the log (check the map's "Timestamp is" line against the build). Prints the log's
open/read/scan header and one line of frames, innermost first. Output is derived from a game run: keep it
under out/harness/ (gitignored). Session 8 task 7.
"""
import bisect
import re
import sys

log_path = sys.argv[1]
map_path = sys.argv[2] if len(sys.argv) > 2 else r'out\build\win-amd64-release\wwe2k14.map'

symbols = []
pattern = re.compile(r'^\s*0001:[0-9a-fA-F]+\s+(\S+)\s+([0-9a-fA-F]{16})\s')
with open(map_path, encoding='utf-8', errors='replace') as f:
    for line in f:
        m = pattern.match(line)
        if m and not m.group(1).startswith('__imp__'):
            symbols.append((int(m.group(2), 16) - 0x140000000, m.group(1)))
symbols.sort()
starts = [s[0] for s in symbols]


def resolve(rva):
    i = bisect.bisect_right(starts, rva) - 1
    if i < 0:
        return '?'
    start, name = symbols[i]
    if len(name) > 60:
        name = name[:60] + '...'
    return '%s+0x%X' % (name, rva - start)


with open(log_path, encoding='utf-8', errors='replace') as f:
    for line in f:
        m = re.search(r'\[wwe2k14\]\[ftrace\] (.*?)(?: bt: (.*))?$', line.rstrip('\n'))
        if not m:
            continue
        print(m.group(1))
        if m.group(2):
            frames = [resolve(int(t[3:], 16)) if t.startswith('+0x') else '?' for t in m.group(2).split()]
            while frames and frames[-1] == '?':
                frames.pop()
            print('    ' + ' <- '.join(frames))
