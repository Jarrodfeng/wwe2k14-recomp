#!/usr/bin/env python3
"""S103 lane WSL (COV6 G5) check: builds tools\\scene\\slot_vis_unit.cpp (the game's per-slot look gate sub_824E7190 0x824E72A8-0x824E7500 and the applier's stage-generation reset; 0 game runs, no game,
no SDK), runs it, then its planted twins (each must exit 1 and fail its NAMED check), then the wiring lints.
  py -I tools\\scene\\slot_vis_check.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {
    'cycle_all_drawn': 'cycle.pick_at_ticks', 'cycle_pick_last': 'cycle.pick_at_ticks',
    'tail_polarity': 'tail.hide_list_needs_bit_set', 'tail_pairs_swapped': 'tail.pairs_240_247',
    'w_flag_lost': 'w.skipped_in_game_look', 'phase_off': 'w.skipped_in_game_look', 'e_as_match': 'e.drawn_only_at_phase4', 'gen_ignored': 'adopt.new_generation_clears', 'lose_vis': 'record.roundtrip', 'moving_known_only': 'moving.dropped_on_new_generation', 'arena_ignored': 'adopt.other_arena_ignored',
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's103', 'wsl')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'slot_vis_unit.exe')
    srcs = [os.path.join(root, 'tools', 'scene', 'slot_vis_unit.cpp'), os.path.join(root, 'src', 'renderer_scene', 'record_io.cpp'), os.path.join(root, 'src', 'renderer_scene', 'live_scene.cpp')]
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
        print('twin %-14s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', name))
        bad += 0 if ok else 1

    def text(rel):
        with open(os.path.join(root, rel), encoding='utf-8') as f:
            return f.read()
    need = [
        ('src/scene_live/stage_fx_contrib.h', 'BuildVisSkips(', 'the stage writer builds the skip list at the latch'),
        ('src/scene_live/stage_fx_contrib.h', 'CycleSkips(cyc, cw, recs', 'S103 CAR: the writer merges the cycle groups non-picked slots into the skip list'),
        ('src/scene_live/stage_fx_contrib.h', 'VisToRows(f->vis, t.stage_fx)', 'the writer emits the marker and skip rows'),
        ('src/scene_live/stage_fx_contrib.h', 'stage_gen_.fetch_add', 'the writer bumps the stage generation on a stage change / free'),
        ('src/renderer_scene/rf_c_core.cpp', 'VisCull(art, st.vis', 'the applier masks the skipped slots per frame (or the old look rule while unread)'),
        ('src/renderer_scene/rf_c_core.cpp', 'VisAdoptRows(ctx.t.stage_fx, as.arena', 'the applier adopts only its own arena\'s marker'),
        ('src/renderer_scene/live_assets.cpp', 'KeepLookSlots() = true', 'the live arena load keeps the e and g slots (masked per frame)'),
        ('src/assets/asset_load.h', 'KeepLookSlots()', 'the loader skips its e / g delete when asked'),
        ('src/renderer_adapter/reader.h', 'wwe2k14_rdr_slot_look', 'the switch exists'),
        ('src/renderer_scene/record_io.cpp', 'kFxSlotPalette', 'the record reader accepts the new row kinds (up to kFxSlotPalette, S110 BARRICADE; kFxCar, S103 CAR, before it)'),
    ]
    for rel, needle, what in need:
        if needle not in text(rel):
            print('FAIL: lint: %s (%s: %s missing)' % (what, rel, needle))
            bad += 1
    print('RESULT', 'OK' if bad == 0 else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
