#!/usr/bin/env python3
r"""Gate check, Session 103 lane P2 (landing 1: our record-drawn frame's post). 0 game runs; offline (frame_rig.exe + the case zip).

  py -I tools\scene\post_p2_check.py --root . [--case out\s103\run27_case.zip] [--frame 500]

  1 PASSES   feature_diff on the frame: "depth of field" and "grade / final combine" RAN in ours (the rig now draws the post the live mirror draws, from the record's post row).
  2 TWIN     WWE_RDR_PLANT_POST=dof_off: the tool must report the DoF pass MISSING (the planted twin is caught).
  3 PICTURE  far-crowd sharpness (mean |dx| of rows 28..70 % of the picture height) post / no-post: at 1280x720 and at 2560x1440 the ratio must be below 0.6 (DoF visibly blurs) and the two
             ratios within 0.08 of each other (the blur is the same fraction of the picture at every render scale).
  4 REFIND   KNOWN BUG (run 27 sheets, ours sharp at scale 2): WWE_RDR_PLANT_POST_DRAWN_TAPS=1 (tap tables from the drawn size, the old behaviour) must break check 3's equality.
Exit 0 = PASS.
"""
import argparse, os, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import feature_diff as F  # noqa: E402


def grad(png):
    x = np.asarray(Image.open(png).convert('L'), dtype=np.float32)
    h = x.shape[0]
    return float(np.abs(np.diff(x[int(h * .28):int(h * .70)], axis=1)).mean())


def run(case, fx, frame, tag, size, env):
    os.environ['FEATURE_DIFF_SIZE'] = size
    out = os.path.join(F.ROOT, 'out', 's103', 'P2', 'check_' + tag)
    r = F.run_frames(case, fx, [frame], out, env, tag=tag)[0]
    return r, os.path.join(out, '%s_%d_whole.png' % (tag, frame))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--case', default='out/s103/run27_case.zip')
    ap.add_argument('--frame', type=int, default=500)
    a = ap.parse_args()
    case_path = a.case if os.path.isabs(a.case) or os.path.exists(a.case) else os.path.join(F.main_checkout(), a.case)
    if not os.path.exists(case_path):
        print('FAIL  no case at ' + case_path)
        return 1
    fx, case = F.Fx(), F.Case(case_path)
    bad = 0

    def say(ok, msg):
        nonlocal bad
        print(('PASS  ' if ok else 'FAIL  ') + msg)
        bad += 0 if ok else 1

    r, p_small = run(case, fx, a.frame, 'post720', '1280x720', {})
    miss = [m['what'] for m in r[1]['missing']]
    say(not any('"depth of field"' in m or '"grade / final combine"' in m for m in miss), 'DoF and grade / final combine ran in ours (missing: %s)' % [m for m in miss if m.startswith('pass "depth') or 'grade' in m])
    t, _ = run(case, fx, a.frame, 'dofoff', '1280x720', {'WWE_RDR_PLANT_POST': 'dof_off'})
    say(any('"depth of field"' in m['what'] for m in t[1]['missing']), 'planted twin dof_off: the DoF pass is reported MISSING')
    _, n_small = run(case, fx, a.frame, 'nop720', '1280x720', {'WWE_RDR_NO_SCENE_POST': '1'})
    _, p_big = run(case, fx, a.frame, 'post1440', '2560x1440', {})
    _, n_big = run(case, fx, a.frame, 'nop1440', '2560x1440', {'WWE_RDR_NO_SCENE_POST': '1'})
    rs, rb = grad(p_small) / grad(n_small), grad(p_big) / grad(n_big)
    print('  crowd sharpness post / no-post: 1280x720 %.3f, 2560x1440 %.3f' % (rs, rb))
    say(rs < 0.6 and rb < 0.6, 'DoF blurs the far crowd at both sizes (ratio < 0.6)')
    say(abs(rs - rb) < 0.08, 'the blur is the same fraction of the picture at both sizes (|diff| %.3f < 0.08)' % abs(rs - rb))
    _, p_old = run(case, fx, a.frame, 'old1440', '2560x1440', {'WWE_RDR_PLANT_POST_DRAWN_TAPS': '1'})
    ro = grad(p_old) / grad(n_big)
    print('  planted old taps at 2560x1440: %.3f' % ro)
    say(abs(rs - ro) >= 0.04 and ro > rb, 'known bug re-found: taps from the drawn size leave the crowd sharper at scale 2 (%.3f vs %.3f)' % (ro, rb))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
