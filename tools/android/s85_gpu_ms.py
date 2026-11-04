# Session 85: GPU / GPU-thread CPU / game CPU ms per frame and the frame rate over a phone run's match window (the last
# 2300 guest frames before the route's `quit`, the route is fixed). py tools\android\s85_gpu_ms.py 192 193 194
import csv
import glob
import os
import re
import statistics
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def newest(run, pattern):
    files = glob.glob(os.path.join(ROOT, 'out', 'android', 'runs', run, 'logs', pattern))
    return max(files, key=lambda p: int(re.search(r'_(\d+)\.', p).group(1))) if files else None


def quit_frame(run):
    src = newest(run, 'wwe2k14_*.log')
    for line in open(src, errors='replace') if src else []:
        m = re.search(r'line \d+: quit.*\(frame (\d+)\)', line)
        if m:
            return int(m.group(1))
    return None


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))]


print('run   frames   fps   gpu_ms mean / p50 / p95     gpu-thread cpu mean / p95     game cpu mean')
for run in sys.argv[1:]:
    q = quit_frame(run)
    path = newest(run, 'frames_*.csv')
    if q is None or not path:
        print(run, 'no quit mark / frames')
        continue
    g, c, gc, t = [], [], [], []
    for r in csv.DictReader(open(path, newline='')):
        try:
            f = int(r['frame'])
        except ValueError:
            continue
        if not (q - 2300 <= f <= q):
            continue
        if r['frame_ms']:
            t.append(float(r['frame_ms']))
        if r['gpu_ms']:
            g.append(float(r['gpu_ms']))
        if r['gpu_thread_cpu_ms']:
            c.append(float(r['gpu_thread_cpu_ms']))
        if r['game_cpu_ms']:
            gc.append(float(r['game_cpu_ms']))
    if not g or len(t) < 2:
        print(run, 'no gpu_ms')
        continue
    fps = 1000.0 / statistics.mean(t)  # frame_ms per frame: the frame rate over the window
    print(f'{run}  {len(g):6d}  {fps:5.1f}   {statistics.mean(g):6.2f} / {statistics.median(g):6.2f} / {pct(g, .95):6.2f}'
          f'     {statistics.mean(c) if c else 0:6.2f} / {pct(c, .95) if c else 0:6.2f}     {statistics.mean(gc) if gc else 0:6.2f}')
