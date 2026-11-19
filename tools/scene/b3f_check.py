#!/usr/bin/env python3
r"""Lane B3F (S103): hair highlight + per-material rim constants. 0 game runs, 0 Surface runs; offline (the rig + feature_diff on a case).

  py -I tools\scene\b3f_check.py [CASE.zip] [--plant]

Answers: do ours' wrestler draws still lack the game's c249 HairSpeParam / c250 SpeCol1 / c251 SpeCol2 / c174 RimLightParam3 values?
Normal run: feature_diff (run 29 frames 450,600) must list NO wrestler CONST line for HairSpeParam / SpeCol1 / SpeCol2 / RimLightParam3 (before the lane: 8 lines).
--plant: WWE_RDR_B3F_OLD_HAIR_RIM=1 restores the zeros / hard-coded rim; feature_diff must find the gap again (the 4 names listed) or the check FAILs (exit 1).
"""
import os, re, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
NAMES = ('HairSpeParam', 'SpeCol1', 'SpeCol2', 'RimLightParam3')


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    plant = '--plant' in sys.argv
    sys.path.insert(0, HERE)
    import feature_diff
    case = args[0] if args else os.path.join(feature_diff.main_checkout(), 'out', 's103', 'run29_case.zip')
    if not os.path.exists(case):
        print('FAIL: case missing: %s' % case)
        return 1
    cmd = [sys.executable, '-I', os.path.join(HERE, 'feature_diff.py'), case, '--frames', '450,600', '--max-values', '200']
    if plant:
        cmd += ['--env', 'WWE_RDR_B3F_OLD_HAIR_RIM=1']
    p = subprocess.run(cmd, capture_output=True, text=True)
    out = p.stdout + p.stderr
    if p.returncode != 0 or 'frames 450,600' not in out:   # a tool that printed nothing must not pass
        print('FAIL: feature_diff did not report (rc %d):\n%s' % (p.returncode, out[-1500:]))
        return 1
    hits = [l.strip() for l in out.splitlines() if re.search(r'CONST wrestler (%s)\b' % '|'.join(NAMES), l)]
    found = {re.search(r'CONST wrestler (\w+)', l).group(1) for l in hits}
    if plant:
        if found != set(NAMES):
            print('FAIL (plant): feature_diff did not find the gap again; found %s' % sorted(found))
            return 1
        print('PASS (plant): the restored zeros are found again: %d lines, %s' % (len(hits), sorted(found)))
        return 0
    if hits:
        print('FAIL: %d wrestler CONST lines remain:\n  ' % len(hits) + '\n  '.join(hits))
        return 1
    print('PASS: no wrestler CONST line for %s' % ', '.join(NAMES))
    return 0


if __name__ == '__main__':
    sys.exit(main())
