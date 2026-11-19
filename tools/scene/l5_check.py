#!/usr/bin/env python3
r"""S103 lane L5: the arena STRUCTURE (aisle stairs, tier walls) drawn from the scene record is not brighter than the game's in a match. 0 game runs, 0 Surface runs.

  py -I tools\scene\l5_check.py --case out\s103\run27_case.zip [--frame 641] [--plant] [--expect-fail TOKEN]

Cause [C] (docs\formats\arena_slot_flags.md "S103 L5"): the game reads the stage light-map page for every non-translucent arena yBG_Default / yBG_UVScroll / yProp_Default mesh in a match as in an
entrance (the stream's draws carry the page at vertex-fetch slot 16 = image 388; arena 4: every mesh of a slot without the `a` / `s` flag takes an LM variant, `sampLightMapPrm` or not).
Ours drew a mesh with no light-map slot of its own (the stairs, tier walls: msg_stair / msg_wall0 ...) PLAIN in a match (only an entrance gave it the page): unshaded. Second part: the game's plain
variant has NO highlight lobe for those materials (level 0.5, DXT1): ours took a `_specular_alpha_spec` pair.
CHECK 1 (the rig): the offline rig draws the case's last scene record; the game's presented picture of that frame is the case's picture.png. Three region classes (aisle stairs, tier-wall ledge, upper
wall) are compared as mean-colour ratios ours / game over matched pixels; each class mean must be <= BAR (1.5). The regions contain the crowd in front of them (the crowd is lane L4's and matches), so the
bar is a ceiling for the structure, not an equality. Before the fix (planted twin: WWE_RDR_L5_PLANT=1) the classes read 1.71 / 1.39 / 1.62; after 1.41 / 1.27 / 1.44.
CHECK 2 (the rule): tools\arena\variant_rule.py --check highlight (the plain / highlight split over the 244 yBG / yProp_Default meshes of the arena-4 table: 244 / 244 with one named exception).
The page itself (the game's runtime image 388 against our fade_lightmap01) is [U]: the remaining 1.2-1.4x is not shown to be that. Numbers only; nothing from the game is written.
Exit 1 on any FAIL."""
import argparse
import io
import os
import subprocess
import sys
import zipfile

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import pad_check as pc  # noqa: E402  (rig_from_case: unpack the frames + the record, run frame_rig.py)

BAR = 1.5
REGIONS = {  # full-res (2560x1440) boxes in the picture of frame 641 (the aisle stairs left of the ring post, the tier-wall ledge, the wall above it)
    'stairs': (371, 460, 550, 770),
    'ledge_wall': (560, 540, 1100, 665),
    'wall_upper': (560, 300, 1100, 540),
}


def ratios(game, ours):
    out = {}
    for k, (x0, y0, x1, y1) in REGIONS.items():
        a = game[y0:y1, x0:x1].reshape(-1, 3).mean(0)
        b = ours[y0:y1, x0:x1].reshape(-1, 3).mean(0)
        out[k] = (b / np.maximum(a, 1e-3))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--case', required=True)
    ap.add_argument('--frame', type=int, default=641)
    ap.add_argument('--plant', nargs='?', const='1', default=None, help='WWE_RDR_L5_PLANT: 1 = the old behaviour (no page for a match arena meshes, a highlight pair for a level > 0); submodel = only the SubModel page hand-over (rf_c_core.cpp) is skipped')
    ap.add_argument('--expect-fail', default=None, metavar='TOKEN')
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 's103', 'L5', 'gate'))
    a = ap.parse_args()
    out = os.path.join(a.out, ('plant_' + a.plant) if a.plant else 'main')
    if a.plant:
        os.environ['WWE_RDR_L5_PLANT'] = a.plant
        if a.plant != 'submodel':   # (the submodel plant is in rf_c_core, before the draw path: unaffected)
            os.environ['WWE_RDR_D1_OLD'] = '1'   # S103 PLT: the read rule (D1, front_end.cpp RuleFor) now draws every stage-slot mesh, so the old-heuristic faults planted here (WWE_RDR_*_PLANT) are never reached on it; the twin runs the heuristics (WWE_RDR_D1_OLD=1 = the shipped fallback for no table / unsupported pair). docs/research/s103/PLT_plant_twins.md
    else:
        os.environ.pop('WWE_RDR_L5_PLANT', None)
        os.environ.pop('WWE_RDR_D1_OLD', None)
    # S103 main: this check tests the light-map page / variant rule, so it measures the scene WITHOUT our post (lane P2 made the rig draw post;
    # the post's own brightness on the stands (+~13 %, 1.41 -> 1.60 stairs) is lane P2's question, not this rule's).
    os.environ['WWE_RDR_NO_SCENE_POST'] = '1'
    panel, _cam = pc.rig_from_case(a.case, a.frame, out, False)
    whole = os.path.join(os.path.dirname(panel), 'ours_%d_whole.png' % a.frame)
    with zipfile.ZipFile(a.case) as z:
        game = np.asarray(Image.open(io.BytesIO(z.read('picture.png'))).convert('RGB'), float)
    ours = np.asarray(Image.open(whole).convert('RGB').resize((game.shape[1], game.shape[0])), float)
    r = ratios(game, ours)
    fails = []
    for k, v in r.items():
        m = float(v.mean())
        print('%-10s ours / game per channel %s  mean %.2f  (bar %.2f)' % (k, np.round(v, 2), m, BAR))
        if m > BAR:
            fails.append('STRUCTURE TOO BRIGHT %s %.2f' % (k, m))
    # CHECK 3 (review fix 1): every arena LM-class mesh of a MATCH record (slot < 951, not a movie surface 800-809) must have the stage page in its model (stage_lm >= 0): a sub-model built from a
    # slot with no own-LM mesh (rf_c_core.cpp SubModel) got -1 and was drawn plain with no error. The dev dump prints `stage_lm=N` per mesh.
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'fview'))
    import frame_rig  # noqa: E402
    rsc = os.path.join(out, 'capture', 'scene', 'frame_%06d.rsc' % a.frame)
    game_dir = os.path.join(frame_rig.main_checkout(), 'game_data', 'extracted')
    env = dict(os.environ)
    env['WWE_RDR_DUMP_MATS'] = '1'
    p = subprocess.run([os.path.join(ROOT, 'out', 'tools', 'frame_rig.exe'), '--record', rsc, '--game', game_dir, '--size', '320x180', '--prefix', os.path.join(out, 'dump')],
                       capture_output=True, text=True, errors='replace', env=env, timeout=900)
    import re
    n = bad = 0
    for line in (p.stdout + p.stderr).splitlines():
        m = re.match(r'\[mats-lit\] (yBG_Default|yBG_UVScroll|yProp_Default) part=slot (\d+) .* stage_lm=(-?\d+)', line)
        if not m or int(m.group(2)) >= 951 or 800 <= int(m.group(2)) <= 809:
            continue
        n += 1
        bad += int(m.group(3)) < 0
    print('sub-model page: %d arena LM-class meshes, %d without the stage page' % (n, bad))
    if n == 0 or bad:
        fails.append('SUB-MODEL PAGE MISSING %d of %d' % (bad, n))
    rc = subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'arena', 'variant_rule.py'), os.path.join(ROOT, 'tools', 'arena', 'arena4_variants.json'), '--check', 'highlight'],
                        capture_output=True, text=True, errors='replace')
    print(rc.stdout.strip().splitlines()[0] if rc.stdout.strip() else rc.stderr[-200:])
    if rc.returncode != 0:
        fails.append('HIGHLIGHT RULE')
    for f in fails:
        print('FAIL  ' + f)
    text = '\n'.join(fails)
    if a.expect_fail:
        if fails and a.expect_fail in text:
            print('OK (planted fault found: %s)' % a.expect_fail)
            return 0
        print('FAIL  the planted fault was NOT reported (%s)' % a.expect_fail)
        return 1
    print('l5_check: %s' % ('FAIL' if fails else 'PASS'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
