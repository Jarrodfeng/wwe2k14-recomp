#!/usr/bin/env python3
r"""WGS review round 5: the arena sheet's list of OPEN findings (tools\scene\arena_sheet_known.txt) may not GROW against main without a reason: a longer list than
`git show main:tools/scene/arena_sheet_known.txt` needs a `# grows: <reason>` line that main's copy does not have (written by arena_sheet.py --write-known --known-grows REASON).
Shrinking is always fine. A main without the file counts as an empty list.
  py -I tools\scene\known_growth_lint.py [--base main] [--file tools/scene/arena_sheet_known.txt]
Exit 0 = fine; 1 = grown without a reason.
"""
# entry: growth_problem
import argparse, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))


def rows(text):
    return [ln for ln in text.splitlines() if ln.split('#')[0].strip()]


def grows_lines(text):
    return {ln.strip() for ln in text.splitlines() if ln.strip().startswith('# grows:')}


def growth_problem(old_text, new_text):
    """None when fine; else the reason the lint fails"""
    n_old, n_new = len(rows(old_text)), len(rows(new_text))
    if n_new <= n_old:
        return None
    if grows_lines(new_text) - grows_lines(old_text):
        return None
    return 'the known list grows from %d to %d rows with no new "# grows: <reason>" line' % (n_old, n_new)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--base', default='main')
    ap.add_argument('--file', default='tools/scene/arena_sheet_known.txt')
    a = ap.parse_args(argv)
    p = subprocess.run(['git', '-C', ROOT, 'show', '%s:%s' % (a.base, a.file)], capture_output=True, text=True, encoding='utf-8', errors='replace')
    old = p.stdout if p.returncode == 0 else ''
    try:
        with open(os.path.join(ROOT, a.file), encoding='utf-8') as f:
            new = f.read()
    except OSError:
        new = ''
    why = growth_problem(old, new)
    print('known_growth_lint: %s (%s: %d rows on %s, %d here)' % ('FAIL ' + why if why else 'PASS', a.file, len(rows(old)), a.base, len(rows(new))))
    return 1 if why else 0


if __name__ == '__main__':
    sys.exit(main())
