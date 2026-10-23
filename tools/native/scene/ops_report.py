"""The op probe's run, summarised (Session 50, docs\\native\\SCENE_MAP.md; probe probes\\probes.cpp, route
short\\s50_ops_probe.txt).

Per probe window ([s50][ops] / [s50][rec] lines, split by the route's "census:" markers): records per frame by op
(with the switch_map handler and its db name), by recording list, and the op-15 / op-19 recorders' and the model-part
draw's callers (return address -> the containing function and its db name, tools\\q). Then each captured frame's op
sequence ([s50][seq]) cut into passes at the ops that clear or resolve (1, 64, 65, 73, 74, 77, 78: handler_reach.py),
one line per pass: ops in it (counts) and which lists.

    py tools\\native\\scene\\ops_report.py RUN [--exe coverage]
"""
import argparse
import collections
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
CUT = {1, 64, 65, 73, 74, 77, 78}  # ops reaching d3d_clear / d3d_resolve (handler_reach.py, depth 3)


def containing(addr):
    out = subprocess.run([sys.executable, os.path.join('tools', 'q', 'q.py'), 'addr', f'0x{addr:08X}'], cwd=ROOT,
                         capture_output=True, text=True).stdout
    m = re.search(r'in (sub_[0-9A-F]{8})', out) or re.search(r'(sub_[0-9A-F]{8})', out)
    return m.group(1) if m else '?'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run', type=int)
    ap.add_argument('--exe', default='coverage')
    a = ap.parse_args()
    log = os.path.join(ROOT, 'out', 'build', f'win-amd64-{a.exe}', 'logs', f'wwe2k14_{a.run:03d}.log')
    sys.path.insert(0, HERE)
    from switch_map import generated_switch, names
    db = names()
    cases = generated_switch('0x82BB53D0')
    scene = 'start'
    ops_windows = collections.defaultdict(list)
    seqs = []
    callers = collections.defaultdict(lambda: collections.defaultdict(list))
    lists = collections.defaultdict(list)
    for line in open(log, encoding='utf-8', errors='replace'):
        m = re.search(r'census: ([a-z ]+)', line)
        if m:
            scene = m.group(1).strip().split(' (')[0]
        m = re.search(r'\[s50\]\[ops\] frames \d+-\d+, records per frame by op:(.*)', line)
        if m:
            ops_windows[scene].append({int(o): float(v) for o, v in re.findall(r' (\d+):([0-9.]+)', m.group(1))})
        m = re.search(r'\[s50\]\[ops\] records per frame by list:(.*)', line)
        if m:
            lists[scene].append(m.group(1).strip())
        m = re.search(r'\[s50\]\[rec\] (.+?) callers per frame:(.*)', line)
        if m:
            for addr, v in re.findall(r' ([0-9A-F]{8}):([0-9.]+)', m.group(2)):
                callers[scene][(m.group(1), int(addr, 16))].append(float(v))
        m = re.search(r'\[s50\]\[seq\] frame (\d+) part \d+:(.*)', line)
        if m:
            if not seqs or seqs[-1][0] != (scene, m.group(1)):
                seqs.append([(scene, m.group(1)), []])
            seqs[-1][1] += [(int(l), int(o), int(n or 1)) for l, o, n in re.findall(r' L(\d+):(\d+)(?:x(\d+))?', m.group(2))]
    for scene, windows in ops_windows.items():
        print(f'## {scene}: {len(windows)} windows of 60 frames')
        mean = collections.Counter()
        for w in windows:
            for op, v in w.items():
                mean[op] += v / len(windows)
        print('| op | records / frame | handler (case -> first call) |')
        print('|---|---|---|')
        for op, v in sorted(mean.items(), key=lambda x: -x[1]):
            print(f'| {op} | {v:.1f} | 0x{cases.get(op, 0):08X} |')
        if lists[scene]:
            print(f'lists (last window): {lists[scene][-1]}')
        print()
        print('| hook | caller (return address) | in | calls / frame |')
        print('|---|---|---|---|')
        rows = sorted(((k, sum(v) / len(windows)) for k, v in callers[scene].items()), key=lambda x: (x[0][0], -x[1]))
        for (hook, addr), v in rows:
            fn = containing(addr)
            name = db.get(int(fn[4:], 16), '') if fn.startswith('sub_') else ''
            print(f'| {hook} | 0x{addr:08X} | {fn} {name} | {v:.1f} |')
        print()
    for (scene, frame), seq in seqs:
        print(f'## frame {frame} ({scene}): {sum(n for _, _, n in seq)} records, cut at clears / resolves')
        passes, cur = [], collections.Counter()
        cur_lists = set()
        for l, op, n in seq:
            cur[op] += n
            cur_lists.add(l)
            if op in CUT:
                passes.append((cur, cur_lists))
                cur, cur_lists = collections.Counter(), set()
        if cur:
            passes.append((cur, cur_lists))
        for k, (c, ls) in enumerate(passes):
            draws = c[15] + c[19] + c[68]
            ends = [op for op in c if op in CUT]
            body = ' '.join(f'{op}x{n}' for op, n in sorted(c.items()))
            print(f'{k:3d}: draws {draws:4d} (15:{c[15]} 19:{c[19]} 68:{c[68]}) ends {ends} lists {sorted(ls)} | {body}')
        print()


if __name__ == '__main__':
    main()
