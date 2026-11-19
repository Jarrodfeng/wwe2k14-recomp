#!/usr/bin/env python3
r"""S103 lane E8 check: the wrestlers' caster shadow map has a key light, and the char draws are lit by the light state the game's draws read. 0 game runs, 0 Surface runs.

  py -I tools\scene\e8_check.py [--case CASE.zip] [--frame 500] [--plant light_state|old_key_rule] [--out DIR]

Runs tools\scene\feature_diff.py (game side from the case's stream, ours from the case's scene record through the offline rig) on one mirror frame of run 28 and checks:
  SHADOW  the pass "shadow map (casters)" RAN in ours (before E8: MISSING "no usable key in this frame's wrestler light states"; the record's wrestler state stores w = 0 on its lights).
  LIGHTS  no CONST line of the wrestler class for KeyLightVec / KeyLightColor / DifLghVec / DifLghCol / SkyCol / GroundCol: the char draws are fed the game's own constants.
Plants (each must FAIL with its token; no_state = WWE_RDR_E8_PLANT_NO_STATE: HOLES, zero_state = WWE_RDR_E8_PLANT_ZERO_STATE: a zero state is refused and the arena set stays: LIGHTS):
  light_state (WWE_RDR_NO_WRESTLER_LIGHT_STATE=1, the arena's set feeds the lights: LIGHTS) and old_key_rule (WWE_RDR_E8_OLD_KEY_RULE=1, `w == 1` required again: SHADOW).
Exit 0 = pass (with a plant: the failure token was found).
"""
import argparse, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
MAIN = os.environ.get('RDR_MAIN_CHECKOUT') or r'C:\Users\Jarrod\wwe2k14-recomp'
EXPECT_HOLES = 7   # run 28 f500, no plant [C: the wrestlers family line]. Was 11 before S103 NR fix A: the 4 apron skirts (3 drawn by the ring family, 1 not drawn by the game this frame) are no longer holes; the 7 own-skeleton pieces are
LIGHT_FIELDS = ('KeyLightVec', 'KeyLightColor', 'DifLghVec', 'DifLghCol', 'SkyCol', 'GroundCol')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--case', default=os.path.join(MAIN, 'out', 's103', 'run28_case.zip'))
    ap.add_argument('--frame', type=int, default=500)
    ap.add_argument('--plant', choices=[None, 'light_state', 'old_key_rule', 'no_state', 'zero_state'], default=None)
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 's103', 'E8', 'check'))
    a = ap.parse_args()
    cmd = [sys.executable, '-I', os.path.join(HERE, 'feature_diff.py'), a.case, '--frames', str(a.frame), '--max-values', '200', '--out', a.out]
    if a.plant == 'light_state':
        cmd += ['--env', 'WWE_RDR_NO_WRESTLER_LIGHT_STATE=1']
    elif a.plant == 'old_key_rule':
        cmd += ['--env', 'WWE_RDR_E8_OLD_KEY_RULE=1']
    elif a.plant == 'no_state':
        cmd += ['--env', 'WWE_RDR_E8_PLANT_NO_STATE=1']
    elif a.plant == 'zero_state':
        cmd += ['--env', 'WWE_RDR_E8_PLANT_ZERO_STATE=1']
    env = dict(os.environ, RDR_MAIN_CHECKOUT=MAIN)
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', env=env, timeout=1500)
    text = (p.stdout or '') + (p.stderr or '')
    fails = []
    m = re.search(r'shadow map \(casters\)\s+game\s+\d+ draws\s+ours (\w+)(.*)', text)
    if not m:
        fails.append('SHADOW the report has no shadow-map line (feature_diff failed: %s)' % text[-300:].replace('\n', ' '))
    elif m.group(1) != 'RAN':
        fails.append('SHADOW the casters\' shadow map did not run in ours: %s' % m.group(2).strip())
    bad = [ln.strip() for ln in text.splitlines() if re.match(r'\s*CONST wrestler (%s)' % '|'.join(LIGHT_FIELDS), ln)]
    for ln in bad[:4]:
        fails.append('LIGHTS ' + ln)
    # HOLES: a visible wrestler with no state while the record has states is a hole (an OURS frame falls back whole, never lit by the arena's set). Run 28 f500 [C]: 11 wrestler-family holes without the plant (the
    # skeleton pieces / invisible skeletons, all other causes); every wrestler whose state is missing adds one.
    rep = os.path.join(a.out, 'ours_%d_report.txt' % a.frame)
    holes = None
    if os.path.exists(rep):
        mh = re.search(r'family wrestlers objects \d+ drawn \d+ skipped \d+ holes (\d+)', open(rep, encoding='utf-8', errors='replace').read())
        holes = int(mh.group(1)) if mh else None
    if holes is None:
        fails.append('HOLES the report has no wrestler-family line')
    elif holes != EXPECT_HOLES:
        fails.append('HOLES wrestler-family holes %d, expected %d (a wrestler without a light state must be a hole; a state must not add one)' % (holes, EXPECT_HOLES))
    print('frame %d: shadow map %s; wrestler light constants differing from the game\'s: %d' % (a.frame, m.group(1) if m else '?', len(bad)))
    if fails:
        print('FAIL  ' + ' | '.join(fails))
        tok = {'light_state': 'LIGHTS', 'old_key_rule': 'SHADOW', 'no_state': 'HOLES', 'zero_state': 'LIGHTS'}.get(a.plant)
        return 0 if (a.plant and tok and tok in ' | '.join(fails)) else 1
    print('PASS  SHADOW + LIGHTS: the caster shadow map ran and the wrestlers are lit with the game\'s constants')
    return 1 if a.plant else 0


if __name__ == '__main__':
    sys.exit(main())
