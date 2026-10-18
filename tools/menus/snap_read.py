"""Read guest words / follow pointer chains in --wwe2k14_snapshot_trigger snapshots (Session 27 item 2).

  py tools/menus/snap_read.py RUN NAME[,NAME...] CHAIN [CHAIN ...]
     CHAIN = 0xADDR[>OFF[>OFF...]] as in states.toml (each '>' dereferences, then adds OFF; decimal or 0x)
  py tools/menus/snap_read.py RUN NAME --pointers-to 0xLO 0xHI [--static]
     every word (heap, or with --static the image data) whose value lies in [LO, HI): who points into an object.

Snapshots: logs/snap_RUN_NAME_D.bin (guest 0x835C0000..0x83A60000) and _H.bin (0xA0000000..0xC0000000). Local only.
"""
import os
import sys

import numpy as np

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
LOGS = os.path.join(ROOT, 'out', 'build', 'win-amd64-release', 'logs')
D_LO, D_HI = 0x835C0000, 0x83A60000
H_LO, H_HI = 0xA0000000, 0xC0000000


class Snap:
    def __init__(self, run, name):
        self.d = np.memmap(os.path.join(LOGS, f'snap_{run}_{name}_D.bin'), dtype=np.uint8, mode='r')
        self.h = np.memmap(os.path.join(LOGS, f'snap_{run}_{name}_H.bin'), dtype=np.uint8, mode='r')

    def u32(self, a):
        if D_LO <= a < D_HI - 3:
            b = self.d[a - D_LO:a - D_LO + 4]
        elif H_LO <= a < H_HI - 3:
            b = self.h[a - H_LO:a - H_LO + 4]
        else:
            return None
        return int.from_bytes(bytes(b), 'big')


def chain(s, text):
    parts = text.split('>')
    a = int(parts[0], 0)
    for off in parts[1:]:
        v = s.u32(a)
        if v is None:
            return None
        a = v + int(off, 0)
    return s.u32(a), a


def main():
    run, names = sys.argv[1], sys.argv[2].split(',')
    if len(sys.argv) > 3 and sys.argv[3] == '--pointers-to':
        lo, hi = int(sys.argv[4], 0), int(sys.argv[5], 0)
        static = '--static' in sys.argv
        for n in names:
            s = Snap(run, n)
            raw = s.d if static else s.h
            base = D_LO if static else H_LO
            w = np.frombuffer(raw[:len(raw) // 4 * 4], dtype='>u4')
            idx = np.nonzero((w >= lo) & (w < hi))[0]
            print(f'{n}: {len(idx)} word(s) pointing into [{lo:#x}, {hi:#x})')
            for i in idx[:60]:
                print(f'  0x{base + int(i) * 4:08X} -> 0x{int(w[i]):08X}')
        return
    for n in names:
        s = Snap(run, n)
        out = []
        for c in sys.argv[3:]:
            r = chain(s, c)
            out.append(f'{c} = ' + ('unreadable' if r is None or r[0] is None else f'{r[0]} (0x{r[0]:08X} @0x{r[1]:08X})'))
        print(f'{n}: ' + ' | '.join(out))


if __name__ == '__main__':
    main()
