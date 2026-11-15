#!/usr/bin/env python3
# S102 lane M5: ab_line.py's two-arm output must be byte-identical to the base branch's (s102-arc-pt2) on the real two-arm folders.
#   py -I tools\surface\ab_two_arm_same.py [--base s102-arc-pt2]
# The base script is extracted from git into a temp file beside the real one (it finds bench_line.py next to itself), run on
# out\s102\ab8\g2banks\x (3 rounds, with and without --split) and out\s102\ab7\abt11\x (2 rounds), and removed. A planted twin edits the
# new reader's two-arm line format in a copy and must be caught. Exit 0 = identical, 1 = differs, 3 = input missing.
import argparse, os, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = Path(r'C:\Users\Jarrod\wwe2k14-recomp')
FOLDERS = [MAIN / 'out/s102/ab8/g2banks/x', MAIN / 'out/s102/ab7/abt11/x']


def run(script, folder, split):
    r = subprocess.run([sys.executable, '-I', str(script), str(folder)] + (['--split'] if split else []), capture_output=True, text=True)
    return r.returncode, r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', default='s102-arc-pt2')
    a = ap.parse_args()
    for f in FOLDERS:
        if not (f / 'ab_1_A').is_dir():
            print('SKIP: %s missing' % f)
            return 3
    g = subprocess.run(['git', 'show', '%s:tools/surface/ab_line.py' % a.base], capture_output=True, text=True, cwd=ROOT)
    if g.returncode or not g.stdout.strip():
        print('FAIL: cannot read the base ab_line.py from %s: %s' % (a.base, g.stderr.strip()[:200]))
        return 1
    here = ROOT / 'tools/surface'
    base = here / ('_ab_line_base_%d.py' % os.getpid())
    plant = here / ('_ab_line_plant_%d.py' % os.getpid())
    bad = 0
    try:
        base.write_text(g.stdout, encoding='utf-8')
        new_src = (here / 'ab_line.py').read_text(encoding='utf-8')
        needle = "print('B - A: shown %+5.1f"
        planted = new_src.replace(needle, "print('B - A:  shown %+5.1f", 1)
        if planted == new_src:
            print('FAIL: the plant DID NOT APPLY (the two-arm "B - A" line text moved)')
            return 1
        plant.write_text(planted, encoding='utf-8')
        n = 0
        for f in FOLDERS:
            for split in (False, True):
                rc0, o0 = run(base, f, split)
                rc1, o1 = run(here / 'ab_line.py', f, split)
                n += 1
                same = rc0 == rc1 == 0 and o0 == o1 and len(o0.splitlines()) > 8
                print('%s  %s %s: %d lines, base rc %d, new rc %d' % ('PASS' if same else 'FAIL', f.parent.name + '/' + f.name, '--split' if split else '', len(o0.splitlines()), rc0, rc1))
                bad += 0 if same else 1
        rc2, o2 = run(plant, FOLDERS[0], False)
        rc0, o0 = run(base, FOLDERS[0], False)
        caught = o2 != o0
        print('%s  planted twin (a changed "B - A" line) is caught by the same comparison' % ('PASS' if caught else 'FAIL'))
        bad += 0 if caught else 1
    finally:
        for p in (base, plant):
            try:
                p.unlink()
            except OSError:
                pass
    print('%d failed' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
