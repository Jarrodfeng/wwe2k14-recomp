"""Frame-time summary of phone runs over one time window, or per route segment.

    py tools/android/demo_fps.py 026 027 [--from 55] [--to 115]     (attract demo: a time window, seconds)
    py tools/android/demo_fps.py 029 030 --marks                     (a route's marks: one row per segment)

Reads each run's newest logs/frames_NNN.csv (the phone's logs folder keeps older runs' files too: the newest number is
this run's). Frames under --min-ms (8) are loading-screen frames and left out of time windows. --marks: the segments
between the route's `log` lines that carry a frame number ("... (frame N)", the input script adds it), e.g. Cena's
entrance (scene=tron_139 -> scene=tron_103) and Undertaker's (scene=tron_103 -> bench: end).
Per row: frames, mean fps, p50 / p95 / worst frame ms, frames over 20 / 33 ms.
"""
import argparse
import csv
import glob
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def newest(run, pattern):
    files = glob.glob(os.path.join(ROOT, 'out', 'android', 'runs', run, 'logs', pattern))
    return max(files, key=lambda p: int(re.search(r'_(\d+)\.', p).group(1))) if files else None


def load_frames(path):
    rows = []
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            try:
                rows.append((int(r['frame']), float(r['t_seconds']), float(r['frame_ms'])))
            except (ValueError, KeyError, TypeError):  # (a cut-off last line)
                break
    return rows


def marks(run):
    """[(label, frame)] from the run's game log: input-script log lines with a frame number."""
    path = newest(run, 'wwe2k14_*.log')
    out = []
    if not path:
        return out
    # newline='\n': a route with CRLF line endings logs its labels with a '\r' on the phone (no text-mode read there);
    # Python's universal newlines would split those log lines before their frame number (Session 69).
    with open(path, encoding='utf-8', errors='replace', newline='\n') as f:
        for line in f:
            m = re.search(r'\[input\]\S* (?:line \d+: )?(.*?)\r? \(frame (\d+)\)', line)
            if m and not m.group(1).startswith(('waiting for', 'saw ')):  # the route's own log lines only
                out.append((m.group(1).strip(), int(m.group(2))))
    return out


def row(label, ms):
    if not ms:
        return f'{label:<34} no frames'
    s = sorted(ms)
    fps = len(ms) / (sum(ms) / 1000)
    return (f'{label:<34} {len(ms):6d} {fps:6.1f} {s[len(s) // 2]:6.1f} {s[int(len(s) * 0.95)]:6.1f} {s[-1]:7.1f} '
            f'{sum(1 for x in ms if x > 20):6d} {sum(1 for x in ms if x > 33.4):6d} {sum(1 for x in ms if x > 100):6d} {sum(ms) / 1000:6.1f}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('runs', nargs='+')
    ap.add_argument('--from', dest='t_from', type=float, default=55)
    ap.add_argument('--to', dest='t_to', type=float, default=115)
    ap.add_argument('--min-ms', type=float, default=8, help='skip faster frames (loading screens present at 200+ fps)')
    ap.add_argument('--marks', action='store_true', help='one row per segment between the route marks')
    a = ap.parse_args()
    print(f'{"run / segment":<34} {"frames":>6} {"fps":>6} {"p50":>6} {"p95":>6} {"worst":>7} {">20ms":>6} {">33ms":>6} {">100ms":>6} {"secs":>6}')
    for run in a.runs:
        path = newest(run, 'frames_*.csv')
        if not path:
            print(f'{run} no frames csv')
            continue
        frames = load_frames(path)
        if not a.marks:
            print(row(f'{run} {a.t_from:.0f}-{a.t_to:.0f} s',
                      [v for _, t, v in frames if a.t_from <= t <= a.t_to and v >= a.min_ms]))
            continue
        ms = marks(run)
        if len(ms) < 2:
            print(f'{run} fewer than two marks with frame numbers in the log')
            continue
        for (l0, f0), (l1, f1) in zip(ms, ms[1:]):
            print(row(f'{run} {l0[:14]} -> {l1[:14]}', [v for fr, _, v in frames if f0 <= fr < f1]))


if __name__ == '__main__':
    main()
