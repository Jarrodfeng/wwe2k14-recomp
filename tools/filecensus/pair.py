#!/usr/bin/env python3
"""Lane 257: a vk | rdr | abs-difference strip of one region, upscaled, for reading a tile fault by eye (local only: game pictures, under out\\).
  py -I tools\\filecensus\\pair.py VK.png RDR.png OUT.png [x0 y0 x1 y1] [scale]"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'scene'))
import selftest  # noqa: E402


def main():
    vk, rdr, out = sys.argv[1:4]
    a = selftest.read_png(vk)
    b = selftest.read_png(rdr)
    w, h = a[0], a[1]
    x0, y0, x1, y1 = (int(v) for v in sys.argv[4:8]) if len(sys.argv) >= 8 else (0, 0, w, h)
    s = int(sys.argv[8]) if len(sys.argv) >= 9 else 1
    rows = []
    for y in range(y0, y1):
        line = []
        for src_i in range(3):
            for x in range(x0, x1):
                if src_i < 2:
                    img = a if src_i == 0 else b
                    o = x * img[3]
                    px = img[2][y][o:o + 3]
                else:
                    pa, pb = a[2][y][x * a[3]:x * a[3] + 3], b[2][y][x * b[3]:x * b[3] + 3]
                    px = bytes(min(255, 3 * abs(pa[i] - pb[i])) for i in range(3))
                line.extend(px * s)
            line.extend(bytes([255, 0, 255]) * s) if src_i < 2 else None
        for _ in range(s):
            rows.append(bytes(line))
    selftest.write_png(out, len(rows[0]) // 3, len(rows), rows)
    print(out)


if __name__ == '__main__':
    main()
