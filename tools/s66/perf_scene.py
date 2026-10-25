"""Per-scene performance numbers of LAUNCHED runs, and an alternating base / base+flag comparison (Session 66).

Reads what perf_scene.ps1 produced (frames_NNN.csv, threads_NNN.csv from tools\\perf\\thread_busy.ps1, the run log, and
`bench_report.py report NNN` for the scene ranges, GPU ms and command-thread busy ms). Per scene (Brock's entrance
tron_343, Bryan's tron_324, the match, Undertaker's tron_103 when the run has it): fps, frames over 20 / 33 ms, GPU ms
per frame, command-thread (GPU Commands) busy ms per CP frame, and CPU ms per guest frame of every thread (the
thread sampler is 1 Hz: a scene's edge seconds are pro-rated, so per-thread figures carry a few percent of noise).
Several runs per arm: medians. With --flag runs: the delta of each median (flag minus base, and percent).
Fast-forwarded runs: fps is the PC's speed, not the game's 30/60; compare arms, not against 60.

    py tools\\s66\\perf_scene.py --base 4137[,4138,...] [--flag 4139,...] [--label-flag "--x=1"] [--top 8]

Output: text tables on stdout only (nothing is written). Used by perf_scene.ps1; run it by hand on old runs too.
"""
import argparse
import csv
import re
import statistics
import subprocess
import sys
from collections import defaultdict
from datetime import datetime, timedelta

R = r'C:\Users\Jarrod\wwe2k14-recomp'
L = R + r'\out\build\win-amd64-release\logs'
SCENE = re.compile(r'^\s{2}(tron_\d+|match)\s+(\d+)-(\d+)\s+(\d+)\s+([\d.]+).*?\bgpu ([\d.]+)')
MARK = re.compile(r'\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\].*\[input\] line \d+(?: \([^)]*\))?: scene=(\S+).*\(frame (\d+)\)')
TITLE = {'tron_343': "Brock's entrance", 'tron_324': "Bryan's entrance", 'tron_103': "Undertaker's entrance (WM XXX)",
         'match': 'match'}
ORDER = ('tron_343', 'tron_324', 'tron_103', 'match')


def pad(n):
    return str(n).zfill(3)


def thread_key(name, tid):
    if name in ('', '-'):
        return f'tid {tid}'
    m = re.search(r'\((F[0-9A-F]+)\)', name)
    if m and name.startswith('XThread'):
        return f'XThread ({m.group(1)})'  # the tid in the name changes per run, the guest handle does not
    return name


def load_run(n):
    n = pad(n)
    rep = subprocess.run([sys.executable, R + r'\tools\bench\bench_report.py', 'report', n], capture_output=True,
                         text=True, encoding='utf-8', errors='replace').stdout
    scenes = {}
    for line in rep.splitlines():
        m = SCENE.match(line)
        if m:
            scenes[m.group(1)] = dict(f0=int(m.group(2)), f1=int(m.group(3)), busy=float(m.group(5)), gpu=float(m.group(6)))
    frames = {}
    for r in csv.DictReader(open(f'{L}/frames_{n}.csv')):
        frames[int(r['frame'])] = (float(r['t_seconds']), float(r['frame_ms']))
    mark = None
    for line in open(f'{L}/wwe2k14_{n}.log', encoding='utf-8', errors='replace'):
        m = MARK.search(line)
        if m:
            mark = (datetime.strptime(m.group(1)[:23], '%Y-%m-%d %H:%M:%S.%f'), int(m.group(3)))
            break
    rows = []
    try:
        for r in csv.DictReader(open(f'{L}/threads_{n}.csv', newline='')):
            rows.append((datetime.strptime(r['time'], '%H:%M:%S.%f').time(), thread_key(r['name'], r['tid']),
                         float(r['cpu_ms']), float(r['interval_ms'])))
    except FileNotFoundError:
        pass
    out = {}
    for name, s in scenes.items():
        ms = [frames[f][1] for f in range(s['f0'], s['f1'] + 1) if f in frames]
        if not ms:
            continue
        d = dict(frames=len(ms), fps=1000 * len(ms) / sum(ms), o20=sum(m > 20 for m in ms), o33=sum(m > 33 for m in ms),
                 gpu=s['gpu'], busy=s['busy'], thr={}, cpu=float('nan'))
        if rows and mark and mark[1] in frames and s['f0'] in frames and s['f1'] in frames:
            def t_of(f):
                return mark[0] + timedelta(seconds=frames[f][0] - frames[mark[1]][0])
            a, b = t_of(s['f0']), t_of(s['f1'])
            cpu = defaultdict(float)
            for tm, key, cms, iv in rows:
                end = datetime.combine(a.date(), tm)
                start = end - timedelta(milliseconds=iv)
                ov = (min(end, b) - max(start, a)).total_seconds() * 1000
                if ov > 0 and iv > 0:
                    cpu[key] += cms * ov / iv
            d['thr'] = {k: v / len(ms) for k, v in cpu.items()}
            d['cpu'] = sum(d['thr'].values())
        out[name] = d
    return out


def med(vals):
    return statistics.median(vals) if vals else float('nan')


def arm(runs):
    data = [load_run(r) for r in runs]
    res = {}
    for sc in set().union(*[d.keys() for d in data]) if data else []:
        ds = [d[sc] for d in data if sc in d]
        e = {k: med([x[k] for x in ds]) for k in ('frames', 'fps', 'o20', 'o33', 'gpu', 'busy', 'cpu')}
        e['n'] = len(ds)
        keys = set().union(*[x['thr'].keys() for x in ds])
        e['thr'] = {k: med([x['thr'].get(k, 0.0) for x in ds]) for k in keys}
        res[sc] = e
    return res


def show(title, res, top):
    print(f'### {title}')
    for sc in [s for s in ORDER if s in res]:
        e = res[sc]
        print(f'{TITLE[sc]} [{sc}, {e["n"]} run(s), median, {e["frames"]:.0f} frames]: {e["fps"]:.1f} fps, '
              f'>20 ms {e["o20"]:.0f}, >33 ms {e["o33"]:.0f}, GPU {e["gpu"]:.2f} ms, command-thread busy {e["busy"]:.2f} ms, '
              f'all-thread CPU {e["cpu"]:.2f} ms/frame')
        for k, v in sorted(e['thr'].items(), key=lambda kv: -kv[1])[:top]:
            print(f'    {k:34} {v:6.2f} ms/frame')
    print()


def compare(a, b, label, top):
    print(f'### DELTA: base+{label} minus base (negative ms = faster, positive fps = faster)')
    for sc in [s for s in ORDER if s in a and s in b]:
        x, y = a[sc], b[sc]

        def d(k, nd=2):
            dv = y[k] - x[k]
            pc = 100 * dv / x[k] if x[k] else float('nan')
            return f'{k} {x[k]:.{nd}f} -> {y[k]:.{nd}f} ({dv:+.{nd}f}, {pc:+.1f}%)'
        print(f'{TITLE[sc]}: ' + '; '.join([d('fps', 1), d('gpu'), d('busy'), d('cpu')]) +
              f'; >20 ms {x["o20"]:.0f} -> {y["o20"]:.0f}')
        keys = sorted(set(x['thr']) | set(y['thr']), key=lambda k: -abs(y['thr'].get(k, 0) - x['thr'].get(k, 0)))[:top]
        for k in keys:
            u, v = x['thr'].get(k, 0), y['thr'].get(k, 0)
            print(f'    {k:34} {u:6.2f} -> {v:6.2f} ms/frame ({v - u:+.2f})')
    print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', required=True)
    ap.add_argument('--flag', default='')
    ap.add_argument('--label-flag', default='flag')
    ap.add_argument('--top', type=int, default=8)
    a = ap.parse_args()
    base = arm([int(x) for x in a.base.split(',')])
    show(f'BASE runs {a.base}', base, a.top)
    if a.flag:
        fl = arm([int(x) for x in a.flag.split(',')])
        show(f'BASE + {a.label_flag}, runs {a.flag}', fl, a.top)
        compare(base, fl, a.label_flag, a.top)


if __name__ == '__main__':
    main()
