#!/usr/bin/env python3
r"""S103 L6 check: the scene front-end reads the wrestlers' shadow map (FX_SHADOW variants, slot 12) when the frame's maps hold one.

  py -I tools\scene\l6_check.py [--plant no_reader]

Runs the offline rig (0 game runs) on brock_losswin frame 110 (frame_rig.py; fview headless + frame_rig.exe) and reads its coverage lines: every yCh_skin / yCh_skin_flex / yCh_gear
pair must be a *_shadow_* fragment, and the shadow map must be MADE. --plant no_reader sets WWE_RDR_NO_SHADOW_READER=1 (the old behaviour: plain `base` variants): the check must FAIL
with the token `SHADOW READER`. Exit 0 = pass (or, with --plant, the token was found).
"""
import argparse, os, re, subprocess, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--plant', default=None, choices=[None, 'no_reader'])
    ap.add_argument('--capture', default=None)
    a = ap.parse_args()
    main_checkout = os.environ.get('RDR_MAIN_CHECKOUT') or ROOT
    cap = a.capture or os.path.join(main_checkout, 'out', 'captures', 'brock_losswin')
    out = os.path.join('out', 's103', 'L6', 'plant' if a.plant else 'check')
    env = dict(os.environ, CLAUDECODE='1', WWE_FVIEW_ALLOW_STALE='1')
    if a.plant:
        env['WWE_RDR_NO_SHADOW_READER'] = '1'
    r = subprocess.run([sys.executable, '-I', os.path.join('tools', 'scene', 'frame_rig.py'), '--maps-check', '--capture', cap, '--frames', '110', '--out', out], cwd=ROOT, env=env,
                       capture_output=True, text=True)
    rep = os.path.join(ROOT, out, 'frame_110_report.txt')
    if not os.path.exists(rep):
        print('FAIL  the rig wrote no report: ' + (r.stdout + r.stderr)[-300:])
        return 1
    text = open(rep, errors='replace').read()
    fails = []
    pairs = [l for l in text.splitlines() if re.match(r'\s*yCh_(skin|skin_flex|gear)\s+bound to our effect', l)]
    if not pairs:
        fails.append('no yCh_skin / gear coverage line in the report')
    for l in pairs:
        if '_shadow' not in l:
            fails.append('plain variant bound: ' + l.strip()[:140])
    if 'MAP SHADOW MADE' not in text:
        fails.append('the shadow map is not made')
    if fails:
        print('FAIL  SHADOW READER: ' + '; '.join(fails))
        return 0 if a.plant else 1
    print('PASS  SHADOW READER: %d character pairs bound to the shadow variants, shadow map made' % len(pairs))
    return 1 if a.plant else 0


if __name__ == '__main__':
    sys.exit(main())
