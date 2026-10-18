"""Compare two arms of whole runs (Session 19: the CCD test and other alternating A/B batches).

    py tools/perf/ab_runs.py --a 1440,1442,1444 --b 1441,1443,1445 [--names "all cores,CCD0"]

Per scene (Brock / second entrance / match, anchored on the file opens like rate_check.py): presented fps (fast-forward)
or frame-time p99 (real time), and CP busy ms per frame from out/bench/reports/cpframes_NNN.csv (run
`py tools/bench/bench_report.py report NNN` first; this script runs it when the CSV is missing). Prints each run, then
mean and spread per arm and the difference B - A."""
import argparse
import csv
import os
import re
import statistics
import subprocess
import sys

L = r'C:/Users/Jarrod/wwe2k14-recomp/out/build/win-amd64-release/logs'
R = r'C:/Users/Jarrod/wwe2k14-recomp/out/bench/reports'
REPO = r'C:/Users/Jarrod/wwe2k14-recomp'


def scenes_of(run):
    anch = {}
    for line in open(f'{L}/wwe2k14_{run}.log', encoding='utf-8', errors='replace'):
        m = re.search(r"\[file\] \S+ '([^']+)'.*frame=(\d+)", line)
        if m:
            p = m.group(1).lower()
            for k in ('343.bik2', '103.bik2', '324.bik2', 'm3.pac'):
                if k in p and k not in anch:
                    anch[k] = int(m.group(2))
    frames = [(int(r['frame']), float(r['t_seconds']), float(r['frame_ms']))
              for r in csv.DictReader(open(f'{L}/frames_{run}.csv'))]
    second = anch.get('324.bik2') or anch.get('103.bik2')
    out = {}
    if '343.bik2' in anch and second:
        out['Brock'] = (anch['343.bik2'], second)
    if second and 'm3.pac' in anch:
        out['second'] = (second, anch['m3.pac'])
    if 'm3.pac' in anch:
        out['match'] = (anch['m3.pac'] + 200, frames[-1][0])  # +200: past the held / fast load frames
    res = {}
    if not os.path.exists(f'{R}/cpframes_{run}.csv'):
        subprocess.run([sys.executable, 'tools/bench/bench_report.py', 'report', run], cwd=REPO,
                       capture_output=True)
    cp = []
    if os.path.exists(f'{R}/cpframes_{run}.csv'):
        for r in csv.DictReader(open(f'{R}/cpframes_{run}.csv', newline='')):
            try:
                cp.append((int(r['guest_frame']), float(r['busy'])))
            except ValueError:
                pass
    for name, (f0, f1) in out.items():
        sel = [f for f in frames if f0 < f[0] <= f1]
        wall = sel[-1][1] - sel[0][1]
        ms = sorted(f[2] for f in sel)
        busy = [b for g, b in cp if f0 < g <= f1]
        res[name] = {'fps': (len(sel) - 1) / wall, 'p99': ms[int(len(ms) * 0.99)],
                     'busy': statistics.mean(busy) if busy else float('nan')}
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--a', required=True)
    ap.add_argument('--b', required=True)
    ap.add_argument('--names', default='A,B')
    a = ap.parse_args()
    na, nb = a.names.split(',')
    arms = {na: [x.strip().zfill(3) for x in a.a.split(',')], nb: [x.strip().zfill(3) for x in a.b.split(',')]}
    data = {arm: {run: scenes_of(run) for run in runs} for arm, runs in arms.items()}
    for arm, runs in data.items():
        for run, res in runs.items():
            print(f'{arm:10} {run}: ' + '  '.join(f'{s} {v["fps"]:.1f} fps p99 {v["p99"]:.2f} CP {v["busy"]:.2f}'
                                               for s, v in res.items()))
    print()
    for scene in ('Brock', 'second', 'match'):
        line = f'{scene:7}'
        means = {}
        for arm in (na, nb):
            for key in ('fps', 'p99', 'busy'):
                vals = [r[scene][key] for r in data[arm].values() if scene in r]
                if not vals:
                    continue
                m = statistics.mean(vals)
                sd = statistics.stdev(vals) if len(vals) > 1 else 0.0
                means[(arm, key)] = m
                line += f'  {arm} {key} {m:.2f}±{sd:.2f}'
        if (na, 'fps') in means and (nb, 'fps') in means:
            line += (f'  | B-A fps {means[(nb, "fps")] - means[(na, "fps")]:+.2f} '
                     f'({100 * (means[(nb, "fps")] / means[(na, "fps")] - 1):+.1f}%), '
                     f'CP {means[(nb, "busy")] - means[(na, "busy")]:+.2f} ms')
        print(line)


if __name__ == '__main__':
    main()
