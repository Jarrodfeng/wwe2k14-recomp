#!/usr/bin/env python3
"""The effect coverage of every arena on the rdr backend (offline, 0 game runs): loads each arena in one hidden viewer run, takes a small
shot of each, and reads the front-end's per-effect coverage lines from the log. Prints the union: which material classes the files use and
whether each is "bound to our effect" or "no effect yet" (and the draws it covers).

  py -I tools\\scene\\arena_coverage.py [--root DIR] [--ids 0,1,5]   (default: every arena the viewer lists)
Writes out\\s101\\scene\\coverage_all.txt (of --root). Counts and class names only; nothing from the game's files is kept."""
import argparse
import json
import os
import re
import subprocess
import sys

MAIN = os.environ.get('RDR_MAIN_CHECKOUT') or os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')))
    ap.add_argument('--ids', default='')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    exe = os.path.join(root, 'out', 'tools', 'wwe2k14_assets_rdr.exe')
    game = os.path.join(MAIN, 'game_data', 'extracted')
    for need in (exe, game):
        if not os.path.exists(need):
            print(f'FAIL  cannot read its input: {need}')
            return 1
    outdir = os.path.join(root, 'out', 's101', 'scene')
    os.makedirs(outdir, exist_ok=True)
    ids = [int(x) for x in args.ids.split(',') if x]
    if not ids:
        script = os.path.join(outdir, 'cov_list.jsonl')
        with open(script, 'w') as f:
            f.write('{"cmd":"list","kind":"arena"}\n{"cmd":"quit"}\n')
        out = subprocess.run([exe, '--backend', 'rdr', '--game', game, '--script', script, '--exit', '--hidden'], capture_output=True, text=True, timeout=120).stdout
        ids = json.loads(out.splitlines()[0])['ids']
    script = os.path.join(outdir, 'cov_all.jsonl')
    shot = os.path.join(outdir, 'cov_shot.png')
    with open(script, 'w') as f:
        for i in ids:
            f.write(json.dumps({'cmd': 'load', 'kind': 'arena', 'id': i}) + '\n')
            f.write(json.dumps({'cmd': 'camera', 'preset': 'ring'}) + '\n')
            f.write(json.dumps({'cmd': 'shot', 'path': shot, 'w': 160, 'h': 90}) + '\n')
        f.write('{"cmd":"quit"}\n')
    log = os.path.join(outdir, 'cov_all.err.txt')
    with open(os.path.join(outdir, 'cov_all.out.txt'), 'w') as o, open(log, 'w') as e:
        subprocess.run([exe, '--backend', 'rdr', '--game', game, '--script', script, '--exit', '--hidden'], stdout=o, stderr=e, timeout=3000)
    text = open(log, errors='replace').read()
    shots = len(re.findall(r'^\[rdr\] shot ', text, re.M))
    if shots != len(ids):
        print(f'FAIL  {shots} shots for {len(ids)} arenas; see {log}')
        return 1
    rows = {}
    reasons = {}
    for m in re.finditer(r'^  (\S+)\s+(bound to our effect|no effect yet)\s+(.*?)\s+\((\d+) draws', text, re.M):
        eff, state, detail, n = m.group(1), m.group(2), m.group(3), int(m.group(4))
        r = rows.setdefault(eff, [state, 0, 0])
        r[1] += n
        r[2] += 1
        if state != 'bound to our effect':
            r[0] = state
            reasons[eff] = detail
    partly = {}
    for m in re.finditer(r'^    also (\d+) draws of (\S+) in the debug colour: (.*)$', text, re.M):
        partly[m.group(2)] = partly.get(m.group(2), 0) + int(m.group(1))
        reasons[m.group(2)] = m.group(3)
    lines = [f'{len(ids)} arenas, {shots} shots; material classes seen: {len(rows)}']
    for eff, n in partly.items():
        if eff in rows:
            rows[eff][0] = rows[eff][0] + f' (+{n} debug draws)'
    for eff, (state, draws, arenas) in sorted(rows.items(), key=lambda kv: -kv[1][1]):
        extra = f'  ({reasons[eff]})' if eff in reasons else ''
        lines.append(f'  {eff:20} {state:20} {draws:8} draws in {arenas:3} arenas{extra}')
    bound = sum(1 for r in rows.values() if r[0] == 'bound to our effect')
    lines.append(f'classes bound to our effect: {bound} of {len(rows)}')
    result = '\n'.join(lines)
    print(result)
    with open(os.path.join(outdir, 'coverage_all.txt'), 'w') as f:
        f.write(result + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
