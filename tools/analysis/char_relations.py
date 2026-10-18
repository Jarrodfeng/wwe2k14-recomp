"""Session 16 item 9: the run-in relations of characters, read from a snapshot (--wwe2k14_snapshot_trigger: logs/
snap_NNN_NAME_D.bin = static data 0x835C0000.., _H.bin = physical memory via 0xA0000000..). Read-only; local output.

    py tools/analysis/char_relations.py SNAP_PREFIX ID [ID ...]      (SNAP_PREFIX = logs/snap_1285_s16_match)

Mapping (listings, Session 16): id -> index = the halfword at 0x835DDF90 + 2 * id (sub_822C0A78; ids >= 500 -> 512),
record = [0x8376F3C0] + 204 * index (sub_827DA788), relations = 5 signed halfwords at record + 70 (sub_822C0BF8, 512 =
none). The run-in trigger sub_8259A5A0 (non-Universe path) walks the TARGET's relations 4..0 for the interferer.
"""
import struct
import sys

D0, H0 = 0x835C0000, 0xA0000000


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    d = open(sys.argv[1] + '_D.bin', 'rb').read()
    h = open(sys.argv[1] + '_H.bin', 'rb').read()

    def du16(a):
        return struct.unpack('>H', d[a - D0:a - D0 + 2])[0]

    def du32(a):
        return struct.unpack('>I', d[a - D0:a - D0 + 4])[0]

    table = du32(0x8376F3C0)
    print('record table at 0x%08X' % table)
    for cid in map(int, sys.argv[2:]):
        idx = du16(0x835DDF90 + 2 * cid) if cid < 500 else 512
        if idx == 512:
            print('%d: no record' % cid)
            continue
        rec = table + 204 * idx
        if D0 <= rec < D0 + len(d) - 204:  # the table is in the image's static data (run 1285: 0x8376F3CC)
            buf, o = d, rec - D0
        elif rec >= H0 and rec - H0 + 204 <= len(h):
            buf, o = h, rec - H0
        else:
            print('%d: index %d, record 0x%08X outside the snapshot' % (cid, idx, rec))
            continue
        rel = struct.unpack('>5h', buf[o + 70:o + 80])
        print('%d: index %d, record 0x%08X, relations %s' % (cid, idx, rec, [r for r in rel]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
