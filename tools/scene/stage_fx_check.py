#!/usr/bin/env python3
"""S103 lane C2 gate check: builds tools\\scene\\stage_fx_unit.cpp (the stage's live writers: moving slot worlds from a fake guest, the screen rows, the YUV -> RGBA feed conversion, the record
round trip: 0 game runs, no game, no SDK), runs it, then its planted twins (each must exit 1 and fail its NAMED check), then the wiring lints.
  py -I tools\\scene\\stage_fx_check.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {
    'transposed_guest': 'slot.rows_values', 'node_off': 'slot.node', 'inv_swapped': 'slot.skin_rule', 'stage_off': 'slot.model_join', 'motion_zero': 'slot.motion_gate',
    'join_shift': 'join.arena_worlds', 'feed_no_cache': 'feed.shared',
    'mode_ignored': 'mode.off_reads_nothing', 'swap_uv': 'yuv.values', 'chroma_swapped': 'yuv.chroma_halved', 'lose_section': 'record.roundtrip', 'arena_g64_only': 'arena.id_from_stage', 'stage_free_ignored': 'stage.freed_by_game', 'slots_ready_published_only': 'arena.slots_ready_of_stage', 'phase_is_entrance': 'phase.entrance_from_page',
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's103', 'c2')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'stage_fx_unit.exe')
    srcs = [os.path.join(root, 'tools', 'scene', 'stage_fx_unit.cpp'), os.path.join(root, 'src', 'renderer_scene', 'record_io.cpp'), os.path.join(root, 'src', 'renderer_scene', 'live_scene.cpp')]
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-fuse-ld=lld']
    b = subprocess.run([CLANG] + flags + srcs + ['-o', exe], capture_output=True, text=True, cwd=out)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    r = subprocess.run([exe], capture_output=True, text=True, cwd=out)
    print(r.stdout.strip())
    if r.returncode:
        print('FAIL: the unit test failed')
        return 1
    bad = 0
    for plant, name in PLANTS.items():
        p = subprocess.run([exe, '--plant', plant], capture_output=True, text=True, cwd=out)
        failed = [ln for ln in p.stdout.splitlines() if ln.startswith('FAIL')]
        ok = p.returncode != 0 and any(name in ln for ln in failed)
        print('twin %-18s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', name))
        bad += 0 if ok else 1

    def text(rel):
        with open(os.path.join(root, rel), encoding='utf-8') as f:
            return f.read()
    need = [
        ('src/wwe2k14_app.h', 'scene_stagefx_glue::Register()', 'OnPostSetup registers the stage-fx contributor'),
        ('src/scene_live/arena_world_glue.h', 'stagefx::StageFx().SetStage(self)', 'the MotionSetup post-hook hands over the stage object'),
        ('src/scene_live/arena_hooks.h', 'REX_HOOK_RAW(sub_824E9858)', 'S103 CAE F1: the game frees its stage in a destructor pre-hook (stage_fx drops it at once)'),
        ('src/scene_live/arena_world_glue.h', 'StageFx().OnStageFreed(self)', 'S103 CAE F1: the free hook reaches the stage_fx contributor'),
        ('src/scene_live/stage_fx_glue.h', 'ra::SlotsReady(', 'S103 CAE F2: slots_ready is keyed on the arena of the stage'),
        ('src/step_rate.h', '== 4 && seq_id == 9', 'S103 PHS: an entrance is the phase word 4 inside sequence 9'),
        ('src/scene_live/inputs_contrib.h', 'PageEntranceState(', 'S103 PHS: the pools input entrance is the stage-page object word'),
        ('src/scene_live/stage_fx_glue.h', 'ReadChecked', 'the glue reads guest memory through the page-checked reader'),
        ('src/scene_live/arena_contrib.cpp', 'MovingSlots', 'the arena contributor publishes its moving slots'),
        ('src/renderer_scene/record_io.cpp', 'Sec(kScStageFx, t.stage_fx)', 'the stage-fx section is written'),
    ]
    for rel, needle, what in need:
        if needle not in text(rel):
            print('FAIL: lint: %s (%s: %s missing)' % (what, rel, needle))
            bad += 1
    # this lane adds no hook (the readers run at the latch: a function takes one hook, S101)
    for rel in ('src/scene_live/stage_fx_core.h', 'src/scene_live/stage_fx_contrib.h', 'src/scene_live/stage_fx_glue.h'):
        if 'REX_HOOK' in text(rel):
            print('FAIL: lint: %s adds a hook' % rel)
            bad += 1
    lr = subprocess.run([sys.executable, '-I', os.path.join(root, 'tools', 'scene', 'cae_literals_lint.py'), '--root', root], capture_output=True, text=True)   # S103 CAE F5
    print(lr.stdout.strip())
    bad += 1 if lr.returncode else 0
    print('RESULT', 'OK' if bad == 0 else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
