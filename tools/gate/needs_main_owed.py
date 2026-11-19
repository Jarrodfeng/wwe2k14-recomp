#!/usr/bin/env python3
"""needs_main_owed: the check the main session runs BEFORE merging a builder branch (S103 WTG).

  py -I tools\\gate\\needs_main_owed.py <branch-or-commit>            list the owed rows; exit 3 while any is owed, 0 when none (or no record)
  py -I tools\\gate\\needs_main_owed.py <branch> --ack ROW[,ROW]      say the MAIN-ONLY rows were checked AFTER the merge on the merged tree in the main checkout under the run lock

A builder's gate in a worktree writes out\\gate\\needs_main_<sha12>.json (gate.py: the rows it could not run, exit code 4 = "green with needs-main"). gate_main.py writes
out\\gate\\needs_main_pass_<sha12>.json for the rows it ran and PASSED in gate-main. Owed = the newest record found in the branch's last 60 first-parent commits, minus the rows a
pass file at that commit or later passed, minus rows acked. MAIN-ONLY rows (they need the configured build folder) are never in a pass file: they stay owed until acked,
which is only honest after the merge. No record at all prints that plainly (nothing was owed OR the builder's gate did not run: ask for the gate line)."""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main_checkout():
    c = subprocess.run(['git', '-C', str(HERE), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=30).stdout.strip()
    return Path(c).parent


def walk(main, rev, n=60):
    p = subprocess.run(['git', '-C', str(main), 'rev-list', '--first-parent', '-n', str(n), rev], capture_output=True, text=True, timeout=60)
    if p.returncode:
        raise SystemExit('needs_main_owed: git rev-list %s failed: %s' % (rev, p.stderr.strip()[:200]))
    return p.stdout.split()


def read(p):
    try:
        return json.loads(Path(p).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None


def owed(out_dir, shas):
    """-> (record or None, owed rows, passed rows, acked rows). shas = newest first."""
    rec, at = None, None
    for i, s in enumerate(shas):
        rec = read(Path(out_dir) / ('needs_main_%s.json' % s[:12]))
        if rec:
            at = i
            break
    if rec is None:
        return None, [], [], []
    passed, acked = set(), set()
    for s in shas[:at + 1]:
        pf = read(Path(out_dir) / ('needs_main_pass_%s.json' % s[:12]))
        if pf:
            passed |= set(pf.get('rows', []))
        af = read(Path(out_dir) / ('needs_main_ack_%s.json' % s[:12]))
        if af:
            acked |= set(af.get('rows', []))
    return rec, sorted(set(rec.get('rows', [])) - passed - acked), sorted(passed), sorted(acked)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rev')
    ap.add_argument('--ack', default='', help='comma-separated MAIN-ONLY rows checked after the merge (writes needs_main_ack_<sha12>.json)')
    ap.add_argument('--out', default='', help='the gate results folder (default <main>/out/gate)')
    a = ap.parse_args()
    main = main_checkout()
    out = Path(a.out) if a.out else main / 'out' / 'gate'
    shas = walk(main, a.rev)
    if a.ack:
        rows = [x.strip() for x in a.ack.split(',') if x.strip()]
        (out / ('needs_main_ack_%s.json' % shas[0][:12])).write_text(json.dumps({'sha': shas[0], 'rows': rows, 'when': time.strftime('%Y-%m-%d %H:%M:%S')}), encoding='utf-8')
        print('needs_main_owed: acked %d row(s) at %s' % (len(rows), shas[0][:10]))
    rec, owe, passed, acked = owed(out, shas)
    if rec is None:
        print('needs_main_owed: no needs-main record in the last %d commits of %s: nothing is recorded as owed (if the builder gated in a worktree, ask for its gate line)' % (len(shas), a.rev))
        return 0
    print('needs_main_owed: record %s (builder gate at %s, %d row(s)); passed in gate-main %d, acked %d' % (rec.get('when', '?'), str(rec.get('sha', '?'))[:10], len(rec.get('rows', [])), len(passed), len(acked)))
    for r in owe:
        print('OWED  ' + r)
    if owe:
        print('needs_main_owed: %d row(s) OWED: run `py -I tools\\gate\\gate_main.py --rev %s --only %s`; MAIN-ONLY rows are checked after the merge, then --ack' % (len(owe), a.rev, ','.join(owe)))
        return 3
    print('needs_main_owed: nothing owed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
