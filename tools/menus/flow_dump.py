"""Dump the menu flow table's records from a --wwe2k14_snapshot_trigger / snapshot_now snapshot (Session 28).

  py tools/menus/flow_dump.py RUN NAME [MENU_ID ...]

The SmartMenu's flow table (docs/systems/menus.md, "The menu flow table"): holder [0x839294DC], T = data + 4,
T+0 record count, T+4 records (76 bytes), T+84 / T+88 the index. Prints, per record, its menu id, item index and the
first 19 words (+0 record id, +4 parent record id, +24 label text id, +40 flags, +44 screen code). No menu ids = all.
Snapshots are local game memory (logs/snap_RUN_NAME_D.bin / _H.bin): nothing printed here is ever committed.
"""
import mmap
import os
import struct
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
LOGS = os.path.join(ROOT, 'out', 'build', 'win-amd64-release', 'logs')
D_LO, D_HI = 0x835C0000, 0x83A60000
H_LO = 0xA0000000


class Snap:
    def __init__(self, run, name):
        self.fd = open(os.path.join(LOGS, f'snap_{run}_{name}_D.bin'), 'rb')
        self.fh = open(os.path.join(LOGS, f'snap_{run}_{name}_H.bin'), 'rb')
        self.d = mmap.mmap(self.fd.fileno(), 0, access=mmap.ACCESS_READ)
        self.h = mmap.mmap(self.fh.fileno(), 0, access=mmap.ACCESS_READ)

    def bytes(self, a, n):
        if D_LO <= a and a + n <= D_HI:
            return self.d[a - D_LO:a - D_LO + n]
        if H_LO <= a and a - H_LO + n <= len(self.h):
            return self.h[a - H_LO:a - H_LO + n]
        return None

    def u32(self, a):
        b = self.bytes(a, 4)
        return None if b is None else struct.unpack('>I', b)[0]

    def s32(self, a):
        b = self.bytes(a, 4)
        return None if b is None else struct.unpack('>i', b)[0]

    def u16(self, a):
        b = self.bytes(a, 2)
        return None if b is None else struct.unpack('>H', b)[0]


def main():
    run, name = sys.argv[1], sys.argv[2]
    menus = [int(x, 0) for x in sys.argv[3:]]
    s = Snap(run, name)
    holder = s.u32(0x839294DC)
    data = s.u32(holder + 20) or s.u32(holder + 16)
    T = data + 4
    count, recs = s.u32(T), s.u32(T + 4)
    print(f'holder {holder:#x} state {s.u32(holder + 24)} table {T:#x} count {count} records {recs:#x} '
          f'index count {s.u32(T + 84)} index {s.u32(T + 88):#x}')
    for i in range(count):
        r = recs + 76 * i
        mid, idx = s.s32(r + 8), s.u16(r + 12)
        if menus and mid not in menus:
            continue
        words = [s.u32(r + 4 * k) for k in range(19)]
        print(i, 'menu', mid, 'item', idx, ' '.join(f'{w:08X}' for w in words))


if __name__ == '__main__':
    main()
