#!/usr/bin/env python3
"""S103 lane V1 check: builds tools\\arena\\stage_vis_test.cpp (the stage visibility's tree reader, slot join and volume: 0 game runs, no game, no SDK, no files), runs it, then every planted
fault (each must exit 1 and fail a NAMED check), then the wiring lints (the switch, the applier, the loader hook are where they must be).
  py -I tools\\arena\\stage_vis_check.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
# plant -> the check names, any one of which must be among its FAIL lines
PLANTS = {
    'no_widen': ('frustum.matches_oracle', 'frustum.widens_beyond_display'),   # the plain display frustum: the game draws what it rejects
    'term1': ('frustum.matches_oracle',),                                      # the angle term lost
    'slack1': ('frustum.matches_oracle', 'frustum.slack_margin'),              # no margin: the measured near miss would be dropped
    'no_flip': ('mirror.flip', 'frustum.matches_oracle'),                      # the mirrored copy tested as the base
    'collision_joined': ('fill.collision_refused',),                           # a name shared by two models joined without the geometry check
    'moving_boxed': ('fill.moving_skipped',),
    'placement_ignored': ('fill.placed_never_boxed',),                         # a corner-placed slot boxed at its stored position (review 1)                                  # an m(N) slot boxed with its local node box
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's103', 'V1')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'stage_vis_test.exe')
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-fuse-ld=lld']
    b = subprocess.run([CLANG] + flags + [os.path.join(root, 'tools', 'arena', 'stage_vis_test.cpp'), '-o', exe], capture_output=True, text=True)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    r = subprocess.run([exe], capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode:
        print('FAIL: the unit test failed')
        return 1
    bad = 0
    for plant, names in PLANTS.items():
        p = subprocess.run([exe, '--plant', plant], capture_output=True, text=True)
        failed = [ln for ln in p.stdout.splitlines() if ln.startswith('FAIL')]
        ok = p.returncode != 0 and any(n in ln for ln in failed for n in names)
        print('twin %-18s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', ' / '.join(names)))
        bad += 0 if ok else 1

    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
    from read_unit import read_path  # a split file is read with its parts expanded (tools/split/read_unit.py)

    def text(rel):
        return read_path(os.path.join(root, rel))
    need = [
        ('src/renderer_adapter/rdr_switches.h', 'X(StageVis, "wwe2k14_rdr_stage_vis", PerFrame, 0)', 'the switch is in the table (default off)'),
        ('src/renderer_adapter/reader.h', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_stage_vis, false', 'the cvar exists, default false'),
        ('src/renderer_scene/rf_arena.cpp', 'rdr_sw::kStageVis', 'the arena applier reads the switch'),
        ('src/renderer_scene/rf_arena.cpp', 'stagevis::StagePlanes(ctx.frame.camera.view.m, ctx.frame.camera.proj.m', 'the volume is built from OUR frame camera'),
        ('src/assets/asset_load.h', 'stage_tree::Fill(', 'the loader reads entry 50020'),
        ('src/renderer_scene/rf_arena.cpp', 'SetStageCull(model, cull)', 'the cull is a per-frame mask, not part of the hashed state (review 3)'),
    ]
    for rel, needle, what in need:
        if needle not in text(rel):
            print('FAIL: lint: %s (%s: %s missing)' % (what, rel, needle))
            bad += 1
    print('stage_vis_check: %s' % ('all passed' if not bad else '%d problem(s)' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
