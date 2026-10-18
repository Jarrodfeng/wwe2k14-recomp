"""Resolve a crash log's `wwe2k14.exe+0xOFFSET` host frames to symbols via the linker map, and the guest
context's return addresses / lr / last_indirect_target to the containing sub_ function.

Usage: py tools/analysis/resolve_backtrace.py <log file> [map file]
The map must come from the same link as the exe that crashed (check its timestamp line). Guest addresses
are matched to the nearest function start at or below them from generated/default/wwe2k14_funcs.h
(local codegen output, never committed).
"""
import bisect
import glob
import re
import sys

log_path = sys.argv[1]
map_path = sys.argv[2] if len(sys.argv) > 2 else r'out\build\win-amd64-release\wwe2k14.map'

guest_starts = sorted(
    int(m, 16)
    for path in glob.glob(r'generated\default\wwe2k14_funcs.h')
    for m in re.findall(r'DECLARE_REX_FUNC\(sub_([0-9A-Fa-f]{8})\)', open(path, encoding='utf-8').read()))


def guest_name(address):
    i = bisect.bisect_right(guest_starts, address) - 1
    if i < 0 or not address:
        return '?'
    return f'sub_{guest_starts[i]:08X}+0x{address - guest_starts[i]:X}'

base = 0x140000000
symbols = []  # (rva, name)
with open(map_path, encoding='utf-8', errors='replace') as f:
    for line in f:
        if 'Preferred load address is' in line:
            base = int(line.split()[-1], 16)
        m = re.match(r'\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s', line)
        if m:
            symbols.append((int(m.group(2), 16) - base, m.group(1)))
symbols.sort()
rvas = [s[0] for s in symbols]

# Only the last crash block in the log.
with open(log_path, encoding='utf-8', errors='replace') as f:
    lines = f.read().splitlines()
start = max((i for i, l in enumerate(lines) if '[wwe2k14][crash]' in l), default=None)
if start is None:
    sys.exit('no [wwe2k14][crash] block in the log')
print(lines[start][:200])
for line in lines[start + 1:]:
    m = re.match(r'\s*#(\d+) (\S+)\+0x([0-9A-Fa-f]+)', line)
    if not m:
        break
    frame, module, offset = m.group(1), m.group(2), int(m.group(3), 16)
    if module.lower() != 'wwe2k14.exe':
        print(f'#{frame:>2} {module}+0x{offset:X}')
        continue
    i = bisect.bisect_right(rvas, offset) - 1
    name = f'{symbols[i][1]}+0x{offset - symbols[i][0]:X}' if i >= 0 else '?'
    print(f'#{frame:>2} wwe2k14.exe+0x{offset:X}  {name}')

# Guest context block (crash_handler.h GuestContextReport), if present.
context = next((i for i in range(start, len(lines)) if '[wwe2k14][crash] guest context:' in lines[i]), None)
if context is not None:
    if not guest_starts:
        print('(no generated/default/wwe2k14_funcs.h: guest addresses left unresolved)')
    for line in lines[context:context + 16]:
        m = re.search(r'lr=([0-9A-F]{8})\s+ctr=([0-9A-F]{8})\s+last_indirect_target=([0-9A-F]{8})', line)
        if m:
            for label, value in zip(('lr', 'ctr', 'last_indirect_target'), m.groups()):
                print(f'guest {label:<20} {value}  {guest_name(int(value, 16))}')
        m = re.search(r'return addresses:((?: [0-9A-F]{8})*)', line)
        if m:
            for depth, value in enumerate(m.group(1).split()):
                print(f'guest frame #{depth:>2}        {value}  {guest_name(int(value, 16))}')
