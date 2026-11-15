"""Session 102 lane F2: parser for docs\\agents\\s102\\READY.md (the table the main session keeps).

  py -I tools\\harness\\ready_lanes.py --count-ready | --count-running | --check [--file PATH] [--json]

Table columns: Lane | State | Brief | Agent | Started | Notes. States: ready, running, waiting-input, done.
--check exits 1 (and says why) when the file is missing, has no table, has an unknown state or a duplicate lane: it never reads nothing quietly.
"""
import argparse, hashlib, json, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT = ROOT / 'docs' / 'agents' / 's102' / 'READY.md'
STATES = ('ready', 'running', 'waiting-input', 'done')
COLS = ('lane', 'state', 'brief', 'agent', 'started', 'notes')


def parse(path):
    """Return (rows, problems). Raises OSError when the file is missing."""
    text = Path(path).read_text(encoding='utf-8')
    rows, problems, seen_header = [], [], False
    for n, line in enumerate(text.splitlines(), 1):
        s = line.strip()
        if not s.startswith('|'):
            continue
        cells = [c.strip() for c in s.strip('|').split('|')]
        if not seen_header:
            if cells and cells[0].lower() == 'lane':
                seen_header = True
            continue
        if all(re.fullmatch(r':?-{2,}:?', c) for c in cells if c):
            continue
        cells += [''] * (len(COLS) - len(cells))
        row = dict(zip(COLS, cells[:len(COLS)]))
        row['line'] = n
        if row['state'] not in STATES:
            problems.append('line %d: lane %s has unknown state %r' % (n, row['lane'], row['state']))
        rows.append(row)
    if not seen_header:
        problems.append('no table with a "Lane" header row found')
    elif not rows:
        problems.append('table has no lane rows')
    names = [r['lane'] for r in rows]
    for d in sorted({x for x in names if names.count(x) > 1}):
        problems.append('duplicate lane %s' % d)
    return rows, problems


def counts(rows):
    return {s: sum(1 for r in rows if r['state'] == s) for s in STATES}


def digest(path):
    """Hash of the table rows (not the prose): changes when a lane's state/agent/notes change."""
    rows, _ = parse(path)
    return hashlib.sha1(json.dumps([[r[c] for c in COLS] for r in rows]).encode()).hexdigest()


def digest_states(rows):
    """Hash of (lane, state) only: Notes / Agent / Started edits do not change it (the stop hook's no-forever key)."""
    return hashlib.sha1(json.dumps(sorted([r['lane'], r['state']] for r in rows)).encode()).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--file', default=str(DEFAULT))
    ap.add_argument('--count-ready', action='store_true')
    ap.add_argument('--count-running', action='store_true')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()
    try:
        rows, problems = parse(a.file)
    except (OSError, ValueError) as e:
        print('FAIL: cannot read %s: %s' % (a.file, e))
        return 1
    c = counts(rows)
    if a.count_ready:
        print(c['ready'])
        return 1 if problems else 0
    if a.count_running:
        print(c['running'])
        return 1 if problems else 0
    if a.json:
        print(json.dumps({'counts': c, 'problems': problems}))
    else:
        print('lanes %d: %s' % (len(rows), ', '.join('%s %d' % (k, v) for k, v in c.items())))
    for p in problems:
        print('PROBLEM:', p)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
