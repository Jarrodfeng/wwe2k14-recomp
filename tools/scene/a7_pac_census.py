#!/usr/bin/env python3
"""S103 A7 (step 3 of docs\\renderer\\S103_FABLE_REVIEW.md 1.5): which light-set ids does each arena pac carry in its nested PACH 30002 (entry id = set id)?
Reads the player's own game files, read-only; prints ids only (no game bytes). An arena lacking 10 / 20 / 30 / 31 / 32 installs NOTHING for that class (sub_827F6A40 returns -1,
sub_826203E8 skips a negative index), so the rule's caveat 2 is checked per arena.
  py -I tools\\scene\\a7_pac_census.py [--quiet]
Exit 0 always (a census, not a gate); the table is the finding."""
import glob
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MAIN = REPO if os.path.isdir(os.path.join(REPO, 'game_data')) else r'C:\Users\Jarrod\wwe2k14-recomp'  # worktrees have no game_data
sys.path.insert(0, os.path.join(MAIN, 'tools', 'wm30'))
import pac  # noqa: E402

NEED = (10, 20, 30, 31, 32)


def main():
    quiet = '--quiet' in sys.argv
    paths = sorted(glob.glob(os.path.join(MAIN, 'game_data', 'extracted', 'pac', 'bg', 'bg*.pac')))
    missing_any = 0
    no30002 = []
    rows = []
    for p in paths:
        n = os.path.basename(p)[2:4]
        ids = set()
        d = pac.load(p)
        for name, dec in pac.entries(d, 8):
            m = re.fullmatch(r'p\d+/30002/(\d+)', name)
            if m:
                ids.add(int(m.group(1)))
        if not ids:
            no30002.append(n)
            continue
        lack = [i for i in NEED if i not in ids]
        rows.append((n, sorted(ids), lack))
        missing_any += 1 if lack else 0
    print('arenas read %d, with a 30002 section %d, without %s' % (len(paths), len(rows), no30002))
    for n, ids, lack in rows:
        if lack or not quiet:
            print('bg%s: %d sets%s%s' % (n, len(ids), '  LACKS %s' % lack if lack else '', '' if quiet else '  ids %s' % ids))
    print('arenas lacking at least one of %s: %d of %d' % (list(NEED), missing_any, len(rows)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
