"""Session 91: the match builder's numeric spec (src/scenario.h --wwe2k14_match) for a menu capture, for the byte check.

  py tools/atlas/spec_from_capture.py NAME [NAME ...] [--dir out/scenarios]

Reads the capture's own summary (NAME.toml: match type, arena, the participant setter calls in finalize's order with
their controller argument) and M's pace word (NAME.m.bin +528, big-endian). Seats 40-42 (referee, commentators) are
the game's, not ours. Royal Rumble types (20-23): every M entry's id as the entrant list (the capture keeps the drawn
order, not the screen's picks: a built Rumble can match its roster, not its order). Local captures only (game memory).
"""
import argparse
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def spec_for(name, d):
    toml = open(os.path.join(d, name + '.toml'), encoding='utf-8').read()
    m = open(os.path.join(d, name + '.m.bin'), 'rb').read()
    typ = int(re.search(r'^match_type = (-?\d+)', toml, re.M).group(1))
    arena = int(re.search(r'^arena = (-?\d+)', toml, re.M).group(1))
    pace = struct.unpack_from('>i', m, 528)[0]
    parts = {}
    for args in re.findall(r'fn = "participant_set"\nargs = \[([^\]]*)\]', toml):
        a = [int(x) for x in args.split(',')]
        if a[0] < 40 and a[1] < 500:
            parts[a[0]] = (a[1], a[2])
    spec = 'type=%d;arena=%d;pace=%d' % (typ, arena, pace)
    # The capture's whole option block (M+332..423 = R+8..99, O[0..91]; type setter sub_822CA9C0 copies it): a build
    # then no longer depends on the per-type options stored in the profile (build_diffs.md, the sixpack's O[3]).
    spec += ';o=' + ','.join('%d:%d' % (k, m[332 + k]) for k in range(92))
    if typ in (39, 40):  # Elimination Chamber: each seat's pod as captured (E+72)
        spec += ';pods=' + ','.join(str(struct.unpack_from('>i', m, 724 + 3580 * i + 72)[0]) for i in range(6))
    if 20 <= typ <= 23:
        ids = [struct.unpack_from('>I', m, 724 + 3580 * i + 56)[0] for i in range(40)]
        spec += ';rumble=' + ','.join(str(i) for i in ids if i < 500)
    elif parts:
        spec += ';p=' + ','.join('%d:%d' % parts[i] for i in sorted(parts))
    return spec


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('names', nargs='+')
    ap.add_argument('--dir', default=os.path.join(REPO, 'out', 'scenarios'))
    a = ap.parse_args()
    for n in a.names:
        print('%s\t%s' % (n, spec_for(n, a.dir)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
