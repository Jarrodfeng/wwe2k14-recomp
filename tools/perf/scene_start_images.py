# Session 73 (the long frame at every scene start): per load-turbo end (a scene appearing), the longest frames in the
# next 2 s and the truth images made in the next 3 s of log ("[native][truth] image ..." lines: new allocations).
#   py tools\perf\scene_start_images.py LOG_DIR RUN[=LABEL] ...
import csv
import datetime
import os
import re
import sys

OFF = re.compile(r'\[turbo\] (ingame|match) off at frame (\d+)')


def stamp(line):
    try:
        return datetime.datetime.strptime(line[1:24], '%Y-%m-%d %H:%M:%S.%f')
    except ValueError:
        return None


def main():
    d = sys.argv[1]
    for arg in sys.argv[2:]:
        run, _, label = arg.partition('=')
        frames = {}
        with open(os.path.join(d, 'frames_%s.csv' % run), newline='') as f:
            for row in csv.reader(f):
                if row and row[0].isdigit():
                    frames[int(row[0])] = float(row[2])
        with open(os.path.join(d, 'wwe2k14_%s.log' % run), encoding='utf-8', errors='replace') as f:
            lines = f.read().splitlines()
        cells, total = [], sum('[native][truth] image ' in l for l in lines)
        for i, line in enumerate(lines):
            m = OFF.search(line)
            if not m:
                continue
            t0 = stamp(line)
            made = 0
            for later in lines[i:]:
                t = stamp(later)
                if t and t0 and (t - t0).total_seconds() > 3.0:
                    break
                made += '[native][truth] image ' in later
            start = int(m.group(2))
            top = sorted((frames.get(n, 0) for n in range(start, start + 120)), reverse=True)[:3]
            cells.append('%s: longest %s ms, images made %d' % (m.group(1), '/'.join('%.0f' % x for x in top), made))
        print('%s %-12s %s | whole run %d' % (run, label, '; '.join(cells), total))


if __name__ == '__main__':
    main()
