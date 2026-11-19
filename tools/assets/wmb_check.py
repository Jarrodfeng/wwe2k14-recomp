"""S103 WMB: an arena texture name its own pac lacks borrows the byte-identical shared texture (asset_load.h kBorrowTextures).
Offline from the player's own files, 0 game runs:  py tools\\assets\\wmb_check.py [--game game_data\\extracted] [--exe out\\tools\\asset_dump.exe]
  asset_dump --all twice (borrow on / planted empty donor table = asset_dump --plant-borrow-empty):
  - each admitted name's "texture 'X' not found" warning is gone ONLY for the arenas that use it (EXPECT below), present in the planted run;
  - every other warning of every item is the same in both arms (only derived "material without ..." counts may change on those arenas);
  - names not admitted (stand_ground0, truss_01, fade_lightmap00, specular, ...) keep their warnings.
Exit 0 = pass, 1 = a check failed, 2 = cannot run."""
import collections
import json
import os
import re
import subprocess
import sys
import tempfile

# msg_score1 = None: the arenas that use it are read from the planted run (WMB.md named arena 18 only; asset_dump finds it missing in 18 arenas, and arena 18 is not an asset_dump item)
EXPECT = {'beam_moya01': {15, 56}, 'ji_chair': {16}, 'lightmap00': {28, 29, 38, 39}, 'msg_score1': None, 'slope_panel': {44}}
NOT_FOUND = re.compile(r"^texture '([^']+)' not found in any directory$")


def run(exe, game, plant, out):
    cmd = [os.path.abspath(exe), '--game', game, '--all', '--json', out]
    if plant:
        cmd.append('--plant-borrow-empty')
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    if p.returncode != 0:
        print('asset_dump failed (%d): %s' % (p.returncode, p.stderr[-300:]))
        sys.exit(2)
    return {it['label']: it for it in json.load(open(out))}


def main(argv):
    game = 'game_data/extracted'
    exe = 'out/tools/asset_dump.exe'
    if '--game' in argv:
        game = argv[argv.index('--game') + 1]
    if '--exe' in argv:
        exe = argv[argv.index('--exe') + 1]
    if not os.path.isdir(game):   # a worktree has no game_data: the main checkout (the parent of .claude\worktrees\<name>) holds the player's files
        alt = os.path.normpath(os.path.join(os.getcwd(), '..', '..', '..', game))
        game = alt if os.path.isdir(alt) else game
    if not os.path.isdir(game) or not os.path.isfile(exe):
        print('cannot run: game dir %s exists=%s, exe %s exists=%s (cwd %s)' % (game, os.path.isdir(game), exe, os.path.isfile(exe), os.getcwd()))
        return 2
    tmp = tempfile.mkdtemp(prefix='wmb_')
    on = run(exe, game, False, os.path.join(tmp, 'on.json'))
    off = run(exe, game, True, os.path.join(tmp, 'off.json'))
    bad = 0
    gone_seen = collections.defaultdict(set)
    for label, a in off.items():
        b = on[label]
        m = re.match(r'arena (\d+)$', label)
        arena = int(m.group(1)) if m else None
        wa, wb = collections.Counter(a['warnings']), collections.Counter(b['warnings'])
        for w in (wa - wb):
            nf = NOT_FOUND.match(w)
            if nf:
                name = nf.group(1).lower()
                if EXPECT.get(name, ()) is None or arena in EXPECT.get(name, ()):
                    gone_seen[name].add(arena)
                else:
                    print('FAIL %s: unexpected borrow of %s' % (label, name))
                    bad += 1
            elif not w.startswith('material without'):
                print('FAIL %s: other warning changed: %s' % (label, w))
                bad += 1
        for w in (wb - wa):
            if not w.startswith('material without'):
                print('FAIL %s: new warning with the borrow: %s' % (label, w))
                bad += 1
    for name, arenas in sorted(EXPECT.items()):
        got = gone_seen.get(name, set())
        if arenas is None:   # every arena of the planted run that lacks it must have it back-filled, and there must be some
            arenas = {int(l.split()[1]) for l, a in off.items() if l.startswith('arena ') and any(NOT_FOUND.match(w) and NOT_FOUND.match(w).group(1).lower() == name for w in a['warnings'])}
            if not arenas:
                print('FAIL %s: no arena uses it in the planted run' % name)
                bad += 1
        planted_back = all(any(NOT_FOUND.match(w) and NOT_FOUND.match(w).group(1).lower() == name for w in off['arena %d' % ar]['warnings']) for ar in arenas)
        ok = got == arenas and planted_back
        print('%s %-12s gone for arenas %s (expected %s), back under the planted empty table: %s' % ('ok  ' if ok else 'FAIL', name, sorted(got), sorted(arenas), planted_back))
        bad += 0 if ok else 1
    print('PASS' if not bad else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
