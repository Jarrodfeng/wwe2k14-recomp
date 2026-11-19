#!/usr/bin/env python3
r"""Lane SCS lint (Session 103 overnight; docs\research\s103\RFF.md part B; 0 game runs): the in-scene 2D rows are drawn, in the game's order, and never dropped again.

  py -I tools\scene\scs_lint.py [--root DIR] [--plant NAME]

Checks (text, on the committed sources; each a FAIL when missed):
  order      frame_hooks.cpp AttachFrameHooks pushes the maps hook, then the scene-2D hook, then the particles hook (the game's category 36 draws before the particle node's 47)
  fate       scene_host.cpp FateOf returns Fate::InScene for a kQpSceneColour row when the scene hook is on (the old DropPass for it = every entrance frame refused pass_scene_colour)
  accounting DrawRows counts the in-scene rows from the hook's report (scene2d_rep_) and does not treat them as dropped passes while the hook is on
  wiring     SceneHost::Start installs the hook (SetScene2D) with Context::scene_depth = true; the stream build (renderer_adapter\live.cpp) never calls SetScene2D
Planted twins (the lint must FAIL on each; the unit's own twin is scs_unit --plant scene_z_ignored):
  fate_drop      FateOf drops scene-colour rows again (the pre-SCS rule)
  order_swapped  the particles hook is pushed before the scene-2D hook
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
    live = rd(root, 'src/renderer_adapter/live.cpp')
    if plant == 'fate_drop':
        host = re.sub(r'if \(r\.pass == rdr_scene::kQpSceneColour && scene_hook[^\n]*return Fate::InScene;', '', host)
    if plant == 'order_swapped':
        a, b = 'frame.stage_hooks.push_back(h.scene2d_.get());', 'frame.stage_hooks.push_back(h.particles_.get());'
        hooks = hooks.replace(a, '@@A@@').replace(b, a).replace('@@A@@', b)
    bad = []
    body = hooks[hooks.find('void AttachFrameHooks'):]
    pm, ps, pp = (body.find(x) for x in ('stage_hooks.push_back(h.maps_.get())', 'stage_hooks.push_back(h.scene2d_.get())', 'stage_hooks.push_back(h.particles_.get())'))
    if not (0 <= pm < ps < pp):
        bad.append('order: AttachFrameHooks must push maps_, then scene2d_, then particles_ (found offsets %d %d %d)' % (pm, ps, pp))
    fate = host[host.find('static Fate FateOf'):]
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
