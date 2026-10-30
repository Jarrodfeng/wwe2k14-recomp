# Session 73 (scene starts in slow motion): game speed in the first seconds after each load-turbo window ends (the
# moment a scene becomes visible: "[wwe2k14][turbo] ... off at frame N"), from a run's log and frames CSV. Speed =
# game time / real time over 0.5 s windows (fixed 1/60 s step per frame at word 60); also the frames of 30+ ms.
#   py tools\perf\scene_start_ab.py LOG_DIR RUN [RUN ...]
import csv
import os
import re
import sys

OFF = re.compile(r'\[turbo\] (ingame|match) off at frame (\d+)')


def main():
    d = sys.argv[1]
    for run in sys.argv[2:]:
        frames = {}
        with open(os.path.join(d, 'frames_%s.csv' % run), newline='') as f:
            for row in csv.reader(f):
                if row and row[0].isdigit():
                    frames[int(row[0])] = float(row[2])
        threshold = 'game'
        starts = []
        with open(os.path.join(d, 'wwe2k14_%s.log' % run), encoding='utf-8', errors='replace') as f:
            for line in f:
                m = OFF.search(line)
                if m:
                    starts.append((m.group(1), int(m.group(2))))
                if 'late-frame threshold set to' in line:
                    threshold = line.split('late-frame threshold set to ')[1].split(' ')[0]
        print('== run %s (late-frame threshold %s)' % (run, threshold))
        for kind, start in starts:
            cells, n, slow, total_ms, count = [], start, 0, 0.0, 0
            for _ in range(4):
                t, c = 0.0, 0
                while t < 500.0 and n in frames:
                    t += frames[n]
                    c += 1
                    slow += frames[n] >= 30.0
                    n += 1
                if not c:
                    break
                total_ms += t
                count += c
                cells.append('%.2f' % (c * 16.667 / t))
            print('  %-6s frame %5d: speed per 0.5 s %s | first 2 s: %.2f, frames >= 30 ms: %d' % (
                kind, start, ' '.join(cells), count * 16.667 / total_ms if total_ms else 0, slow))


if __name__ == '__main__':
    main()
