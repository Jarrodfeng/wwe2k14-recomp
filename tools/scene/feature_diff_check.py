#!/usr/bin/env python3
r"""Gate check for tools\scene\feature_diff.py (Session 103, lane T2). 0 game runs; offline (frame_rig.exe + the case zip).

  py -I tools\scene\feature_diff_check.py --root . --case out\s103\run27_case.zip --frame 500 --refind unlit|old_alpha|today

  today      the tool on a real frame: both sides read (game draws > 0, our dumped draws > 0), the pass table has rows, and (reported, never asserted: the state moves as lanes land) the
             wrestler MISSING / VALUES lines of the day are printed.
  unlit      KNOWN BUG (a): the crowd drawn unlit (L4's plant WWE_RDR_PLANT_UNLIT_CROWD=1). The baseline must NOT carry "crowd: the game uses FX_LIT", the planted run MUST.
  old_alpha  KNOWN BUG (b): the barricade / pad's alpha handling before P1 (WWE_RDR_P1_PLANT=old_alpha). The planted run MUST add a game pixel variant (an alpha_gloss / alpha_spec one) that ours no
             longer draws, which the baseline does not have.
Exit 0 = PASS.
"""
import argparse, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import feature_diff as F  # noqa: E402


def texts(res):
    return [m['what'] for m in res['missing']] + [m['what'] for m in res['values']]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--case', default='out/s103/run27_case.zip')
    ap.add_argument('--frame', type=int, default=500)
    ap.add_argument('--refind', required=True, choices=['unlit', 'old_alpha', 'today'])
    a = ap.parse_args()
    case_path = a.case if os.path.isabs(a.case) or os.path.exists(a.case) else os.path.join(F.main_checkout(), a.case)
    if not os.path.exists(case_path):
        print('FAIL  no case at ' + case_path)
        return 1
    fx = F.Fx()
    case = F.Case(case_path)
    out = os.path.join(F.ROOT, 'out', 's103', 'T2', 'check_' + a.refind)
    base = F.run_frames(case, fx, [a.frame], out, {}, tag='base')[0]
    n, res, gs, os_, gdraws, o = base
    print(F.render(n, res, gs, os_))
    if not gdraws or not o['draws'] or not res['passes']:
        print('FAIL  a side read nothing (game draws %d, our draws %d, pass rows %d)' % (len(gdraws), len(o['draws']), len(res['passes'])))
        return 1
    if a.refind == 'today':
        print('PASS  both sides read: game %d draws, ours %d draws, %d pass rows, %d MISSING, %d VALUES' % (len(gdraws), len(o['draws']), len(res['passes']), len(res['missing']), len(res['values'])))
        return 0
    if a.refind == 'unlit':
        env, token = {'WWE_RDR_PLANT_UNLIT_CROWD': '1'}, 'crowd: the game uses FX_LIT'
    else:
        env, token = {'WWE_RDR_P1_PLANT': 'old_alpha'}, 'pixel variant yProp_Default_spec_alpha_gloss'
    planted = F.run_frames(case, fx, [a.frame], out, env, tag='plant')[0][1]
    in_base = any(token in t for t in texts(res))
    in_plant = any(token in t for t in texts(planted))
    print('%s  baseline carries "%s": %s (must be False)' % ('PASS' if not in_base else 'FAIL', token, in_base))
    print('%s  planted run (%s) carries it: %s (must be True)' % ('PASS' if in_plant else 'FAIL', ' '.join('%s=%s' % kv for kv in env.items()), in_plant))
    added = [t for t in texts(planted) if t not in set(texts(res))]
    print('lines the plant added (%d):' % len(added))
    for t in added[:10]:
        print('    ' + t)
    ok = (not in_base) and in_plant
    print('%s  known bug re-found: %s' % ('PASS' if ok else 'FAIL', a.refind))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
