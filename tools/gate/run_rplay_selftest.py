r"""Selftest for tools\gate\run_rplay.py (S103 GX): the picture path is per checkout. py -I tools\gate\run_rplay_selftest.py
Plant: the old behaviour (picture under the MAIN checkout whatever the checkout) gives two worktrees the same path; the test must call that a collision."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_rplay

main_co, wt_a, wt_b = Path('C:/m'), Path('C:/m/.claude/worktrees/a'), Path('C:/m/.claude/worktrees/b')
fails = []


def check(ok, text):
    print(('ok    ' if ok else 'FAIL  ') + text)
    if not ok:
        fails.append(text)


def old_path(here, main, name):
    return main / 'out' / 'gate' / f'{name}.png'


pa, pb = run_rplay.picture_path(wt_a, 'brock', {}), run_rplay.picture_path(wt_b, 'brock', {})
check(pa != pb, 'two worktrees get different picture paths for the same capture')
check(wt_a in pa.parents and wt_b in pb.parents, 'each picture is under its own checkout')
check(main_co / 'out' / 'gate' / 'brock.png' not in (pa, pb), 'no worktree writes the main checkout path')
check(run_rplay.picture_path(main_co, 'brock', {}) == main_co / 'out' / 'gate' / 'brock.png', 'the main checkout keeps its own out\\gate')
check(run_rplay.picture_path(wt_a, 'brock', {'RDR_GATE_PICTURE_DIR': 'C:/x'}) == Path('C:/x/brock.png'), 'RDR_GATE_PICTURE_DIR overrides')
check(run_rplay.picture_path(wt_a, 'brock', {'GATE_CHECK_DIR': 'C:/m/c1'}) == Path('C:/m/c1/brock.png') and run_rplay.picture_path(wt_a, 'brock', {'GATE_CHECK_DIR': 'C:/m/c2'}) != Path('C:/m/c1/brock.png'),
      'S103 PS2: GATE_CHECK_DIR (the check\'s own folder) holds the picture: two checks of one gate never share a path')
check(old_path(wt_a, main_co, 'brock') == old_path(wt_b, main_co, 'brock'), 'PLANT: the old shared path is a collision (this is what the test catches)')
print('PASS run_rplay selftest' if not fails else f'FAIL run_rplay selftest ({len(fails)})')
sys.exit(1 if fails else 0)
