#!/usr/bin/env python3
r"""S103 lane L13b: the arena meshes whose light-map slot names a STAGE PAGE texture (fade_lightmap00 / fade_lightmap01 / lightmap00) are drawn in a MATCH with the game's plain vertex-LM
class, not the LM pixel pair. 0 game runs, 0 Surface runs.

  py -I tools\scene\l13_check.py --case out\s103\run28_case.zip [--frame 622] [--plant] [--expect-fail TOKEN]

Rule [C arena 4; docs\formats\arena_lightmap.md "S103 L13b"]: an arena `yBG_Default` mesh whose sampLightMapPrm names one of the stage's own page textures has no light map of its own
(Material::lm_stage_page); its record word +0x90 (27 of 27 = 1) picks the program as for every other yBG_Default mesh (L10).
CHECK 1 (the game): in the case's stream frame, every full yBG_Default draw joined (by vertex positions) to such a file mesh is `yBG_Default_vertex_lm_vertex` (>= 2 draws).
CHECK 2 (ours): the offline rig's feature dump of the same frame: every draw of a slot whose meshes are all of that class is `yBG_Default_vertex_lm_vertex` (>= 4 draws), none takes the
  `yBG_DefaultLM_*` pair. The planted twin WWE_RDR_L13B_PLANT=1 (the old rule: the LM pixel pair) must FAIL with WRONG CLASS.
Numbers only; nothing from the game is written. Exit 1 on any FAIL."""
import argparse
import collections
import json
import os
import struct
import subprocess
import sys
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'arena'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'fview'))
import feature_diff as fd  # noqa: E402
import arena_files as af  # noqa: E402

STAGE_PAGES = ('fade_lightmap00', 'fade_lightmap01', 'lightmap00')


def stage_page_meshes(a):
    """{(slot, mesh)} of yBG_Default meshes whose light-map slot names a stage page texture, and the slots made only of them."""
    meshes = {(s, m.index) for s, ms in a.slots.items() for m in ms if m.effect == 'yBG_Default' and m.samplers.get('sampLightMapPrm') in STAGE_PAGES}
    pure = {s for s, ms in a.slots.items() if ms and all((s, m.index) in meshes for m in ms)}
    return meshes, pure


def game_side(case_path, frame, a, meshes):
    fx = fd.Fx()
    case = fd.Case(case_path)
    fr, g, _ = fd.game_frame(case, fx, frame)
    buf = fr.buf
    nsec = struct.unpack_from('<I', buf, 12)[0]
    secs = {}
    for i in range(nsec):
        sid, es, cnt, off = struct.unpack_from('<IIQQ', buf, 32 + 24 * i)
        secs[sid] = (es, cnt, off)
    lay = np.frombuffer(buf, np.dtype([('ef', '<u4'), ('ec', '<u4'), ('hs', '<u4'), ('gs', '<u4')]), secs[11][1], secs[11][2])
    pos = {}
    for (s, mi) in meshes:
        m = a.slots[s][mi]
        if m.positions is not None:
            for q in m.positions:
                pos.setdefault(tuple(np.round(q, 1)), set()).add((s, mi))
    geo = zipfile.ZipFile(case_path).open('capture/geometry.bin')
    found = []
    for di in range(len(fr.draws)):
        d = fr.draws[di]
        vs = g[di]['vs'] or ''
        if not vs.startswith('yBG_Default') or int(d['vertex_bytes']) == 0 or int(d['layout']) >= len(lay):
            continue
        st = int(lay[int(d['layout'])]['hs'])
        geo.seek(int(d['vertex_offset']))
        raw = geo.read(min(int(d['vertex_bytes']), st * 8))
        cand = collections.Counter()
        for q in range(len(raw) // st):
            for k in pos.get(tuple(np.round(np.frombuffer(raw, '<f4', 3, q * st), 1)), ()):
                cand[k] += 1
        if cand and cand.most_common(1)[0][1] >= 3:
            found.append((di, cand.most_common(1)[0][0], vs))
    return found


def ours_side(case_path, frame, out, plant):
    import frame_rig
    os.makedirs(out, exist_ok=True)
    rsc = os.path.join(out, 'frame_%06d.rsc' % frame)
    with zipfile.ZipFile(case_path) as z:
        open(rsc, 'wb').write(z.read('capture/scene/frame_%06d.rsc' % frame))
    dump = os.path.join(out, 'fdump.jsonl')
    if os.path.exists(dump):
        os.remove(dump)
    env = dict(os.environ)
    env['WWE_RDR_FEATURE_DUMP'] = dump
    env['RDR_MAIN_CHECKOUT'] = frame_rig.main_checkout()
    if plant:
        env['WWE_RDR_L13B_PLANT'] = '1'
        env['WWE_RDR_D1_OLD'] = '1'   # S103 PLT: the read rule (D1, front_end.cpp RuleFor) now draws every stage-slot mesh, so the old-heuristic faults planted here (WWE_RDR_*_PLANT) are never reached on it; the twin runs the heuristics (WWE_RDR_D1_OLD=1 = the shipped fallback for no table / unsupported pair). docs/research/s103/PLT_plant_twins.md
    else:
        env.pop('WWE_RDR_L13B_PLANT', None)
        env.pop('WWE_RDR_D1_OLD', None)
    game_dir = os.path.join(frame_rig.main_checkout(), 'game_data', 'extracted')
    p = subprocess.run([os.path.join(ROOT, 'out', 'tools', 'frame_rig.exe'), '--record', rsc, '--game', game_dir, '--size', '320x180', '--prefix', os.path.join(out, 'dump')],
                       capture_output=True, text=True, errors='replace', env=env, timeout=1200)
    if not os.path.exists(dump):
        raise SystemExit('FAIL  the rig wrote no feature dump (exit %d): %s' % (p.returncode, (p.stdout + p.stderr)[-600:]))
    draws, begins = [], 0
    for line in open(dump, encoding='utf-8'):
        j = json.loads(line)
        if j.get('k') == 'pass' and j.get('name') == 'frame_begin':
            begins += 1
        elif j.get('k') == 'draw' and begins <= 1:
            draws.append(j)
    return draws


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--case', required=True)
    ap.add_argument('--frame', type=int, default=622)
    ap.add_argument('--plant', action='store_true', help='WWE_RDR_L13B_PLANT=1: the old rule (the LM pixel pair); must FAIL')
    ap.add_argument('--expect-fail', default=None, metavar='TOKEN')
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 's103', 'L13', 'gate'))
    a_ = ap.parse_args()
    case_path = a_.case if os.path.isabs(a_.case) else os.path.join(ROOT, a_.case)
    if not os.path.exists(case_path):
        import frame_rig
        case_path = os.path.join(frame_rig.main_checkout(), a_.case)
    arena = af.load_arena(4)
    meshes, pure = stage_page_meshes(arena)
    fails = []
    print('file: %d yBG_Default meshes with a stage-page light-map slot, %d slots made only of them' % (len(meshes), len(pure)))
    if len(meshes) < 20 or not pure:
        fails.append('FILE CLASS EMPTY (%d meshes)' % len(meshes))
    g = game_side(case_path, a_.frame, arena, meshes)
    bad_g = [x for x in g if not x[2].startswith('yBG_Default_vertex_lm_vertex')]
    print('game frame %d: %d full draws of the class: %s' % (a_.frame, len(g), ', '.join('draw %d slot %d:%d %s' % (di, k[0], k[1], vs) for di, k, vs in g)))
    if len(g) < 2:
        fails.append('GAME SIDE TOO FEW (%d draws joined)' % len(g))
    if bad_g:
        fails.append('GAME SIDE NOT VERTEX-LM (%d draws): the rule does not hold' % len(bad_g))
    o = ours_side(case_path, a_.frame, os.path.join(a_.out, 'plant' if a_.plant else 'main'), a_.plant)
    mine = [d for d in o if d.get('part', '').startswith('slot ') and int(d['part'].split()[1]) in pure and (d.get('vs') or '').startswith('yBG_')]
    c = collections.Counter((d['vs'], d['ps']) for d in mine)
    print('ours frame %d: %d draws of those slots: %s' % (a_.frame, len(mine), dict(c)))
    wrong = [d for d in mine if d['vs'] != 'yBG_Default_vertex_lm_vertex' or d['ps'] != 'yBG_Default_base_fragment']
    if len(mine) < 4:
        fails.append('OURS TOO FEW (%d draws)' % len(mine))
    if wrong:
        fails.append('WRONG CLASS: %d of %d draws are not yBG_Default_vertex_lm_vertex + _base_fragment' % (len(wrong), len(mine)))
    for f in fails:
        print('FAIL  ' + f)
    text = '\n'.join(fails)
    if a_.expect_fail:
        if fails and a_.expect_fail in text:
            print('OK (planted fault found: %s)' % a_.expect_fail)
            return 0
        print('FAIL  the planted fault was NOT reported (%s)' % a_.expect_fail)
        return 1
    print('l13_check: %s' % ('FAIL' if fails else 'PASS'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
