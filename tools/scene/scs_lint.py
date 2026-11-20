#!/usr/bin/env python3
r"""Lane SCS lint (Session 103 overnight; docs\research\s103\RFF.md part B; 0 game runs): the in-scene 2D rows are drawn, in the game's order, and never dropped again.

  py -I tools\scene\scs_lint.py [--root DIR] [--plant NAME]

Checks (text, on the committed sources; each a FAIL when missed):
  order      frame_hooks.cpp AttachFrameHooks pushes the maps hook, then the scene-2D hook, then the particles hook (the game's category 36 draws before the particle node's 47)
  fate       scene_host.cpp FateOf returns Fate::InScene for a kQpSceneColour row when the scene hook is on (the old DropPass for it = every entrance frame refused pass_scene_colour)
  accounting DrawRows counts the in-scene rows from the hook's report (scene2d_rep_) and does not treat them as dropped passes while the hook is on
  wiring     SceneHost::Start installs the hook (SetScene2D) with Context::scene_depth = true; the stream build (renderer_adapter\live.cpp) never calls SetScene2D
  walkhook   S105 A4B review F2: draw_origin.h hooks sub_82C3B200 (begin: OnWalkBegin(ctx.r4.u32) BEFORE __imp__) and sub_82C3B218 (end: OnWalkEnd), not the sub-walker sub_82C2F960; quads2d_tap.h OnOp19 calls walkcat::Mark(t_scope.cur, t_walk) AFTER q::OnOp19(t_scope, and before the scene-colour log
Planted twins (the lint must FAIL on each; the unit's own twin is scs_unit --plant scene_z_ignored):
  fate_drop      FateOf drops scene-colour rows again (the pre-SCS rule)
  order_swapped  the particles hook is pushed before the scene-2D hook
  walk_begin_r3  the begin hook reads ctx.r3 (not the category in r4)
  walk_end_gone  the sub_82C3B218 hook no longer calls OnWalkEnd
  mark_early     walkcat::Mark runs BEFORE q::OnOp19 (cur is still the previous row)
  mark_gone      the walkcat::Mark call is removed
Exit 0 = pass."""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))


def rd(root, rel):
    with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
        return f.read()


def run(root, plant):
    hooks = rd(root, 'src/renderer_scene/frame_hooks.cpp')
    host = rd(root, 'src/renderer_host/scene_host.cpp')
    fate_src = rd(root, 'src/renderer_host/row_fate.h')   # S105 A4B: FateOf moved here (a unit drives it)
    live = rd(root, 'src/renderer_adapter/live.cpp')
    if plant == 'fate_drop':
        fate_src = re.sub(r'if \(r\.pass == rdr_scene::kQpSceneColour && scene_hook[^\n]*return Fate::InScene;', '', fate_src)
    if plant == 'order_swapped':
        a, b = 'frame.stage_hooks.push_back(h.scene2d_.get());', 'frame.stage_hooks.push_back(h.particles_.get());'
        hooks = hooks.replace(a, '@@A@@').replace(b, a).replace('@@A@@', b)
    origin = rd(root, 'src/draw_origin.h')   # S105 A4B review F2
    tap = rd(root, 'src/scene_live/quads2d_tap.h')
    if plant == 'walk_begin_r3':
        origin = origin.replace('OnWalkBegin(ctx.r4.u32)', 'OnWalkBegin(ctx.r3.u32)')
    if plant == 'walk_end_gone':
        origin = origin.replace('wwe2k14::q2d_tap::OnWalkEnd();', '')
    if plant == 'mark_gone':
        tap = re.sub(r'rdr_scene::walkcat::Mark\(t_scope\.cur, t_walk\);', '', tap)
    if plant == 'mark_early':
        m = re.search(r'[^\n]*walkcat::Mark\(t_scope\.cur, t_walk\);[^\n]*\n', tap)
        if m:
            tap = tap.replace(m.group(0), '')
            k = tap.find('q::OnOp19(t_scope,')
            tap = tap[:k] + m.group(0) + tap[k:]
    bad = []
    mb = re.search(r'REX_HOOK_RAW\(sub_82C3B200\)\s*\{(.*?)\n\}', origin, re.S)
    if not mb or not re.search(r'OnWalkBegin\(ctx\.r4\.u32\);\s*__imp__sub_82C3B200', mb.group(1)):
        bad.append('walkhook: REX_HOOK_RAW(sub_82C3B200) must call OnWalkBegin(ctx.r4.u32) before __imp__sub_82C3B200 (r4 = the category)')
    me = re.search(r'REX_HOOK_RAW\(sub_82C3B218\)\s*\{(.*?)\n\}', origin, re.S)
    if not me or 'OnWalkEnd()' not in me.group(1):
        bad.append('walkhook: REX_HOOK_RAW(sub_82C3B218) must call OnWalkEnd()')
    if re.search(r'REX_HOOK_RAW\(sub_82C2F960\)\s*\{[^}]*OnWalk', origin):
        bad.append('walkhook: the sub-walker sub_82C2F960 must not drive OnWalkBegin (its categories are sub-walk ids)')
    k1, k2 = tap.find('q::OnOp19(t_scope,'), tap.find('walkcat::Mark(t_scope.cur, t_walk)')
    k3 = tap.find('kQpSceneColour', k2) if k2 >= 0 else -1
    if not (0 <= k1 < k2 and k3 > k2):
        bad.append('walkhook: quads2d_tap.h must call walkcat::Mark(t_scope.cur, t_walk) after q::OnOp19(t_scope, (found offsets %d %d)' % (k1, k2))
    body = hooks[hooks.find('void AttachFrameHooks'):]
    pm, ps, pp = (body.find(x) for x in ('stage_hooks.push_back(h.maps_.get())', 'stage_hooks.push_back(h.scene2d_.get())', 'stage_hooks.push_back(h.particles_.get())'))
    if not (0 <= pm < ps < pp):
        bad.append('order: AttachFrameHooks must push maps_, then scene2d_, then particles_ (found offsets %d %d %d)' % (pm, ps, pp))
    fate = fate_src[fate_src.find('inline Fate FateOf'):]
    fate = fate[:fate.find('\n}\n')]
    if not re.search(r'kQpSceneColour\s*&&\s*scene_hook\s*&&\s*rdr_scene::Scene2DSite\(r\.site\)\)\s*return Fate::InScene', fate):
        bad.append('fate: FateOf has no "kQpSceneColour && scene_hook -> Fate::InScene" rule')
    rows = host[host.find('void SceneHost::DrawRows'):]
    rows = rows[:rows.find('\nvoid SceneHost::Handle')]
    if 'Fate::InScene' not in rows or 'scene2d_rep_' not in rows:
        bad.append('accounting: DrawRows does not count the in-scene rows from scene2d_rep_')
    start = host[host.find('bool SceneHost::Start'):]
    start = start[:start.find('\nstd::string SceneHost::')] if '\nstd::string SceneHost::' in start else start[:6000]
    if 'SetScene2D' not in start or 'scene_depth = true' not in start or 'opts_.scene2d' not in start:
        bad.append('wiring: SceneHost::Start must call draw_->SetScene2D (under opts_.scene2d) with ctx.scene_depth = true')
    if 'SetScene2D' in live:
        bad.append('wiring: the stream build (renderer_adapter/live.cpp) must not install the scene-2D hook (the stream draws those rows)')
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--plant', default='')
    a = ap.parse_args()
    bad = run(a.root, a.plant)
    if a.plant:
        if bad:
            print('PASS planted %s: caught (%s)' % (a.plant, bad[0]))
            return 0
        print('FAIL planted %s: not caught' % a.plant)
        return 1
    for b in bad:
        print('FAIL', b)
    print('%s: scs_lint (%d problems)' % ('FAIL' if bad else 'PASS', len(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
