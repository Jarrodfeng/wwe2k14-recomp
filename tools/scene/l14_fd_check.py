#!/usr/bin/env python3
r"""Lane L14 (Session 103), question 2: feature_diff on run 28's case reports the plane shadow and rim maps as RAN (the rig is told body scale 1, so the wrestlers are drawn and the casters are resident).
0 game runs: frame_rig.exe on run28_case.zip.

  py -I tools\scene\l14_fd_check.py [--case Z] [--frames 500] [--plant no_body_scale]

Cause [C]: a case from a live run has no body-scale table, so frame_rig drew NO wrestler ("wrestler 80 / 0 NOT DRAWN"), every wrestler-fed map said "no caster is resident to measure" -- a tool fault (run 28's record
HAS the plane inputs: the plane map's own message was "no caster meshes drawn", not "inputs missing"). Run 27 said RAN only with fixtures. The shadow map (casters) stays MISSING in run 28: "no usable key in this frame's wrestler
light states" = the live record's light states / caster entries are open (bits 6 / 7), a real missing input, reported as such.
Plant: no_body_scale (feature_diff --no-body-scale-one) must FAIL.
"""
import argparse, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import feature_diff as F  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--case', default=None)
    ap.add_argument('--frames', default='500')
    ap.add_argument('--plant', default=None, choices=[None, 'no_body_scale'])
    a = ap.parse_args()
    main_co = F.main_checkout()
    case_zip = a.case or os.path.join(main_co, 'out', 's103', 'run28_case.zip')
    if not os.path.exists(case_zip):
        print('FAIL  L14 feature_diff: the case %s is not on disk' % case_zip)
        return 1
    F.BODY_SCALE_ONE = a.plant != 'no_body_scale'
    fx = F.Fx()
    case = F.Case(case_zip)
    out = os.path.join(F.ROOT, 'out', 'l14', 'fd_%s' % (a.plant or 'ok'))
    frames = [int(x) for x in a.frames.split(',')]
    results = F.run_frames(case, fx, frames, out, {})
    fails, lines = [], []
    for n, res, *_ in results:
        ran = {feat: (r, why) for feat, _n, r, why in res['passes']}
        wr = res['counts'].get('wrestler', (0, 0))
        lines.append('f%d wrestler draws game %d ours %d; plane %s, rim %s, shadow %s' % (
            n, wr[0], wr[1], 'RAN' if ran.get('plane shadow', (0,))[0] else 'MISSING', 'RAN' if ran.get('rim light map', (0,))[0] else 'MISSING', 'RAN' if ran.get('shadow map (casters)', (0,))[0] else 'MISSING (%s)' % ran.get('shadow map (casters)', (0, ''))[1][:60]))
        for feat in ('plane shadow', 'rim light map'):
            if not ran.get(feat, (False,))[0]:
                fails.append('frame %d: %s MISSING (%s)' % (n, feat, ran.get(feat, (0, '?'))[1][:100]))
        if wr[0] and not wr[1]:
            fails.append('frame %d: the wrestlers are not drawn by the rig' % n)
    msg = ' | '.join(lines)
    if fails:
        print('FAIL  L14 feature_diff: ' + '; '.join(fails) + ' | ' + msg)
        return 0 if a.plant else 1
    print('PASS  L14 feature_diff: ' + msg)
    return 1 if a.plant else 0


if __name__ == '__main__':
    sys.exit(main())
