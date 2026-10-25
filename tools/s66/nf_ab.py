"""In-run A/B of CPU-side switches (Session 66, track F2): per-thread CPU ms per guest frame with a set of hot cvars
on vs off, from ONE launched run (no separate-run noise; the run must still have the PC to itself).

route: writes tools\\input_scripts\\short\\s66_nfab_NAME.txt - launched Lesnar vs Bryan; in Bryan's entrance and the
       match, CYCLES x (cvars on, "ab: mode 1", WINDOW frames; cvars off, "ab: mode 0", WINDOW frames).
report: reads the run's log (ab: mode switches with their wall-clock time and frame) and logs\\threads_NNN.csv (1 Hz
       per-thread CPU from tools\\perf\\thread_busy.ps1, started with the run by tools\\s66\\nf_ab.ps1); samples that
       straddle a switch are dropped. Prints per thread: CPU ms per guest frame in mode 0 and 1 and the change, and fps.

    py tools\\s66\\nf_ab.py route NAME "cvar=on/off" ["cvar=on/off" ...] [--window 1500] [--cycles 4]
    py tools\\s66\\nf_ab.py report RUN [--top 8]
"""
import argparse
import csv
import datetime as dt
import os
import re
import sys
from collections import defaultdict

R = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SHORT = os.path.join(R, 'tools', 'input_scripts', 'short')
LOGS = os.path.join(R, 'out', 'build', 'win-amd64-release', 'logs')
MODE = re.compile(r'^\[(\d{4}-\d\d-\d\d) (\d\d:\d\d:\d\d\.\d+)\].*\[input\] line \d+: ab: mode (\d) \(frame (\d+)\)')
SCENE = re.compile(r'^\[(\d{4}-\d\d-\d\d) (\d\d:\d\d:\d\d\.\d+)\].*\[input\] line \d+: (scene=\w+|bench: done) \(frame (\d+)\)')


def route(a):
    pairs = []
    for item in a.cvars:
        cvar, values = item.split('=', 1)
        on, off = values.split('/', 1)
        pairs.append((cvar, on, off))
    src = open(os.path.join(SHORT, 'profile_lesnar_bryan.txt'), encoding='utf-8').read().splitlines()
    nav = src[src.index('wait_file SaveData.Dat 3600'):src.index('wait_file 343.bik2 1800')]
    body = []
    for marker, timeout, scene, settle in (('324.bik2', 20000, 'tron_324', 60), ('M3.PAC', 30000, 'match', 300)):
        body += [f'wait_file {marker} {timeout}', f'log scene={scene}', f'wait_frames {settle}']
        for _ in range(a.cycles):
            body += [f'cvar {c} {on}' for c, on, _ in pairs] + ['log ab: mode 1', f'wait_frames {a.window}']
            body += [f'cvar {c} {off}' for c, _, off in pairs] + ['log ab: mode 0', f'wait_frames {a.window}']
        body += ['log scene=done_' + scene]
    out = [f'# harness: realtime (Session 66 in-run CPU A/B {a.name}, tools\\s66\\nf_ab.py; pass --vsync=false)',
           '# ' + ' '.join(f'{c}={on}/{off}' for c, on, off in pairs),
           '# Use with --wwe2k14_launch=lesnar_bryan --wwe2k14_preset_low=true. on_fail quit.',
           'on_fail quit', 'log bench: start nf_ab (launch)'] + nav + body + ['log bench: done', 'quit']
    path = os.path.join(SHORT, f's66_nfab_{a.name}.txt')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out) + '\n')
    print(path)


def stamp(day, hms):
    return dt.datetime.strptime(f'{day} {hms[:12]}', '%Y-%m-%d %H:%M:%S.%f')


def report(a):
    log = os.path.join(LOGS, f'wwe2k14_{a.run:03d}.log')
    switches, scene_marks = [], []  # (time, mode, frame)
    with open(log, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = MODE.match(line)
            if m:
                switches.append((stamp(m.group(1), m.group(2)), int(m.group(3)), int(m.group(4))))
                continue
            m = SCENE.match(line)
            if m:
                scene_marks.append((stamp(m.group(1), m.group(2)), m.group(3), int(m.group(4))))
    if len(switches) < 2:
        sys.exit('fewer than 2 "ab: mode" switches in the log')
    day = switches[0][0].date()
    # Windows: from each switch to the next (or the next scene mark).
    ends = sorted([s[0] for s in switches] + [m[0] for m in scene_marks])
    frame_at = {s[0]: s[2] for s in switches}
    frame_at.update({m[0]: m[2] for m in scene_marks})
    windows = []
    for t, mode, frame in switches:
        nxt = next((e for e in ends if e > t), None)
        if nxt is not None:
            windows.append((t, nxt, mode, frame_at[nxt] - frame))
    frames = defaultdict(int)
    secs = defaultdict(float)
    for t0, t1, mode, n in windows:
        frames[mode] += n
        secs[mode] += (t1 - t0).total_seconds()
    cpu = defaultdict(lambda: defaultdict(float))  # thread -> mode -> cpu ms (samples inside one window)
    path = os.path.join(LOGS, f'threads_{a.run}.csv')
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            end = dt.datetime.combine(day, dt.datetime.strptime(row['time'][:12], '%H:%M:%S.%f').time())
            start = end - dt.timedelta(milliseconds=float(row['interval_ms']))
            for t0, t1, mode, _ in windows:
                if t0 <= start and end <= t1:
                    name = re.sub(r'XThread[0-9A-F]+ ', 'XThread ', row['name'])
                    cpu[name][mode] += float(row['cpu_ms'])
                    break
    print(f'run {a.run}: {len(switches)} switches; mode 0: {frames[0]} frames in {secs[0]:.1f} s ({frames[0] / max(secs[0], 1e-9):.1f} fps), '
          f'mode 1: {frames[1]} frames in {secs[1]:.1f} s ({frames[1] / max(secs[1], 1e-9):.1f} fps)')
    print('CPU ms per guest frame (1 Hz samples wholly inside a window; frames per window from the log):')
    print(f'{"thread":34} {"off":>7} {"on":>7} {"change":>8}')
    # Per-frame: the samples kept cover part of each window; scale by kept-time / window-time per mode.
    kept = defaultdict(float)
    with open(path, newline='') as f:
        rows = list(csv.DictReader(f))
    first_name = rows[0]['name'] if rows else ''
    for row in rows:
        if row['name'] != first_name:
            continue
        end = dt.datetime.combine(day, dt.datetime.strptime(row['time'][:12], '%H:%M:%S.%f').time())
        start = end - dt.timedelta(milliseconds=float(row['interval_ms']))
        for t0, t1, mode, _ in windows:
            if t0 <= start and end <= t1:
                kept[mode] += float(row['interval_ms']) / 1000.0
                break
    rate = {m: frames[m] / max(secs[m], 1e-9) for m in (0, 1)}  # fps per mode
    ranked = sorted(cpu, key=lambda n: -(cpu[n][0] + cpu[n][1]))
    for name in ranked[:a.top]:
        per = {m: cpu[name][m] / max(kept[m] * rate[m], 1e-9) for m in (0, 1)}
        ch = 100.0 * (per[1] - per[0]) / per[0] if per[0] else 0.0
        print(f'{name[:34]:34} {per[0]:7.3f} {per[1]:7.3f} {ch:+7.1f}%')


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('route')
    r.add_argument('name')
    r.add_argument('cvars', nargs='+')
    r.add_argument('--window', type=int, default=1500)
    r.add_argument('--cycles', type=int, default=4)
    p = sub.add_parser('report')
    p.add_argument('run', type=int)
    p.add_argument('--top', type=int, default=8)
    a = ap.parse_args()
    route(a) if a.cmd == 'route' else report(a)


if __name__ == '__main__':
    main()
