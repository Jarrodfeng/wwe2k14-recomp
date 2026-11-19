#!/usr/bin/env python3
"""Replay recorded gate runs against the result cache and say how many checks WOULD have hit (S103 H3 proof, audit 1.3: "replay tonight's L1C and B2F gate
sequences from times.csv against the cache and print how many of their checks would have hit").

  py -I tools\\gate\\cache_replay.py --lane s103_B2F --branch worktree-agent-abcb964cb23a524fb [--times PATH] [--root-contains TEXT]

times.csv has no input hashes (the cache column was empty in every row: the cache was off for --only), so the key is REBUILT from git: for each recorded run of the lane,
the newest commit of --branch at or before the run's time gives the content (git blob ids) of every file a check's `affects` globs match, plus the lane json's blob;
that, with the check name, is the proxy for fastgate.cache_key. A check is CACHEABLE when it declares `affects`, has no "cache": false and runs no .ps1 (builds are never
cached by the gate: they have their own tool caches). A cacheable check that PASSED in an earlier recorded run with the same proxy key is a HIT; a FAIL / SKIP / BUILD is
never a pass to hit on. LIMITS, printed with the result: uncommitted edits between commits are invisible (a builder edits for minutes between its commits), so a "hit" is
an estimate that assumes no uncommitted edit changed an affects file; the lane json is read at the branch TIP (affects a lane declared later count for earlier runs)."""
import argparse
import csv
import hashlib
import json
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import fastgate as fg   # noqa: E402


def git(*a):
    p = subprocess.run(['git', '-C', str(ROOT)] + list(a), capture_output=True, text=True, errors='replace', timeout=120)
    return p.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--lane', required=True)
    ap.add_argument('--branch', required=True)
    ap.add_argument('--times', default=str(ROOT.parents[2] / 'out' / 'gate' / 'times.csv') if ROOT.parent.name == 'worktrees' else str(ROOT / 'out' / 'gate' / 'times.csv'))
    ap.add_argument('--root-contains', default='')
    a = ap.parse_args()
    lane_json = json.loads(git('show', f'{a.branch}:tools/gate/lanes/{a.lane}.json'))
    checks = {c['name']: c for c in lane_json['checks']}
    lane_aff = lane_json.get('affects')
    commits = sorted((int(ct), h) for ct, h in (ln.split() for ln in git('log', '--format=%ct %H', a.branch).splitlines() if ln.strip()))
    runs = defaultdict(list)
    with open(a.times, newline='', encoding='utf-8', errors='replace') as f:
        for r in csv.DictReader(f):
            if r['lane'] == a.lane and a.root_contains in r['root'] and r['mode'] == 'full':
                runs[(r['when'], r['root'], r.get('run') or '')].append(r)   # S103 GST review 11: the gate's RUN_ID (two gates in one second / one tree no longer merge)
    if not runs:
        print(f'no recorded runs of {a.lane} in {a.times}')
        return 1
    tree_cache = {}

    def key_for(commit, name):
        c = checks.get(name)
        aff = (c or {}).get('affects', lane_aff)
        if not c or aff is None or c.get('cache') is False or any(str(x).lower().endswith('.ps1') for x in c.get('cmd', [])):
            return None
        if commit not in tree_cache:
            tree_cache[commit] = [ln.split(None, 3) for ln in git('ls-tree', '-r', commit).splitlines()]
        aff = [aff] if isinstance(aff, str) else aff
        d = hashlib.sha256((name + json.dumps(c.get('cmd'))).encode())
        for _mode, _t, blob, rest in tree_cache[commit]:
            path = rest.split('\t', 1)[-1] if '\t' in rest else rest
            if fg.matches(path, aff):
                d.update(path.encode() + blob.encode())
        return d.hexdigest()
    seen_pass = set()
    tot = hits = unc = 0
    sec_all = sec_hit = 0.0
    print(f'{a.lane} on {a.branch}: {len(runs)} recorded gate run(s)')
    for (when, root, _run), rows in sorted(runs.items()):
        ts = time.mktime(time.strptime(when, '%Y-%m-%d %H:%M:%S'))
        before = [h for ct, h in commits if ct <= ts]
        commit = before[-1] if before else commits[0][1]
        line = []
        for r in rows:
            tot += 1
            sec = float(r['seconds'])
            sec_all += sec
            k = key_for(commit, r['check'])
            if k is None:
                unc += 1
                line.append(f"{r['check'].split('.', 1)[-1]}:{r['verdict']}(uncacheable)")
            elif k in seen_pass:
                hits += 1
                sec_hit += sec
                line.append(f"{r['check'].split('.', 1)[-1]}:HIT({sec:.0f}s)")
            else:
                line.append(f"{r['check'].split('.', 1)[-1]}:{r['verdict']}")
            if k is not None and r['verdict'] == 'PASS':
                seen_pass.add(k)
        print(f'  {when} @ {commit[:9]}: ' + '  '.join(line))
    print(f'RESULT {a.lane}: {hits} of {tot} checks would have hit the cache ({sec_hit:.0f} s of {sec_all:.0f} s check time); {unc} are uncacheable (build .ps1 steps or no affects: always run).')
    print('LIMITS: keyed on committed state at each run (uncommitted edits between commits invisible: an estimate); the lane json is read at the branch tip.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
