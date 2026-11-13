r"""S101 lane 128 gate check: the arena D3 readiness tool (tools\skip\arena_ready.py) on run 7079's case folder, plus its planted twins. Offline, 0 game runs.
Input: lane 92's unpacked case of run 7079 (<main checkout>\out\s101\lane92\case_20261112_122658_040_0_pc_run_exit, game data, local only). A missing input FAILS loudly.

  py -I tools\skip\arena_ready_selftest.py [--frames 0:380:19]

Positive (the shipped tool, run as a process): exit 0 with `--expect placed --expect mirror --expect families`: the placed family has ~16 draws a frame (ARENA_OURS.md),
the mirrored copies are sourced by the mirror rule with no unsourced camera register, the family draws add up to the arena-wide draws; the light_state column is
reported dormant (this capture carries no light tables); the screens family is 100 % sourced; the projected-with-light-states share is >= the sourced share.
Planted twins (each must exit 1 and say why):
  placed_off       the placed slots not recognised (the ledger's own rule files them under ring): `--expect placed` must FAIL (re-finds the "arena 0 % because the
                   placed slots are counted under ring" misreading of ledger section 7.5)
  no_mirror_rule   the mirrored cameras get no mirror rule: `--expect mirror` must FAIL
  missing_input    a path that does not exist must exit 1, never an empty report"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
TOOL = os.path.join(HERE, 'arena_ready.py')
CASE = 'out/s101/lane92/case_20261112_122658_040_0_pc_run_exit'


def main_checkout():
    try:
        common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return ROOT


def tool(args):
    p = subprocess.run([sys.executable, '-I', TOOL] + args, capture_output=True, text=True, timeout=900)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def main():
    frames = '0:380:19'
    if '--frames' in sys.argv:
        frames = sys.argv[sys.argv.index('--frames') + 1]
    case = os.path.join(main_checkout(), CASE)
    bad = 0

    def check(name, ok, why):
        nonlocal bad
        print('%s  %-18s %s' % ('PASS' if ok else 'FAIL', name, why))
        bad += 0 if ok else 1

    if not os.path.isdir(os.path.join(case, 'capture', 'frames')):
        print('FAIL  input missing: %s' % case)
        return 1
    ex = ['--expect', 'placed', '--expect', 'mirror', '--expect', 'families']
    rc, out = tool([case, '--frames', frames] + ex)
    check('positive', rc == 0 and 'arena_ready: PASS' in out, 'exit %d' % rc)
    check('dormant_light', 'light-state records 0' in out, 'the capture has no light tables, so the column is reported dormant')
    screens = [l for l in out.splitlines() if l.strip().startswith('screens')]
    check('screens', bool(screens) and '100-sourced 100.00%' in screens[0].replace('%-', '-').replace('100%', '100'), screens[0].strip()[:90] if screens else 'no screens line')
    for name, expect in (('placed_off', 'placed'), ('no_mirror_rule', 'mirror')):
        rc, out = tool([case, '--frames', frames, '--plant', name, '--expect', expect])
        check('plant ' + name, rc == 1 and 'FAIL  expect' in out, 'exit %d (must be 1 with a FAIL expect line)' % rc)
    rc, out = tool([case + '_does_not_exist'])
    check('plant missing_input', rc == 1 and 'FAIL' in out, 'exit %d' % rc)
    print('arena_ready selftest: %s' % ('FAIL (%d)' % bad if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
