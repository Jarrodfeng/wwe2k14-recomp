"""Session 92 (step 6): per-scene fps of profile_lesnar_bryan runs - Brock's entrance (343.bik2 open -> 324.bik2 open),
Bryan's entrance (324.bik2 open -> M3.PAC open), the match (M3.PAC + 300 -> + 2,000) - from the run's log + frames CSV.

  py tools/census/bench_scenes.py RUN [RUN ...] [--logs DIR] [--names A,B,C]

Prints average fps, 5% low and 1% low per scene and the GPU ms average where the CSV has it. Relative numbers between
arms of the same route and seed; uncapped (--vsync=false) runs overstate gains against normal-speed fps (LESSONS).
"""
import argparse
import csv
import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FILE_RE = re.compile(r"NtCreateFile '([^']+)'.*frame=(\d+)")


def marks(log):
    m = {}
    with open(log, encoding='utf-8', errors='replace') as f:
        for line in f:
            if 'NtCreateFile' not in line:
                continue
            g = FILE_RE.search(line)
            if not g:
                continue
            name, frame = g.group(1).upper(), int(g.group(2))
            for key, pat in (('brock', '343.BIK2'), ('bryan', '324.BIK2'), ('m3', '\\M3.PAC')):
                if key not in m and name.endswith(pat):
                    m[key] = frame
    return m


def stats(rows, a, b):
    v = sorted(ms for fr, ms, _ in rows if a <= fr <= b)
    if not v:
        return None
    g = [gm for fr, _, gm in rows if a <= fr <= b and gm is not None]
    avg = 1000.0 * len(v) / sum(v)
    low = lambda p: 1000.0 / (sum(v[-max(1, len(v) * p // 100):]) / max(1, len(v) * p // 100))
    return avg, low(5), low(1), (sum(g) / len(g) if g else None), len(v)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('runs', nargs='+')
    ap.add_argument('--logs', default=os.path.join(REPO, 'out', 'build', 'win-amd64-release', 'logs'))
    ap.add_argument('--names', default='')
    a = ap.parse_args()
    names = a.names.split(',') if a.names else a.runs
    for run, nm in zip(a.runs, names):
        n = int(run)
        log = os.path.join(a.logs, 'wwe2k14_%03d.log' % n)
        csvp = os.path.join(a.logs, 'frames_%03d.csv' % n)
        m = marks(log)
        rows = []
        with open(csvp, newline='') as f:
            for r in csv.DictReader(f):
                if r['frame_ms']:
                    rows.append((int(r['frame']), float(r['frame_ms']), float(r['gpu_ms']) if r.get('gpu_ms') else None))
        print('%s (run %d) marks %s' % (nm, n, m))
        scenes = []
        if 'brock' in m and 'bryan' in m:
            scenes.append(('brock entrance', m['brock'], m['bryan'] - 1))
        if 'bryan' in m and 'm3' in m:
            scenes.append(('bryan entrance', m['bryan'], m['m3'] - 1))
        if 'm3' in m:
            scenes.append(('match', m['m3'] + 300, m['m3'] + 2000))
        for label, f0, f1 in scenes:
            s = stats(rows, f0, f1)
            if s:
                print('  %-15s frames %6d-%6d  avg %7.1f fps  5%% low %7.1f  1%% low %7.1f  gpu %s ms  (%d frames)' % (
                    label, f0, f1, s[0], s[1], s[2], ('%.2f' % s[3]) if s[3] is not None else '-', s[4]))


if __name__ == '__main__':
    main()
