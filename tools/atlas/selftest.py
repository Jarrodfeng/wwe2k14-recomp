"""S101 lane 4: the atlas self test (the gate entry, tools/gate/lanes/atlas.json).

    py -I tools/atlas/selftest.py

Builds out/tools/atlas.exe when it is missing or older than its sources, then:
  1. atlas.exe --check --expect tools/atlas/expect.json  -> must exit 0 (three pacs: a wrestler, an arena, menuHD; counts match)
  2. the same with --fault (every expected count +1)       -> must exit 1 (the planted-fault twin: a "0 problems" check that cannot fail is no check)
  3. --check with a missing expect file                    -> must exit 2 (a check that cannot read its input fails loudly)
No game run: the exe only reads files under game_data/extracted of the main checkout. Counts only; no game strings are printed or stored.
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# game_data lives in the main checkout (gitignored): the gate's env var, this tree, or the checkout above a .claude/worktrees/<name> worktree
_cands = [Path(os.environ[k]) for k in ('RDR_MAIN_CHECKOUT',) if k in os.environ] + [ROOT, ROOT.parents[2]]
GAME = next((c / 'game_data' / 'extracted' for c in _cands if (c / 'game_data' / 'extracted' / 'pac').is_dir()), _cands[0] / 'game_data' / 'extracted')
EXE = ROOT / 'out' / 'tools' / 'atlas.exe'
EXPECT = ROOT / 'tools' / 'atlas' / 'expect.json'
SOURCES = [ROOT / 'tools' / 'atlas' / 'atlas.cpp'] + [ROOT / 'src' / 'assets' / n for n in ('jboy.h', 'dds.h', 'pac.h', 'asset_model.h')]


def run(args, timeout):
    p = subprocess.run([str(EXE), '--game', str(GAME)] + args, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout + p.stderr).strip()


def main():
    if not GAME.is_dir():
        print(f'FAIL: game files not found at {GAME}')
        return 1
    if not EXPECT.is_file():
        print(f'FAIL: {EXPECT} missing')
        return 1
    stale = not EXE.is_file() or any(s.stat().st_mtime > EXE.stat().st_mtime for s in SOURCES if s.is_file())
    if stale:
        b = subprocess.run(['pwsh', '-NoProfile', '-File', str(ROOT / 'tools' / 'atlas' / 'build.ps1')], capture_output=True, text=True, timeout=240)
        if b.returncode != 0 or not EXE.is_file():
            print('FAIL: build failed: ' + (b.stdout + b.stderr).strip().splitlines()[-1][:200])
            return 1
    rc, out = run(['--check', '--expect', str(EXPECT)], 110)
    if rc != 0:
        print('FAIL: check: ' + out.replace('\n', ' | ')[-300:])
        return 1
    ok_line = out.splitlines()[-1]
    rc2, out2 = run(['--check', '--fault', '--expect', str(EXPECT)], 110)
    if rc2 != 1:
        print(f'FAIL: planted fault was not caught (exit {rc2}): ' + out2.replace("\n", " | ")[-200:])
        return 1
    rc3, out3 = run(['--check', '--expect', str(ROOT / 'tools' / 'atlas' / 'no_such_expect.json')], 110)
    if rc3 != 2:
        print(f'FAIL: an unreadable expect file did not fail loudly (exit {rc3})')
        return 1
    print(f'PASS atlas selftest: {ok_line}; planted fault caught; unreadable input fails loudly')
    return 0


if __name__ == '__main__':
    sys.exit(main())
