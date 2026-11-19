#!/usr/bin/env python3
r"""S103 L7 check: what our wrestler draws are FED, against the game's wrestler draws of the same stream frame (0 game runs: fview-less; the offline rig + capread on the capture's stream).

  py -I tools\scene\l7_const.py [--frame 200] [--plant black_rim|key_unrotated|raw_bound]

Offline rig on brock_losswin (frame_rig.py --maps-check, WWE_RDR_RIG_BODY_SCALE_ONE=1: the rig has no body-scale latch, so the lighting proof draws an unknown scale as 1; the silhouettes are
approximate, the CONSTANTS and the BINDINGS are exact). WWE_RDR_FEATURE_DUMP writes one JSON line per draw our front-end submitted. Verdicts (tokens):
  RIM SLOT    every yCh_skin / yCh_gear shadow draw of ours binds slot 15 to a map (not the black stand-in)
  KEY LIGHT   ours DifLghVec[4] and KeyLightVec equal the game's wrestler draw's (within 0.03 per component) and DifLghCol[4] within 0.05
  RIM PARAMS  RimLightDepth.z == (far - near) / 1000 (the game's own rule, checked on its draw) and our near / far are within 25 % of the game's width of the stream's
  RAW         the census says both maps went through their chains (rim filtered > 0, shadow filtered > 0)
Plants: black_rim (WWE_RDR_NO_MAP_CHAIN=1: the rim map stays raw, slot 15 stays black) must fail RIM SLOT; key_unrotated (WWE_RDR_NO_KEY_ATTACH=1: the authored view-space key in [4]) must fail KEY LIGHT;
raw_bound (WWE_RDR_NO_MAP_CHAIN=1 + WWE_RDR_RAW_RIM_BOUND=1: a raw rim map bound as if filtered) must fail RAW. Exit 0 = pass (or, with --plant, its token was found).
"""
import argparse, json, os, re, subprocess, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'solver'))
sys.path.insert(0, HERE)
import capread  # noqa: E402
import feature_diff as fd  # noqa: E402

MAIN = os.environ.get('RDR_MAIN_CHECKOUT') or r'C:\Users\Jarrod\wwe2k14-recomp'
PLANTS = {
    'black_rim': ({'WWE_RDR_NO_MAP_CHAIN': '1'}, 'RIM SLOT'),
    'key_unrotated': ({'WWE_RDR_NO_KEY_ATTACH': '1'}, 'KEY LIGHT'),
    'raw_bound': ({'WWE_RDR_NO_MAP_CHAIN': '1', 'WWE_RDR_RAW_RIM_BOUND': '1'}, 'RAW'),
}


def game_values(capture, frame):
    case = fd.Case(capture)
    fx = fd.Fx()
    fr = capread.Frame(case.src.read(case.frame_names[frame]))
    for i in range(len(fr.draws)):
        d = fr.draws[i]
        lab, per, _ = fx.eff.resolve(int(d['vs_hash']), int(d['ps_hash']))
        if not lab or not lab.startswith('yCh_skin'):
            continue
        ps = {f: (r, c) for f, r, c in per.get('fragment', [])}
        if 'DifLghVec' not in ps or ps['DifLghVec'][1] < 5 or 'RimLightDepth' not in ps:
            continue
        b = int(d['ps_bank'])
        out = {'DifLghVec4': fr.reg(b, ps['DifLghVec'][0] + 4, 1)[0][:3], 'DifLghCol4': fr.reg(b, ps['DifLghCol'][0] + 4, 1)[0][:3], 'RimLightDepth': fr.reg(b, ps['RimLightDepth'][0], 1)[0]}
        out['KeyLightVec'] = out['DifLghVec4']   # the five-light skin fragment reads the key as DifLghVec[4] (c12): the same register as KeyLightVec
        if 'KeyLightVec' in ps:
            out['KeyLightVec'] = fr.reg(b, ps['KeyLightVec'][0], 1)[0][:3]
        return out
    return None


def ours_values(dump):
    best = None
    for line in open(dump, errors='replace'):
        d = json.loads(line)
        if d.get('k') != 'draw' or not str(d.get('shader', '')).startswith('yCh_skin') or 'shadow' not in d.get('ps', ''):
            continue
        v = d['vals']
        if len(v.get('DifLghVec', [])) < 20:
            continue
        best = dict(slot15=d['slots'].get('15'), DifLghVec4=v['DifLghVec'][16:19], DifLghCol4=v['DifLghCol'][16:19], KeyLightVec=v.get('KeyLightVec', [0, 0, 0])[:3], RimLightDepth=v['RimLightDepth'])
        break
    allslots = [json.loads(l)['slots'].get('15') for l in open(dump, errors='replace') if '"yCh_' in l and '"k":"draw"' in l and 'shadow' in l]
    return best, allslots


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--frame', type=int, default=200)
    ap.add_argument('--capture', default=os.path.join(MAIN, 'out', 'captures', 'brock_losswin'))
    ap.add_argument('--plant', default=None, choices=[None] + list(PLANTS))
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 's103', 'L7', 'const'))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    tag = a.plant or 'clean'
    dump = os.path.join(a.out, 'dump_%s.jsonl' % tag)
    if os.path.exists(dump):
        os.remove(dump)
    env = dict(os.environ, CLAUDECODE='1', WWE_FVIEW_ALLOW_STALE='1', RDR_MAIN_CHECKOUT=MAIN, WWE_RDR_RIG_BODY_SCALE_ONE='1', WWE_RDR_FEATURE_DUMP=dump)
    if a.plant:
        env.update(PLANTS[a.plant][0])
    work = os.path.join(a.out, 'rig_%s' % tag)
    r = subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'scene', 'frame_rig.py'), '--maps-check', '--capture', a.capture, '--frames', str(a.frame), '--out', work], cwd=ROOT, env=env,
                       capture_output=True, text=True)
    rep = os.path.join(work, 'frame_%d_report.txt' % a.frame)
    if not os.path.exists(rep) or not os.path.exists(dump):
        print('FAIL  the rig wrote no report / dump: ' + (r.stdout + r.stderr)[-300:])
        return 1
    text = open(rep, errors='replace').read()
    ours, slots = ours_values(dump)
    game = game_values(a.capture, a.frame)
    if not ours or not game:
        print('FAIL  no wrestler skin draw with five lights on one side (ours %s, game %s)' % (bool(ours), bool(game)))
        return 1
    fails = []
    black = [s for s in slots if s != 'map']
    if not slots or black:
        fails.append('RIM SLOT: %d of %d shadow-variant wrestler draws bind slot 15 to %s' % (len(black) if slots else 0, len(slots), sorted(set(black)) or 'nothing'))
    kv = np.abs(np.array(ours['KeyLightVec']) - np.array(game['KeyLightVec'])).max()
    k4 = np.abs(np.array(ours['DifLghVec4']) - np.array(game['DifLghVec4'])).max()
    kc = np.abs(np.array(ours['DifLghCol4']) - np.array(game['DifLghCol4'])).max()
    if kv > 0.03 or k4 > 0.03 or kc > 0.05:
        fails.append('KEY LIGHT: ours KeyLightVec %s [4] %s col %s, game %s %s %s (max |d| %.3f / %.3f / %.3f)' % (np.round(ours['KeyLightVec'], 3), np.round(ours['DifLghVec4'], 3), np.round(ours['DifLghCol4'], 3),
                     np.round(game['KeyLightVec'], 3), np.round(game['DifLghVec4'], 3), np.round(game['DifLghCol4'], 3), kv, k4, kc))
    g, o = game['RimLightDepth'], ours['RimLightDepth']
    width = float(g[1] - g[0])
    rule_game = abs(float(g[2]) - width / 1000.0)
    rule_ours = abs(float(o[2]) - (o[1] - o[0]) / 1000.0)
    if rule_game > 2e-4 or rule_ours > 2e-4 or abs(o[0] - g[0]) > 0.25 * width or abs(o[1] - g[1]) > 0.25 * width:
        fails.append('RIM PARAMS: ours (%.1f, %.1f, %.4f) game (%.1f, %.1f, %.4f); z rule |d| game %.5f ours %.5f' % (o[0], o[1], o[2], g[0], g[1], g[2], rule_game, rule_ours))
    m = re.search(r'chains: shadow filtered (\d+) \(passes \d+\), rim filtered (\d+)', text)
    if not m or int(m.group(1)) == 0 or int(m.group(2)) == 0:
        fails.append('RAW: the census does not show both chains run (%s)' % (m.group(0) if m else 'no [l7] line'))
    print('ours  KeyLightVec %s [4] %s col4 %s RimLightDepth %s slot15 %s (%d draws)' % (np.round(ours['KeyLightVec'], 4), np.round(ours['DifLghVec4'], 4), np.round(ours['DifLghCol4'], 3), np.round(o, 3), ours['slot15'], len(slots)))
    print('game  KeyLightVec %s [4] %s col4 %s RimLightDepth %s' % (np.round(game['KeyLightVec'], 4), np.round(game['DifLghVec4'], 4), np.round(game['DifLghCol4'], 3), np.round(g, 3)))
    if a.plant:
        want = PLANTS[a.plant][1]
        hit = [f for f in fails if f.startswith(want)]
        print(('PASS' if hit else 'FAIL') + '  plant %s: %s' % (a.plant, hit[0][:200] if hit else 'the token %s was NOT found (fails: %s)' % (want, [f[:30] for f in fails])))
        return 0 if hit else 1
    if fails:
        print('FAIL  ' + '\n      '.join(fails))
        return 1
    print('PASS  RIM SLOT, KEY LIGHT, RIM PARAMS, RAW: slot 15 bound to the filtered rim map on %d draws, key light [4] within %.4f of the game, rim z == (far - near) / 1000' % (len(slots), max(kv, k4)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
