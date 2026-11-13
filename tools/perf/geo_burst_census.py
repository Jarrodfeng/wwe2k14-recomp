r"""What the entrance's geometry looks like per frame (lane 273): inline draws, distinct vertex ranges, bytes.

  py -I tools\perf\geo_burst_census.py CAPTURE_DIR [--top N]

Reads frames\frame_*.bin of a capture folder (CAPTURE_FORMAT.md): per frame the draws with kDrawInline (flag 1), their count,
vertex bytes, distinct (vertex_offset) values, how many offsets are NEW against all earlier frames of the capture (content-addressed:
a new offset = geometry the reader had to build + store for the first time), and the largest single inline draw.
Prints median / p95 / max of each and the frames with the most new bytes. Local only, reads game data.
"""
import argparse, struct, sys
from pathlib import Path

DRAW_FMT = '<QQQ' + 'I' * 8 + 'QQ' + 'IIII' + 'BBB5s'  # adapter_frame.h struct Draw, 96 bytes
DRAW_SIZE = struct.calcsize(DRAW_FMT)
SEC_DRAWS_ID = None

def sections(b):
    magic, ver, cf, sc, gf, pi, fl = struct.unpack_from('<IIIIQII', b, 0)
    out = {}
    for s in range(sc):
        sid, es, cnt, off = struct.unpack_from('<IIQQ', b, 32 + s * 24)
        out[sid] = (es, cnt, off)
    return out

def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))] if v else 0

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('--top', type=int, default=8)
    ap.add_argument('--draws-id', type=int, default=4)
    a = ap.parse_args()
    files = sorted((Path(a.capture) / 'frames').glob('frame_*.bin'))
    seen = set()
    rows = []
    for f in files:
        b = f.read_bytes()
        sec = sections(b)
        es, cnt, off = sec.get(a.draws_id, (0, 0, 0))
        if es != DRAW_SIZE:
            print('draw size mismatch', es, DRAW_SIZE); return 2
        n_inl = bytes_inl = new_inl = new_bytes = big = 0
        ids = set(); n_idx = new_idx = 0
        for i in range(cnt):
            d = struct.unpack_from(DRAW_FMT, b, off + i * es)
            vo, io, vb, ib, vc = d[11], d[12], d[13], d[14], d[15]
            flags = d[19]
            if flags & 8:
                continue
            if flags & 1:
                n_inl += 1; bytes_inl += vb; big = max(big, vb)
                if vo not in seen and vo not in ids:
                    new_inl += 1; new_bytes += vb
                ids.add(vo)
            else:
                n_idx += 1
                if vo not in seen and vo not in ids:
                    new_idx += 1
                ids.add(vo)
        seen |= ids
        rows.append((f.name, cnt, n_inl, bytes_inl, new_inl, new_bytes, big, n_idx, new_idx))
    print('frames', len(rows))
    for name, k in (('draws', 1), ('inline draws', 2), ('inline vertex bytes', 3), ('NEW inline ranges', 4), ('new inline bytes', 5), ('largest inline draw', 6), ('indexed draws', 7), ('NEW indexed ranges', 8)):
        v = [r[k] for r in rows]
        print(f'{name:22s} median {pct(v, .5):>9} p95 {pct(v, .95):>9} max {max(v):>9}')
    print('frames with the most new inline bytes:')
    for r in sorted(rows, key=lambda r: -r[5])[:a.top]:
        print('  ', r)

if __name__ == '__main__':
    sys.exit(main())
