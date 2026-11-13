r"""Per-frame new-geometry series of a capture (lane 273): which frames build many NEW inline / indexed ranges, against the frame's timing.csv row.

  py -I tools\perf\geo_burst_series.py CAPTURE_DIR TIMING_CSV [--top N]

New = a vertex_offset not seen in any earlier frame of the capture (geometry.bin is content-addressed, so a new offset is a build the reader had to store).
Prints the percentiles of inline draws / new inline per frame, the frames with the most new ranges, and the mean new count of the frames whose timing step is slow.
"""
import argparse
import csv
import statistics
import struct
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
sys.path.insert(0, str(here))
from geo_burst_census import DRAW_FMT, sections  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('timing')
    ap.add_argument('--top', type=int, default=12)
    a = ap.parse_args()
    tim = {int(r['guest_frame']): r for r in csv.DictReader(open(a.timing))}
    seen = set()
    rows = []
    for f in sorted((Path(a.capture) / 'frames').glob('frame_*.bin')):
        b = f.read_bytes()
        sec = sections(b)
        es, cnt, off = sec[4]
        gf = struct.unpack_from('<Q', b, 16)[0]
        ninl = newinl = nidx = newidx = 0
        for i in range(cnt):
            d = struct.unpack_from(DRAW_FMT, b, off + i * es)
            vo = d[11]
            fl = d[19]
            if fl & 8:
                continue
            new = vo not in seen
            seen.add(vo)
            if fl & 1:
                ninl += 1
                newinl += new
            else:
                nidx += 1
                newidx += new
        t = tim.get(gf, {})
        rows.append((gf, ninl, newinl, nidx, newidx, float(t.get('step_ms') or 0), float(t.get('render_ms') or 0)))

    def pc(k, p):
        v = sorted(r[k] for r in rows)
        return v[min(len(v) - 1, int(len(v) * p))]

    print(len(rows), 'frames; columns: guest_frame, inline, NEW inline, indexed, NEW indexed, step_ms, render_ms')
    print('inline per frame p50/p95/max', pc(1, .5), pc(1, .95), pc(1, 1))
    print('new inline per frame p50/p95/p99/max', pc(2, .5), pc(2, .95), pc(2, .99), pc(2, 1))
    for r in sorted(rows, key=lambda r: -r[2])[:a.top]:
        print(r)
    slow = [r for r in rows if r[5] > 30]
    if slow:
        print('frames with step_ms > 30:', len(slow), 'their mean NEW inline', round(statistics.mean(r[2] for r in slow), 1),
              'all frames', round(statistics.mean(r[2] for r in rows), 1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
