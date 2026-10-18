"""Session 30 (#6): live differential coverage: which guest functions ran in one coverage window but not in another.

  py tools/coverage/diffcov.py B [A] [--run NNN] [--top 40] [--all]

Windows are the files a coverage-build run writes at each `mark` (out/harness/coverage/<run>/NN_<text>.txt; src/
coverage.h): live, `py tools/live/live.py window LABEL` closes the window that ran since the last one under LABEL.
B and A name windows by their text (a substring, or the number NN). Without A: B against every other window of the run.
The result = functions in B and not in A, rarest first: "elsewhere" counts the other windows of all recorded runs
(out/harness/coverage/*/) that also ran it, so a button's handler (rare) comes before the menu code that runs on every
press. Names from the function database (db/*.json) when there is an entry. --run defaults to the newest run folder.
Typical: idle on a menu item (window "idle"), press A (window "press_a"), then `diffcov press_a idle`; trace the top
of the list with --wwe2k14_trace (CLAUDE.md: the runtime trace).
"""
import argparse
import glob
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
COV = os.path.join(REPO, 'out', 'harness', 'coverage')


def run_dirs():
    return sorted((d for d in os.listdir(COV) if d.isdigit() and os.path.isdir(os.path.join(COV, d))), key=int)


def windows(run):
    out = []
    for f in sorted(os.listdir(os.path.join(COV, run))):
        m = re.match(r'(\d+)_(.*)\.txt$', f)
        if m:
            out.append((int(m.group(1)), m.group(2), os.path.join(COV, run, f)))
    return out


def read(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return {ln.strip() for ln in f if ln.strip() and not ln.startswith('#')}


def pick(wins, key):
    if key.isdigit():
        hits = [w for w in wins if w[0] == int(key)]
    else:
        hits = [w for w in wins if w[1] == key] or [w for w in wins if key.lower() in w[1].lower()]
    if len(hits) != 1:
        raise SystemExit('window %r: %s (windows: %s)' % (key, 'not found' if not hits else 'ambiguous',
                                                           ', '.join('%02d_%s' % (w[0], w[1]) for w in wins)))
    return hits[0]


def db_names():
    names = {}
    for p in glob.glob(os.path.join(REPO, 'db', '*.json')):
        try:
            data = json.load(open(p, encoding='utf-8'))
        except (OSError, ValueError):
            continue
        entries = data if isinstance(data, list) else data.get('functions', data.get('entries', []))
        if isinstance(entries, dict):
            entries = list(entries.values())
        for e in entries if isinstance(entries, list) else []:
            if isinstance(e, dict) and e.get('address') and e.get('name'):
                names.setdefault(e['address'].lower(), (e['name'], e.get('confidence', '')))
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('b')
    ap.add_argument('a', nargs='?')
    ap.add_argument('--run')
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--all', action='store_true', help='print every function, not only --top')
    a = ap.parse_args()
    runs = run_dirs()
    if not runs:
        raise SystemExit('no coverage runs in %s' % COV)
    run = ('%03d' % int(a.run)) if a.run else runs[-1]
    wins = windows(run)
    wb = pick(wins, a.b)
    fb = read(wb[2])
    if a.a:
        wa = pick(wins, a.a)
        fa = read(wa[2])
        against = '%02d_%s' % (wa[0], wa[1])
    else:
        fa = set()
        for w in wins:
            if w != wb:
                fa |= read(w[2])
        against = 'every other window of run %s' % run
    only = fb - fa
    # How often each function ran in other windows of every recorded run (rarer = more specific to B).
    count = {f: 0 for f in only}
    total = 0
    for r in runs:
        for w in windows(r):
            if r == run and w == wb:
                continue
            total += 1
            for f in read(w[2]) & only:
                count[f] += 1
    names = db_names()
    ranked = sorted(only, key=lambda f: (count[f], f))
    print('run %s: window %02d_%s has %d functions, %d of them not in %s; "elsewhere" = of %d other windows '
          '(all runs)' % (run, wb[0], wb[1], len(fb), len(only), against, total))
    for f in ranked if a.all else ranked[:a.top]:
        n = names.get(f.lower())
        print('  %-12s elsewhere %4d   %s' % (f, count[f], ('%s (%s)' % n) if n else ''))
    if not a.all and len(ranked) > a.top:
        print('  ... %d more (--all)' % (len(ranked) - a.top))
    return 0


if __name__ == '__main__':
    sys.exit(main())
