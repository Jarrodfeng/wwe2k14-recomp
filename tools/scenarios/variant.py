"""Session 30: a copy of a captured scenario with some bytes of its match description M changed (probing what a field
does: launch the copy and look).

  py tools/scenarios/variant.py SRC NEW --set 332=1 [--set 21=0 ...] [--copy-from M.bin --range 12-76 ...] [--dir out/scenarios]

OFFSET (decimal or 0x hex) = a byte offset in M (NAME.m.bin, big-endian guest bytes; layout docs/systems/match_setup.md),
VALUE = the new byte (0-255). Writes NEW.m.bin / .g.bin / .a.bin / .x.bin and NEW.toml with the hashes and the name
updated (the summary keeps the source's values: it is for reading only). Launch with --wwe2k14_launch=NEW
--wwe2k14_launch_restore_ga=false (otherwise the source's captured G / A are written back after apply and hide the
change). Local only, like every capture (copies of game memory: never commit).
"""
import argparse
import os
import re
import shutil
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('src')
    ap.add_argument('new')
    ap.add_argument('--set', action='append', default=[], help='OFFSET=BYTE in M')
    ap.add_argument('--copy-from', help='another M snapshot (e.g. logs/scenario_NNN_*_apply_entry_M.bin) for --range')
    ap.add_argument('--range', action='append', default=[], help='A-B: copy M bytes A..B-1 from --copy-from')
    ap.add_argument('--dir', default=os.path.join(REPO, 'out', 'scenarios'))
    a = ap.parse_args()
    d = a.dir
    if os.path.exists(os.path.join(d, a.new + '.toml')):
        sys.exit('%s exists' % a.new)
    m = bytearray(open(os.path.join(d, a.src + '.m.bin'), 'rb').read())
    changes = []
    if a.range:
        other = open(a.copy_from, 'rb').read()
        assert len(other) == len(m), 'snapshot size differs from M'
        for r in a.range:
            lo, hi = (int(x, 0) for x in r.split('-'))
            n = sum(1 for i in range(lo, hi) if m[i] != other[i])
            m[lo:hi] = other[lo:hi]
            changes.append('M+%d..%d from %s (%d bytes differed)' % (lo, hi - 1, os.path.basename(a.copy_from), n))
    for s in a.set:
        off, val = s.split('=')
        off, val = int(off, 0), int(val, 0)
        changes.append('M+%d: %d -> %d' % (off, m[off], val))
        m[off] = val
    open(os.path.join(d, a.new + '.m.bin'), 'wb').write(m)
    for ext in ('g', 'a', 'x'):
        shutil.copyfile(os.path.join(d, '%s.%s.bin' % (a.src, ext)), os.path.join(d, '%s.%s.bin' % (a.new, ext)))
    t = open(os.path.join(d, a.src + '.toml'), encoding='utf-8').read()
    t = t.replace('name = "%s"' % a.src, 'name = "%s"' % a.new)
    t = t.replace('file = "%s.' % a.src, 'file = "%s.' % a.new)
    t = re.sub(r'(m = \{ file = "[^"]+", size = \d+, fnv1a64 = ")[0-9A-F]+(")',
               lambda mo: mo.group(1) + '%016X' % fnv1a64(m) + mo.group(2), t)
    t = '# VARIANT of %s (tools/scenarios/variant.py): %s\n' % (a.src, '; '.join(changes)) + t
    open(os.path.join(d, a.new + '.toml'), 'w', encoding='utf-8', newline='\n').write(t)
    print('%s: %s' % (a.new, '; '.join(changes)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
