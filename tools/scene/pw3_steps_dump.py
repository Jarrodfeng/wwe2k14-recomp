#!/usr/bin/env python3
"""S103 PW-3: dumps a capture's stream draws of the ring-steps object (engine object 0xAA141900, docs\\renderer\\RING_PROMOTION_MAP.md 1.5) for the weapon drawer's unit.
  py -I tools\\scene\\pw3_steps_dump.py CAPTURE_DIR OUT.txt [--object 0xAA141900] [--frames 40]
One line per draw at the colour pass site: `D frame draw part r0..r11` (VS c76..c78 as float bits, hex). Reads the capture's frames only (0 game runs); only numbers leave it."""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'skip'))
from ledger_io import Frame  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('out')
    ap.add_argument('--object', default='0xAA141900')
    ap.add_argument('--frames', type=int, default=40, help='how many capture frames to scan (from the first)')
    a = ap.parse_args()
    want = int(a.object, 16)
    d = os.path.join(a.capture, 'frames')
    names = sorted(x for x in os.listdir(d) if x.startswith('frame_') and x.endswith('.bin'))[:a.frames]
    lines = []
    for n in names:
        f = Frame(os.path.join(d, n))
        if not f.has_origins:
            continue
        for i in range(f.ndraws):
            o = f.origin(i)
            if not o or o['object'] != want or o['klass'] != 1:
                continue
            bank = f.bank(f.draw(i)['vs_bank'])
            rows = [bank[4 * r + k] for r in (76, 77, 78) for k in range(4)]
            lines.append('D %d %d %d %s' % (f.cap_frame, i, o['part'], ' '.join('%08x' % b for b in rows)))
    with open(a.out, 'w', encoding='utf-8') as fh:
        fh.write('\n'.join(lines) + ('\n' if lines else ''))
    print('%d draws of object %08x in %d frames -> %s' % (len(lines), want, len(names), a.out))
    return 0 if lines else 1


if __name__ == '__main__':
    sys.exit(main())
