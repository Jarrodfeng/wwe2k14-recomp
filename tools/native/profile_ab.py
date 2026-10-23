"""Where an in-run A/B's CPU time goes, per function (Session 47).

Splits a profiled A/B run's samples (--wwe2k14_profile_gpu_commands=true --wwe2k14_profile_stacks=true, a route that
logs "ab: mode M" like short\\s46_truth_ab.txt, --wwe2k14_file_trace=true for the scenes) by scene and mode, and lists
per function the inclusive and self time per guest frame in each mode and the difference (ms per frame = samples x
the sampling period / frames; the sampler's period is nominal, so compare the modes, not absolute numbers).

    py tools\\native\\profile_ab.py RUN [--scene match] [--thread "GPU Commands"] [--top 40] [--skip 10]
                                    [--self] [--match TEXT] [--focus NAME]

--self sorts by the self-time difference (the leaf), the default by the inclusive difference.
--focus NAME: only samples through a function containing NAME, broken down by its direct callee.
Frames are guest frames at sample time; the command thread runs 2-3 frames behind, so the first --skip frames
after each switch are left out (both modes alike).
"""
import argparse
import os
import re
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
LOGS = os.path.join(HERE, '..', '..', 'out', 'build', 'win-amd64-release', 'logs')
MODE = re.compile(r'\[input\] line \d+: ab: mode (\d) \(frame (\d+)\)')
OPEN = re.compile(r"\[file\] NtCreateFile '([^']+)'.* frame=(\d+)")
SCENES = (('343.bik2', 'brock'), ('324.bik2', 'bryan'), ('m3.pac', 'match'))
PERIOD = re.compile(r'wwe2k14_profile_interval_ms = \'(\d+)\'')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run', type=int)
    ap.add_argument('--scene', default='match', help='brock, bryan, match or all')
    ap.add_argument('--thread', default='GPU Commands')
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--skip', type=int, default=10)
    ap.add_argument('--self', action='store_true')
    ap.add_argument('--match', default='')
    ap.add_argument('--focus', default='')
    ap.add_argument('--tail', type=int, default=120,
                    help='frames after the last switch that count (Session 52: a one-mode profile run sets it large)')
    a = ap.parse_args()

    log = os.path.join(LOGS, f'wwe2k14_{a.run:03d}.log')
    switches, scenes, period = [], [], 1.0
    seen = set()
    with open(log, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = MODE.search(line)
            if m:
                switches.append((int(m.group(2)), m.group(1)))
                continue
            m = OPEN.search(line)
            if m:
                p = m.group(1).lower()
                for key, name in SCENES:
                    if p.endswith(key) and name not in seen:
                        seen.add(name)
                        scenes.append((int(m.group(2)), name))
                continue
            m = PERIOD.search(line)
            if m:
                period = float(m.group(1))
    if not switches:
        sys.exit('no "ab: mode" lines in the log')
    last = switches[-1][0] + a.tail

    def bucket(frame):
        sw = sc = None
        for f0, v in switches:
            if f0 <= frame:
                sw = (f0, v)
        for f0, v in scenes:
            if f0 <= frame:
                sc = v
        if not sw or not sc or frame - sw[0] < a.skip or frame > last:
            return None
        if a.scene != 'all' and sc != a.scene:
            return None
        return sw[1]

    frames = Counter()
    for fr in range(switches[0][0], last + 1):
        b = bucket(fr)
        if b is not None:
            frames[b] += 1

    names, stacks = {}, {}
    per_mode = defaultdict(Counter)  # mode -> sid -> samples
    cache = {}
    with open(os.path.join(LOGS, f'stacks_{a.run:03d}.txt'), encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('n '):
                _, nid, name = line.rstrip('\n').split(' ', 2)
                names[int(nid)] = name
            elif line.startswith('s '):
                _, sid, key = line.rstrip('\n').split(' ', 2)
                parts = key.split(',')
                stacks[int(sid)] = (parts[0], [int(x) for x in parts[1:] if x])
            elif line and line[0].isdigit():
                fr, sid = line.split(' ')[:2]
                fr = int(fr)
                b = cache.get(fr)
                if b is None:
                    b = cache[fr] = bucket(fr) or '-'
                if b != '-':
                    per_mode[b][int(sid)] += 1

    modes = sorted(frames)
    print(f'run {a.run}, scene {a.scene}, thread {a.thread!r}, period {period} ms; frames per mode: '
          + ', '.join(f'{m}: {frames[m]}' for m in modes))
    incl = {m: Counter() for m in modes}
    self_ = {m: Counter() for m in modes}
    total = Counter()
    for m in modes:
        for sid, n in per_mode[m].items():
            label, ids = stacks[sid]
            if not label.lower().startswith(a.thread.lower()):
                continue
            fn = [names[i] for i in ids]
            if a.focus:
                idx = [i for i, x in enumerate(fn) if a.focus in x]
                if not idx:
                    continue
                i = idx[-1]
                callee = fn[i - 1] if i > 0 else '(self)'
                incl[m][callee] += n
                total[m] += n
                continue
            total[m] += n
            for x in set(fn):
                incl[m][x] += n
            if fn:
                self_[m][fn[0]] += n

    def ms(m, c):
        return c * period / frames[m] if frames[m] else 0.0

    print('total ms/frame: ' + ', '.join(f'mode {m} {ms(m, total[m]):.3f}' for m in modes))
    if len(modes) < 2:  # (Session 52) one mode: where its time goes
        if not modes:
            return
        m = modes[0]
        table = self_ if a.self and not a.focus else incl
        kind = 'self' if a.self and not a.focus else ('callee of ' + a.focus if a.focus else 'inclusive')
        print(f'\n{kind} ms per frame, mode {m}:')
        for k, n in table[m].most_common(a.top):
            print(f'{ms(m, n):9.3f}  {k[:130]}')
        return
    on, off = modes[-1], modes[0]
    table = self_ if a.self and not a.focus else incl
    keys = set(table[on]) | set(table[off])
    if a.match:
        keys = {k for k in keys if a.match in k}
    rows = sorted(keys, key=lambda k: -(ms(on, table[on][k]) - ms(off, table[off][k])))
    kind = 'self' if a.self and not a.focus else ('callee of ' + a.focus if a.focus else 'inclusive')
    print(f'\n{kind} ms per frame, mode {off} -> mode {on} (largest increase first):')
    print(f'{"mode " + off:>9} {"mode " + on:>9} {"delta":>8}  function')
    for k in rows[:a.top]:
        x, y = ms(off, table[off][k]), ms(on, table[on][k])
        print(f'{x:9.3f} {y:9.3f} {y - x:+8.3f}  {k[:130]}')
    print('...\nlargest decreases:')
    for k in rows[-8:]:
        x, y = ms(off, table[off][k]), ms(on, table[on][k])
        print(f'{x:9.3f} {y:9.3f} {y - x:+8.3f}  {k[:130]}')


if __name__ == '__main__':
    main()
