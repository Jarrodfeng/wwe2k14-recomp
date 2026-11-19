#!/usr/bin/env python3
"""gate_main: the main session's command for a builder's `needs-main:` rows (S103 WTG).

  py -I tools\\gate\\gate_main.py --rev <commit> --only <rows> [--timeout S] [--jobs N] [--dry]
  <rows> = the comma-separated list the builder handed back (`needs-main: a.b, c.d`, the prefix is accepted), or lanes.

Pins the scratch worktree out\\wt\\gate-main to <commit> (a detached checkout: never gate in the moving main checkout, LESSONS 2026-11-18) and runs those rows there with
tools\\gate\\gate_wait.py ... --assume-main: in gate-main NEEDS-MAIN is impossible, a row that cannot find a file is a plain FAIL. NO junctions or links of any kind are made
(WTG review): a row that reads generated\\ or game_data\\ from its own tree must take MAIN's path by argument or from RDR_MAIN_CHECKOUT (gate.py sets it), the e2_check --game
pattern; one that does not is a real, visible failure to fix in the check.
The rows are read from the pinned commit's lane jsons (git show), so --dry changes nothing.
Rows that need the configured build folder (out\\build\\..., tools\\build.ps1) are MAIN-ONLY: they are NOT run here and NOT passed. They are checked AFTER the merge, on the
merged tree, in the main checkout under the run lock, and recorded as OWED until `needs_main_owed.py --ack` says they were.
Exit: 0 = every requested row RAN AND PASSED (nothing else is ever 0); 1 a row failed; 2 timed out; 3 something is still owed (a row did not run / was skipped / is MAIN-ONLY;
each is printed as "row still needs main: <what>"). A pass is recorded in out\\gate\\needs_main_pass_<sha12>.json for tools\\gate\\needs_main_owed.py."""
import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import needs_main as gnm   # noqa: E402


def main_checkout():
    c = subprocess.run(['git', '-C', str(HERE), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=30).stdout.strip()
    return Path(c).parent


def git(wt, *a, check=True):
    p = subprocess.run(['git', '-C', str(wt)] + list(a), capture_output=True, text=True, timeout=300)
    if check and p.returncode:
        raise SystemExit('gate_main: git %s failed: %s' % (' '.join(a), (p.stderr or p.stdout).strip()[:300]))
    return p.stdout.strip()


def rows_of(only):
    names = []
    for part in only:
        part = part.strip()
        if part.lower().startswith('needs-main:'):
            part = part.split(':', 1)[1]
        names += [x.strip() for x in part.split(',') if x.strip()]
    return names


def load_checks_at(main, sha):
    """Every check of the lane jsons AS COMMITTED at SHA (git show: nothing is checked out, so --dry changes nothing)."""
    names = git(main, 'ls-tree', '--name-only', sha, 'tools/gate/lanes/').splitlines()
    out = []
    for n in names:
        if not n.endswith('.json'):
            continue
        p = subprocess.run(['git', '-C', str(main), 'show', '%s:%s' % (sha, n)], capture_output=True, text=True, encoding='utf-8', timeout=60)
        try:
            data = json.loads(p.stdout)
        except ValueError:
            continue
        for c in data.get('checks', []):
            c['lane'] = Path(n).stem
            out.append(c)
    return out


def split_rows(checks, rows):
    """-> (selected checks, unknown names, main_only {name: why}). Names are matched as exact set members (never substrings)."""
    want = set(rows)
    sel = [c for c in checks if c['name'] in want or c['lane'] in want]
    found = {c['name'] for c in sel} | {c['lane'] for c in sel}
    unknown = sorted(want - found)
    main_only = {}
    for c in sel:
        for n in gnm.declared(c):
            if n.replace('\\', '/').startswith('out/build'):
                main_only[c['name']] = 'needs %s (the configured build folder)' % n
    return sel, unknown, main_only


def parse_gate_text(text):
    """-> (rows seen {name: verdict}, unrun {name: why}) from gate_wait's printed rows (every non-PASS row is printed in full)."""
    unrun, bad = {}, {}
    for ln in text.splitlines():
        m = re.match(r'^(SKIP|NEEDS-MAIN)\s+\S+\s+(\S+)\s+(.*)$', ln)
        if m:
            unrun[m.group(2)] = '%s: %s' % (m.group(1), m.group(3)[:160])
        m = re.match(r'^(FAIL|BUILD|NOT RUN)\s+\S+\s+(\S+)', ln)   # S103 GST review 10: a NOT RUN row (the --deadline) is not a pass either
        if m:
            bad[m.group(2)] = m.group(1)
    return unrun, bad


def final_exit(rc, ran_rows, expected_rows, unrun, main_only):
    """0 only when every requested row ran and passed. -> (exit code, [lines])."""
    lines = []
    for n, why in sorted(unrun.items()):
        lines.append('row still needs main: %s (%s)' % (n, why))
    ran = set(ran_rows)
    for n in sorted(set(expected_rows) - ran - set(unrun) - set(main_only)):
        lines.append('row still needs main: %s (it did not run: no result line)' % n)
    for n, why in sorted(main_only.items()):
        lines.append('row still needs main: %s (MAIN-ONLY, %s: NOT run here, NOT passed; checked AFTER the merge on the merged tree in the main checkout under the run lock, owed until acked)' % (n, why))
    if rc in (1, 2):
        return rc, lines
    if rc not in (0,):
        return 1, lines
    return (3 if lines else 0), lines


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--rev', required=True, help='the commit to pin gate-main to (a builder branch tip, or a sha)')
    ap.add_argument('--only', action='append', required=True, help='rows (lane.check), comma-separated; may repeat')
    ap.add_argument('--timeout', type=int, default=1800)
    ap.add_argument('--jobs', type=int, default=0)
    ap.add_argument('--dry', action='store_true', help='read the rows from the commit and say what would run; change nothing')
    a, extra = ap.parse_known_args()
    extra = [x for x in extra if x != '--']
    main = main_checkout()
    sha = git(main, 'rev-parse', '--verify', a.rev + '^{commit}')
    wt = main / 'out' / 'wt' / 'gate-main'
    rows = rows_of(a.only)
    if not rows:
        print('gate_main: no rows')
        return 3
    sel, unknown, main_only = split_rows(load_checks_at(main, sha), rows)
    if unknown:
        print('gate_main: not in the lanes of %s: %s' % (sha[:10], ', '.join(unknown)))
        return 3
    run_names = [c['name'] for c in sel if c['name'] not in main_only]
    print('gate_main: %d row(s) from %s: run %d in %s (--assume-main), MAIN-ONLY %d' % (len(sel), sha[:10], len(run_names), wt, len(main_only)))
    if a.dry:
        for n in run_names:
            print('  would run  ' + n)
        for n, why in sorted(main_only.items()):
            print('  MAIN-ONLY  %s (%s)' % (n, why))
        return 3 if main_only else 0
    if not (wt / '.git').exists():
        wt.parent.mkdir(parents=True, exist_ok=True)
        git(main, 'worktree', 'add', '--detach', str(wt), sha)
    else:
        if Path(git(wt, 'rev-parse', '--show-toplevel')).resolve() != wt.resolve():
            print('gate_main: %s is not its own worktree root: refusing' % wt)
            return 3
        git(wt, 'checkout', '--detach', '--force', sha)
    rc, text = 0, ''
    if run_names:
        cmd = [sys.executable, '-I', str(wt / 'tools' / 'gate' / 'gate_wait.py'), '--timeout', str(a.timeout), '--root', str(wt), '--only', ','.join(run_names), '--assume-main', '--base', 'HEAD']
        if a.jobs:
            cmd += ['--jobs', str(a.jobs)]
        cmd += extra
        p = subprocess.run(cmd, cwd=str(wt), capture_output=True, text=True, errors='replace')
        text = p.stdout + p.stderr
        print(text)
        rc = p.returncode
        if git(wt, 'rev-parse', 'HEAD') != sha:
            print('gate_main: FAIL gate-main HEAD moved during the run: the result is not on %s' % sha[:10])
            return 1
    unrun, bad = parse_gate_text(text)
    m = re.search(r'\[gate_wait\] (\d+) check rows', text)
    seen = int(m.group(1)) if m else 0
    ran_rows = run_names if (m and seen >= len(run_names)) else []
    code, lines = final_exit(rc, ran_rows, run_names, unrun, main_only)
    for ln in lines:
        print(ln)
    if code in (0, 3) and not bad and not unrun:
        out = main / 'out' / 'gate'
        out.mkdir(parents=True, exist_ok=True)
        (out / ('needs_main_pass_%s.json' % sha[:12])).write_text(json.dumps(
            {'sha': sha, 'when': time.strftime('%Y-%m-%d %H:%M:%S'), 'rows': run_names if rc == 0 else [], 'main_only': sorted(main_only)}, indent=1), encoding='utf-8')
    print('gate_main: exit %d (%s)' % (code, {0: 'every requested row ran and passed', 1: 'a row failed', 2: 'timed out', 3: 'rows are still owed'}.get(code, '?')))
    return code


if __name__ == '__main__':
    sys.exit(main())
