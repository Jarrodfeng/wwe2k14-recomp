#!/usr/bin/env python3
"""Lane 28 acceptance test (offline, 0 game runs): the arena as a live scene contributor (class C3).

  py -I tools\\scene\\arena_selftest.py [--root DIR] [--arena 5] [--size 640x360] [--time 132]

Needs under --root: out\\tools\\arena_rec.exe (tools\\scene\\build_arena_rec.ps1), out\\tools\\rsc_draw.exe + rsc_dump.exe (tools\\scene\\build_rsc.ps1); the
viewer (wwe2k14_assets_rdr.exe, tools\\assets\\build_viewer.ps1 -Backend rdr) is taken from --root or, failing that, the main checkout's out\\tools.
Output in out\\s101\\arena\\ of --root.
 1. The viewer records the arena (camera) and shoots it on the vk backend. arena_rec builds the arena contributor's record from the FILES (evaluated
    light sets at --time, the slot text) through the real Emit, with that camera. rsc_draw (rdr) draws it: validation 0, the tile check against the
    viewer's vk shot, and the picture equals what rsc_draw makes of the viewer's own whole-arena record (the contributor loses nothing).
 2. Content: one object per slot, every 50001 r / y twin present (count equals the text's), 6 lights per set (4 directional + sky + ground), the screens listed as unknown + counted.
 3. Planted faults, each a class of past bug, each must FAIL the same comparison: swap_sets (every slot lit by the wrong set: the "guessing what a
    thing is" class), dirs (every light direction negated: Session 98's entrance light direction), no_twins (the r / y twins dropped: Session 97's
    half arena; also caught structurally against the text).
A check that cannot read its input fails loudly (never a pass)."""
import argparse
import importlib.util
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = os.environ.get('RDR_MAIN_CHECKOUT') or os.path.normpath(os.path.join(HERE, '..', '..'))
# the picture bars (calibrated on arena 5, see the runs in docs\systems\arena_look.md "What is live"):
SAME_MAX = 0.05    # mean |difference| (0..255 per channel) between the contributor draw and the viewer-record draw: the rdr draw is deterministic (measured 0.000)
PLANT_MIN = 0.5    # a planted fault must move the picture by at least this much from the clean contributor draw, or fail the tile check (measured 1.4-28)


def load_lane6():
    spec = importlib.util.spec_from_file_location('lane6_selftest', os.path.join(HERE, 'selftest.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def run(cmd, env_extra=None, timeout=600):
    env = dict(os.environ, **(env_extra or {}))
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
    return p.returncode, p.stdout + p.stderr


def mean_abs(a, b):
    """Mean |a - b| over every colour channel of two pictures (read_png tuples), or None when the sizes differ."""
    if (a[0], a[1]) != (b[0], b[1]):
        return None
    ca, cb = a[3], b[3]
    tot = n = 0
    for ra, rb in zip(a[2], b[2]):
        for x in range(a[0]):
            for c in range(3):
                tot += abs(ra[x * ca + c] - rb[x * cb + c])
                n += 1
    return tot / n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    ap.add_argument('--arena', type=int, default=5)
    ap.add_argument('--size', default='640x360')
    ap.add_argument('--time', type=float, default=132)
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    tools = os.path.join(root, 'out', 'tools')
    rec, draw, dump = (os.path.join(tools, n) for n in ('arena_rec.exe', 'rsc_draw.exe', 'rsc_dump.exe'))
    viewer = os.path.join(tools, 'wwe2k14_assets_rdr.exe')
    if not os.path.exists(viewer):
        viewer = os.path.join(MAIN, 'out', 'tools', 'wwe2k14_assets_rdr.exe')
    game = os.path.join(MAIN, 'game_data', 'extracted')
    for need in (rec, draw, dump, viewer, game):
        if not os.path.exists(need):
            print(f'FAIL  cannot read its input: {need}')
            return 1
    out = os.path.join(root, 'out', 's101', 'arena')
    os.makedirs(out, exist_ok=True)
    w, h = (int(x) for x in args.size.split('x'))
    lane6 = load_lane6()
    fails = []
    P = lambda *n: os.path.join(out, *n)

    # ---- 1. the viewer: a record (camera + the whole-arena object) and the vk shot ----
    look = {'cmd': 'look', 'mode': 'lit', 'shadows': False, 'dof': False, 'fxaa': False, 'bloom': False, 'glare': False, 'gamma': False, 'haze': False}
    viewer_rsc, shot = P('viewer.rsc'), P('vk.png')
    for p in (viewer_rsc, shot):
        if os.path.exists(p):
            os.remove(p)
    with open(P('viewer.jsonl'), 'w') as f:
        for c in (look, {'cmd': 'load', 'kind': 'arena', 'id': args.arena}, {'cmd': 'stage_key', 'time': args.time} if False else None,
                  {'cmd': 'record', 'path': viewer_rsc, 'preset': 'ring', 'w': w, 'h': h},
                  {'cmd': 'shot', 'path': shot, 'preset': 'ring', 'w': w, 'h': h}, {'cmd': 'quit'}):
            if c:
                f.write(json.dumps(c) + '\n')
    with open(P('viewer.out.txt'), 'w') as o:
        p = subprocess.run([viewer, '--backend', 'vk', '--game', game, '--script', P('viewer.jsonl'), '--exit', '--hidden'], stdout=o, stderr=subprocess.STDOUT, timeout=600)
    if not (os.path.exists(viewer_rsc) and os.path.exists(shot)):
        print(f'FAIL  the viewer wrote no record / shot (exit {p.returncode}); see {P("viewer.out.txt")}')
        return 1
    ref = lane6.read_png(shot)

    def build(name, plant=None, extra=()):
        out_rsc = P(name + '.rsc')
        if os.path.exists(out_rsc):
            os.remove(out_rsc)
        cmd = [rec, '--game', game, '--arena', str(args.arena), '--time', str(args.time), '--camera-from', viewer_rsc, '--out', out_rsc]
        if plant:
            cmd += ['--plant', plant]
        cmd += list(extra)
        rc, txt = run(cmd)
        if rc != 0 or not os.path.exists(out_rsc):
            print(f'FAIL  arena_rec {name}: exit {rc}: {txt.strip()[-400:]}')
            return None, None
        kv = {}
        for m in re.finditer(r'\[arena_rec\] ((?:\w+=\S+ ?)+)', txt):  # both result lines
            kv.update(dict(x.split('=') for x in m.group(1).split()))
        return out_rsc, kv

    def draw_pic(name, src_rsc):
        png = P(name + '.png')
        if os.path.exists(png):
            os.remove(png)
        for attempt in range(6):  # the GPU is shared with other tools: "Not enough graphics memory" is transient, anything else is not retried
            rc, txt = run([draw, src_rsc, '--game', game, '--out', png, '--size', args.size], {'WWE_RDR_VALIDATION': '1'})
            if rc == 0 or 'Not enough graphics memory' not in txt:
                break
            time.sleep(10)
        if rc != 0 or not os.path.exists(png):
            print(f'FAIL  rsc_draw {name}: exit {rc}: {txt.strip()[-400:]}')
            return None, None, txt
        errs = [int(x) for x in re.findall(r'validation errors: (\d+)', txt)]
        return lane6.read_png(png), errs, txt

    # ---- 2. the clean contributor record ----
    clean_rsc, kv = build('clean')
    if not clean_rsc:
        return 1
    rc, dtxt = run([dump, clean_rsc])
    if rc != 0:
        print('FAIL  rsc_dump cannot read the contributor record: ' + dtxt.strip()[-300:])
        return 1
    n_slot_objs = dtxt.count('"arena:slot:')
    n_twin_r, n_twin_y = dtxt.count(':r"'), dtxt.count(':y"')
    objects, slots, twins, expect, sets, lights, screens = (int(kv.get(k, -1)) for k in ('objects', 'slots', 'twins', 'expect_twins', 'sets', 'lights', 'screens'))
    ok = slots > 100 and twins > 0 and twins == expect and n_twin_r + n_twin_y == twins and lights == 6 * sets and sets > 0 and n_slot_objs == slots + twins
    print(('PASS' if ok else 'FAIL') + f'  contributor record: {slots} slots, {n_slot_objs} slot objects, twins {twins} (text says {expect}; dump shows {n_twin_r} r + {n_twin_y} y), '
          f'{sets} light sets = {lights} lights, {screens} screens (unknown planes counted: unknown={kv.get("unknown")})')
    if not ok:
        fails.append('content')
    ok = int(kv.get('unknown', -1)) >= screens > 0
    print(('PASS' if ok else 'FAIL') + f'  the {screens} screen surfaces are listed with unknown planes and counted (unknown={kv.get("unknown")})')
    if not ok:
        fails.append('screens not counted')

    clean, errs, txt = draw_pic('clean', clean_rsc)
    if clean is None:
        return 1
    ok = bool(errs) and max(errs) == 0
    print(('PASS' if ok else 'FAIL') + f'  rsc_draw validation errors: {errs}')
    if not ok:
        fails.append('validation')
    print('      ' + ' '.join(l.strip() for l in txt.splitlines() if '[rsc_draw] arena' in l))
    t_ok, notes = lane6.compare(ref, clean)
    print(('PASS' if t_ok else 'FAIL') + '  tile check, contributor record drawn by rsc_draw vs the viewer vk shot: ' + '; '.join(notes))
    if not t_ok:
        fails.append('tile check')
    base, _, _ = draw_pic('viewer_record', viewer_rsc)
    if base is None:
        return 1
    d_same = mean_abs(base, clean)
    ok = d_same is not None and d_same <= SAME_MAX
    print(('PASS' if ok else 'FAIL') + f'  the contributor loses nothing: mean |diff| to the draw of the viewer\'s own whole-arena record = {d_same:.3f} (bar <= {SAME_MAX})')
    if not ok:
        fails.append('contributor differs from the viewer record')

    # ---- 3. planted faults ----
    for plant, why in (('swap_sets', 'every slot lit by the wrong light set (the "guessing what a thing is" class, ledger RENDERER_ARC.md 2.1)'),
                       ('dirs', 'every light direction negated (Session 98: LESSONS "Look fixes verified one at a time can cancel")'),
                       ('no_twins', "the 50001 r / y twins dropped (Session 97's half arena)")):
        prsc, pkv = build('plant_' + plant, plant)
        if not prsc:
            fails.append(plant + ': did not build')
            continue
        pic, perrs, _ = draw_pic('plant_' + plant, prsc)
        if pic is None:
            fails.append(plant + ': did not draw')
            continue
        d = mean_abs(clean, pic)
        t2, n2 = lane6.compare(ref, pic)
        caught = (d is not None and d >= PLANT_MIN) or not t2
        print(('PASS' if caught else 'FAIL') + f'  planted {plant} ({why}) is caught: moves the picture by {d:.3f} (bar >= {PLANT_MIN}); tile check passes={t2}')
        if not caught:
            fails.append(plant + ' not caught')
        if plant == 'no_twins':
            tw, ex = int(pkv.get('twins', -1)), int(pkv.get('expect_twins', -1))
            ok = tw < ex
            print(('PASS' if ok else 'FAIL') + f'  planted no_twins is caught structurally too: record has {tw} twins, the text says {ex}')
            if not ok:
                fails.append('no_twins structural')

    # ---- 4. review fixes (lane 28 review F1 / F2 / F5 / F6): the record the GAME hook would publish ----
    lrsc, lkv = build('live_shaped', None, ['--live-shaped', '--look', 'unknown', '--provider-check'])
    if not lrsc:
        fails.append('live-shaped did not build')
    else:
        ok = lkv.get('fall_kept') == '1'
        print(('PASS' if ok else 'FAIL') + f'  F1 live-shaped record (falloff unknown = -1, 5 lights): the model own cube falloff survives ApplyToModel (fall_kept={lkv.get("fall_kept")})')
        if not ok:
            fails.append('F1 falloff overwritten')
        zrsc, zkv = build('plant_zero_fall', 'zero_fall')
        ok = bool(zrsc) and zkv.get('fall_kept') == '0'
        print(('PASS' if ok else 'FAIL') + f'  F1 planted zero_fall (the old bug: unknown written as 0) IS caught: fall_kept={zkv.get("fall_kept") if zrsc else "no build"}')
        if not ok:
            fails.append('F1 plant not caught')
        inv, mark, when_n, unk = (int(lkv.get(k, -1)) for k in ('invisible', 'vis_marked', 'when_slots', 'unknown'))
        ok = when_n > 0 and inv >= when_n and mark == inv and unk >= inv
        print(('PASS' if ok else 'FAIL') + f'  F2 unknown look: the {when_n} e / g slots (+ twins) are listed NOT visible ({inv}), each marked visibility_unknown ({mark}) and counted (unknown={unk})')
        if not ok:
            fails.append('F2 visibility')
        ok = int(clean_kv_inv := int(kv.get('invisible', -1))) == 0
        print(('PASS' if ok else 'FAIL') + f'  F2 control: a record under the match look has {clean_kv_inv} invisible objects')
        if not ok:
            fails.append('F2 control')
        # S101 lane 264: the look the hook publishes now comes from the game's scene phase word (arena_contrib.h LookFromScenePhase)
        mrsc, mkv = build('look_word1', None, ['--live-shaped', '--from-provider', '--scene-word', '1'])
        ersc, ekv = build('look_word4', None, ['--live-shaped', '--from-provider', '--scene-word', '4'])
        if mrsc and ersc:
            mw, mv_, mi = int(mkv.get('when_objects', -1)), int(mkv.get('when_visible', -1)), int(mkv.get('invisible', -1))
            ew, ev_, ei = int(ekv.get('when_objects', -1)), int(ekv.get('when_visible', -1)), int(ekv.get('invisible', -1))
            ok = mv_ == mw and mi == 0 and ev_ == ew and ei == 0 and mw + ew > 0 and mw != ew and int(mkv.get('unknown', -1)) < int(lkv.get('unknown', 0))
            print(('PASS' if ok else 'FAIL') + f'  L264 decided look: scene word 1 (match) lists {mw} when-flagged objects, all visible; word 4 (entrance) lists {ew}, all visible (a different set), no invisible object; unknown {mkv.get("unknown")} vs {lkv.get("unknown")} when undecided')
            if not ok:
                fails.append('L264 look decided')
        else:
            fails.append('L264 look builds')
        urc, ukv = build('look_unreadable', None, ['--live-shaped', '--from-provider', '--scene-word', '-1'])
        ok = bool(urc) and int(ukv.get('invisible', -1)) == int(lkv.get('invisible', -2)) and int(ukv.get('vis_marked', -1)) == int(lkv.get('vis_marked', -2))
        print(('PASS' if ok else 'FAIL') + f'  L264 plant (an unreadable scene word = the look stays Unknown) IS caught: {ukv.get("invisible") if urc else "no build"} invisible / {ukv.get("vis_marked") if urc else "-"} marked, same as the undecided record')
        if not ok:
            fails.append('L264 plant not caught')
        ok = lkv.get('provider_missing') == '0' and lkv.get('provider_mismatch') == '0' and int(lkv.get('provider_extra', -1)) >= 0
        print(('PASS' if ok else 'FAIL') + f'  F5 the game-side slot provider (pac directory + text) lists every slot the viewer model holds (missing {lkv.get("provider_missing")}, rows differing {lkv.get("provider_mismatch")}, extra {lkv.get("provider_extra")})')
        if not ok:
            fails.append('F5 provider')

    if fails:
        print('FAIL  ' + '; '.join(fails))
        return 1
    print('PASS  lane 28 arena selftest')
    return 0


if __name__ == '__main__':
    sys.exit(main())
