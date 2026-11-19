"""S103 CMB: writes one attire's composer inputs from the player's own files (read-only) for tools\\assets\\attire_compose_check.exe.

    py -I tools\\assets\\attire_compose_dump.py --id 249 --slot 1 --out out\\cmb\\in_249_1 [--colours-slot N] [--raw-offset N]

Writes OUT\\base.dds (entry 12), OUT\\mask13.dds .. mask16.dds (the entries the PACH has) and OUT\\colours.bin (48 bytes = slot --colours-slot,
default --slot, of misc01_Start.pac p0/600 at record +64 + 48 s: tools\\assets\\attire_build.py colour_table).
The PACH is the first (attire_build.first_archive). Game-derived: under out\\ only, never committed.
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import attire_build as ab  # noqa: E402


def plain_dds(blob):
    at = blob.find(b'DDS ', 0, 0x100)
    if at < 0:
        raise ValueError('no DDS header')
    return blob[at:]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--id', type=int, required=True)
    ap.add_argument('--slot', type=int, required=True)
    ap.add_argument('--colours-slot', type=int)
    ap.add_argument('--raw-offset', type=int, help='planted fault: take the 48 colour bytes from record + this offset (the old wrong layout\'s attire 1 = 244)')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    table = ab.colour_table()
    slots = table.get(a.id)
    if slots is None:
        raise SystemExit('no colour record for %d' % a.id)
    cs = a.slot if a.colours_slot is None else a.colours_slot
    cols = slots[cs]
    if a.raw_offset is not None:
        data = ab.pac.load(os.path.join(ab.pac.GAME, 'pac', 'misc01_Start.pac'))
        tab = next(dec for name, dec in ab.pac.entries(data) if name == 'p0/600')
        k = [i for i in range(struct.unpack_from('<H', tab, 0)[0]) if struct.unpack_from('<H', tab, 2 + ab.RECORD * i)[0] == a.id][0]
        raw = tab[2 + ab.RECORD * k + a.raw_offset: 2 + ab.RECORD * k + a.raw_offset + 48]
        cols = [tuple(raw[3 * i: 3 * i + 3]) for i in range(16)]
    if cols is None:
        raise SystemExit('slot %d of %d is unused (all 0xFF)' % (cs, a.id))
    entries = ab.first_archive(a.id)
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, 'base.dds'), 'wb') as f:
        f.write(plain_dds(entries[12]))
    for e in (13, 14, 15, 16):
        p = os.path.join(a.out, 'mask%d.dds' % e)
        if e in entries:
            with open(p, 'wb') as f:
                f.write(plain_dds(entries[e]))
        elif os.path.exists(p):
            os.remove(p)
    with open(os.path.join(a.out, 'colours.bin'), 'wb') as f:
        f.write(bytes(v for c in cols for v in c))
    print('dumped %d slot %d (colours slot %d): masks %s' % (a.id, a.slot, cs, [e for e in (13, 14, 15, 16) if e in entries]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
