"""What the game log shows just before each hitch of a phone run (Session 64: the scene-start hitches).

    py tools/android/hitch_context.py 046 [--before-ms 600] [--min-ms 100]

For each "[frames] hitch: frame N took X ms" line in the run's newest game log: the log lines of the X + before-ms
milliseconds before it, without the per-frame noise ([s59][readback], [native][half2], periodic counter lines), and a
tally of their tags. Reads timestamps from the log lines ("[YYYY-MM-DD HH:MM:SS.mmm]").
"""
import argparse
import glob
import os
import re
from collections import Counter
from datetime import datetime

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NOISE = re.compile(r'\[s59\]\[readback\]|\[native\]\[half2\]|\[s6\]\[|\[s7\]\[|\[s52\]|\[s21\]|\[native\]\[(frame|edram|wb|lean|truth\] frames)|'
                   r'\[scene\]\[crowd\]|\[wwe2k14\]\[frames\] t=|\[native\] frames')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--before-ms', type=float, default=600)
    ap.add_argument('--min-ms', type=float, default=100)
    a = ap.parse_args()
    logs = glob.glob(os.path.join(ROOT, 'out', 'android', 'runs', a.run, 'logs', 'wwe2k14_*.log'))
    path = max(logs, key=lambda p: int(re.search(r'_(\d+)\.log', p).group(1)))
    rows = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]', line)
        if m:
            rows.append((datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S.%f'), line.rstrip()))
    for i, (t, line) in enumerate(rows):
        h = re.search(r'\[frames\] hitch: frame (\d+) took (\d+) ms', line)
        if not h or float(h.group(2)) < a.min_ms:
            continue
        window = (float(h.group(2)) + a.before_ms) / 1000
        ctx = [(tt, ll) for tt, ll in rows[max(0, i - 400):i] if 0 <= (t - tt).total_seconds() <= window and not NOISE.search(ll)]
        tags = Counter(re.sub(r'^.*?\] \[\w+\] \[t\d+\] (\[[^\]]+\](\[[^\]]+\])?).*$', r'\1', ll) for _, ll in ctx)
        print(f'== hitch frame {h.group(1)}: {h.group(2)} ms at {t:%H:%M:%S.%f}'[:-3] + f'; {len(ctx)} lines: '
              + ', '.join(f'{k} {v}' for k, v in tags.most_common(6)))
        for tt, ll in ctx[-12:]:
            body = re.sub(r'^\[[^\]]+\] \[\w+\] ', '', ll)
            print(f'   -{(t - tt).total_seconds() * 1000:5.0f} ms {body[:170]}')


if __name__ == '__main__':
    main()
