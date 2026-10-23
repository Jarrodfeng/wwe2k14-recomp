"""What each render-command handler reaches in the D3D library (Session 50, docs\\native\\SCENE_MAP.md).

For each handler address (the switch_map.py table's calls), the library functions (0x82CA0000-0x82CCFFFF, the
statically linked Direct3D library) it calls within --depth levels of direct calls (tools\\q callees; indirect calls
counted, not followed), with db names. Static only: a reach is a possibility, not a run (the runtime census says what
runs).

    py tools\\native\\scene\\handler_reach.py out\\native\\s50_switch_map.md [--depth 3]
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
LIB = (0x82CA0000, 0x82CD0000)


def q_json(args):
    out = subprocess.run([sys.executable, os.path.join('tools', 'q', 'q.py'), '--json'] + args, cwd=ROOT,
                         capture_output=True, text=True).stdout
    try:
        return json.loads(out)
    except Exception:
        return None


_cache = {}


def callees(addr):
    if addr in _cache:
        return _cache[addr]
    out = subprocess.run([sys.executable, os.path.join('tools', 'q', 'q.py'), 'callees', f'0x{addr:08X}'], cwd=ROOT,
                         capture_output=True, text=True).stdout
    found, indirect = [], 0
    for line in out.splitlines():
        m = re.match(r'\s+sub_([0-9A-F]{8})\s', line)
        if m:
            found.append(int(m.group(1), 16))
        m = re.search(r'indirect calls \(bctrl\) (\d+)', line)
        if m:
            indirect = int(m.group(1))
    _cache[addr] = (found, indirect)
    return _cache[addr]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('table')
    ap.add_argument('--depth', type=int, default=3)
    a = ap.parse_args()
    sys.path.insert(0, HERE)
    from switch_map import names
    db = names()
    rows = []
    for line in open(a.table, encoding='utf-8'):
        m = re.match(r'\| (\d+) \| 0x([0-9A-F]+)[^|]*\| (.*) \|', line)
        if not m:
            continue
        op = int(m.group(1))
        handlers = [int(h, 16) for h in re.findall(r'0x([0-9A-F]{8})', m.group(3))]
        lib, indirect, seen = set(), 0, set()
        frontier = [h for h in handlers if not (LIB[0] <= h < LIB[1])]
        lib.update(h for h in handlers if LIB[0] <= h < LIB[1])
        for _ in range(a.depth):
            nxt = []
            for f in frontier:
                if f in seen:
                    continue
                seen.add(f)
                cs, ind = callees(f)
                indirect += ind
                for c in cs:
                    if LIB[0] <= c < LIB[1]:
                        lib.add(c)
                    else:
                        nxt.append(c)
            frontier = nxt
        desc = ', '.join(f'0x{x:08X}' + (f' {db[x]}' if x in db else '') for x in sorted(lib))
        rows.append(f'| {op} | {desc or "-"} | {indirect} |')
    print('| op | library functions reached (depth %d) | indirect calls on the way |' % a.depth)
    print('|---|---|---|')
    print('\n'.join(rows))


if __name__ == '__main__':
    main()
