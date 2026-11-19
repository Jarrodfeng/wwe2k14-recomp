#!/usr/bin/env python3
r"""S103 lane L10: the arena's light-map class in a MATCH is the game's (the page in the vertex stage for the vertex-LM class, none for the rest). 0 game runs, 0 Surface runs.

  py -I tools\scene\l10_check.py --case out\s103\run27_case.zip [--frame 641] [--plant] [--expect-fail TOKEN]

Rule [C arena 4, docs\formats\arena_lightmap.md "S103 L10"]: the mesh record word +0x90 bit 0 picks the vertex-LM program (264 of 264 meshes of the variant table; `yProp_*` copies carry value 3 and are not it).
CHECK 1 (the rule, from the files): tools\arena\l10_rule.py reads every arena-4 mesh record and the committed table tools\arena\arena4_vlm.json (mesh -> the game's vertex variant, read from the streams by
  variant_table.py): bit 0 of +0x90 on a yBG_ mesh must equal "the game's vertex variant is *_vertex_lm_vertex" on every mesh in the table.
CHECK 2 (the draw): the offline rig draws the case's last scene record (post ON, then post OFF: lane P2's note, the game's haze is in the presented picture, so post ON is the fair one) and the feature dump
  must hold FX_VERTEX_LM vertex variants for the arena's VLM class and NONE with a pixel page (slot 13) in the arena; the planted twin (WWE_RDR_L10_PLANT=1: the page per pixel for all) must not.
CHECK 3 (the picture): stairs / ledge / wall mean ratios ours / game, reported for post ON and OFF; the bar (post ON) is BAR.
Exit 1 on any FAIL."""
import argparse
import io
import json
import os
import subprocess
import sys
import zipfile

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import pad_check as pc  # noqa: E402
import l5_check as l5  # noqa: E402

BAR = 1.90  # a no-regression ceiling on the post-ON ratios (today 1.60 / 1.35 / 1.86, the same with the L5 per-pixel page: this lever does not move them); the game-matching target is ~1.0 [U: not reached]


def dump_variants(out, frame, rsc, game_dir):
    """The arena draws' (vertex variant, has pixel page slot 13) from the rig's feature dump."""
    dump = os.path.join(out, 'fdump.jsonl')
    if os.path.exists(dump):
        os.remove(dump)
    env = dict(os.environ)
    env['WWE_RDR_FEATURE_DUMP'] = dump
    subprocess.run([os.path.join(ROOT, 'out', 'tools', 'frame_rig.exe'), '--record', rsc, '--game', game_dir, '--size', '320x180', '--prefix', os.path.join(out, 'dump')],
                   capture_output=True, text=True, errors='replace', env=env, timeout=900)
    vlm = pix = plain = pix_all = 0
    for line in open(dump, encoding='utf-8', errors='replace'):
        try:
            d = json.loads(line)
        except ValueError:
            continue
        if d.get('k') != 'draw' or not (d.get('vs', '').startswith('yBG_') and str(d.get('part', '')).startswith('slot ')):
            continue
        if 'vertex_lm' in d['vs']:
            vlm += 1
        else:
            plain += 1
        pix_all += '13' in d.get('slots', {})
        pix += ('13' in d.get('slots', {})) and 'vertex_lm' in d['vs'] and d.get('shader') == 'yBG_Default'
    print('(arena draws with a pixel page of any class: %d: own-LM / UVScroll / other classes keep the L5 rule)' % pix_all)
    return vlm, plain, pix


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--case', required=True)
    ap.add_argument('--frame', type=int, default=641)
    ap.add_argument('--plant', action='store_true', help='WWE_RDR_L10_PLANT: the page per pixel for every arena mesh (the L5 behaviour)')
    ap.add_argument('--expect-fail', default=None, metavar='TOKEN')
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 's103', 'L10', 'gate'))
    a = ap.parse_args()
    fails = []
    # CHECK 1
    rc = subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'arena', 'l10_rule.py')], capture_output=True, text=True, errors='replace')
    print((rc.stdout.strip().splitlines() or [rc.stderr[-200:]])[0])
    if rc.returncode != 0:
        fails.append('VLM RULE')
    if a.plant:
        os.environ['WWE_RDR_L10_PLANT'] = '1'
        os.environ['WWE_RDR_D1_OLD'] = '1'   # S103 PLT: the read rule (D1, front_end.cpp RuleFor) now draws every stage-slot mesh, so the old-heuristic faults planted here (WWE_RDR_*_PLANT) are never reached on it; the twin runs the heuristics (WWE_RDR_D1_OLD=1 = the shipped fallback for no table / unsupported pair). docs/research/s103/PLT_plant_twins.md
    else:
        os.environ.pop('WWE_RDR_L10_PLANT', None)
        os.environ.pop('WWE_RDR_D1_OLD', None)
    os.environ.pop('WWE_RDR_L5_PLANT', None)
    with zipfile.ZipFile(a.case) as z:
        game = np.asarray(Image.open(io.BytesIO(z.read('picture.png'))).convert('RGB'), float)
    res = {}
    for post in ('on', 'off'):
        if post == 'off':
            os.environ['WWE_RDR_NO_SCENE_POST'] = '1'
        else:
            os.environ.pop('WWE_RDR_NO_SCENE_POST', None)
        out = os.path.join(a.out, ('plant' if a.plant else 'main') + '_post_' + post)
        panel, _cam = pc.rig_from_case(a.case, a.frame, out, False)
        whole = os.path.join(os.path.dirname(panel), 'ours_%d_whole.png' % a.frame)
        ours = np.asarray(Image.open(whole).convert('RGB').resize((game.shape[1], game.shape[0])), float)
        r = l5.ratios(game, ours)
        res[post] = {k: float(v.mean()) for k, v in r.items()}
        print('post %-3s ' % post + '  '.join('%s %.2f' % (k, v) for k, v in res[post].items()))
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'fview'))
    import frame_rig  # noqa: E402
    out = os.path.join(a.out, ('plant' if a.plant else 'main') + '_post_on')
    rsc = os.path.join(out, 'capture', 'scene', 'frame_%06d.rsc' % a.frame)
    game_dir = os.path.join(frame_rig.main_checkout(), 'game_data', 'extracted')
    os.environ.pop('WWE_RDR_NO_SCENE_POST', None)
    vlm, plain, pix = dump_variants(out, a.frame, rsc, game_dir)
    print('arena draws: %d vertex-LM variant, %d plain vertex, %d vertex-LM draws with a pixel page (slot 13)' % (vlm, plain, pix))
    if vlm == 0:
        fails.append('NO VERTEX-LM DRAWS')
    if pix:
        fails.append('PIXEL PAGE IN A MATCH %d' % pix)
    for k, v in res['on'].items():
        if v > BAR:
            fails.append('STRUCTURE TOO BRIGHT %s %.2f' % (k, v))
    for f in fails:
        print('FAIL  ' + f)
    text = '\n'.join(fails)
    if a.expect_fail:
        if fails and a.expect_fail in text:
            print('OK (planted fault found: %s)' % a.expect_fail)
            return 0
        print('FAIL  the planted fault was NOT reported (%s)' % a.expect_fail)
        return 1
    print('l10_check: %s' % ('FAIL' if fails else 'PASS'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
