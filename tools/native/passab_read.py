r"""Session 72 part 4: read a passab_route.py run - per scene, the GPU frame (first pass to last, gpu_native_truth_pass_log)
with the A values versus the B values: mean, median and the change, plus passes per frame.

    py -I tools\native\passab_read.py RUN [--dir LOGDIR]
"""
import argparse
import os
import re
import statistics
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOTAL = re.compile(r'\[native\]\[pass\] frame (\d+): (\d+) timed segments, ([0-9.]+) ms in them, ([0-9.]+) ms between '
                   r'them, ([0-9.]+) ms first to last')
MARK = re.compile(r'passlog: (\w+)_(a|b)\b')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--dir', default=os.path.join(ROOT, 'out', 'build', 'win-amd64-release', 'logs'))
    a = ap.parse_args()
    cur = None
    data = defaultdict(lambda: defaultdict(list))  # scene -> a/b -> [(ftl, segments)]
    with open(os.path.join(a.dir, f'wwe2k14_{a.run}.log'), encoding='utf-8', errors='replace') as f:
        for line in f:
            m = MARK.search(line)
            if m:
                cur = (m.group(1), m.group(2))
                continue
            m = TOTAL.search(line)
            if m and cur:
                data[cur[0]][cur[1]].append((float(m.group(5)), int(m.group(2))))
    print(f'run {a.run}: GPU frame first pass to last (ms), A vs B')
    print(f'{"scene":8s} {"n A/B":>7s} {"mean A":>8s} {"mean B":>8s} {"med A":>8s} {"med B":>8s} {"change (med)":>13s}'
          f' {"segs A/B":>10s}')
    for scene, d in data.items():
        fa = [x[0] for x in d['a']]
        fb = [x[0] for x in d['b']]
        if not fa or not fb:
            continue
        ma, mb = statistics.median(fa), statistics.median(fb)
        sa = statistics.mean(x[1] for x in d['a'])
        sb = statistics.mean(x[1] for x in d['b'])
        print(f'{scene:8s} {len(fa):3d}/{len(fb):<3d} {statistics.mean(fa):8.3f} {statistics.mean(fb):8.3f} {ma:8.3f}'
              f' {mb:8.3f} {100 * (ma - mb) / mb:+12.1f}% {sa:5.1f}/{sb:<5.1f}')


if __name__ == '__main__':
    main()
