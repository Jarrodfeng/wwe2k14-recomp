#!/usr/bin/env python3
"""S103 lane M gate check: builds tools\\scene\\screen_planes_unit.cpp (the stage screens' live PlaneSource: the flush copy of the display frame, the titan channel -> display set chain, the frame store,
the latch rows, the shared YUV feed, the mode gate: 0 game runs, no game, no SDK), runs it, then its planted twins (each must exit 1 and fail its NAMED check), then the wiring lints.
  py -I tools\\scene\\screen_planes_check.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {
    'swap_channels': 'bind.key_to_movie',            # a screen bound to the WRONG MOVIE SLOT (the reversed / wrong-surface class)
    'wrong_buffer': 'copy.display_buffer', 'ignore_pitch': 'copy.pitch', 'swap_chroma': 'copy.chroma_order',
    'lazy_copy': 'copy.stable', 'size_ignored': 'bind.expected_size', 'stamp_always': 'screens.complete_gate', 'gate_800': 'screens.complete_gate', 'stage_ignored': 'copy.needs_stage', 'banner_to_ch5': 'bind.default_table', 'panels_ch2': 'bind.panels_channel1', 'ignore_state': 'bind.not_ready_not_copied', 'unbound_falls_back': 'bind.unbound_none', 'ignore_serial': 'serial.exact', 'mode_ignored': 'mode.off_no_work', 'idle_off': 'screens.idle_gate', 'idle_window0': 'screens.idle_gate', 'idle_ignores_channels': 'screens.idle_gate', 'idle_keeps_texture': 'screens.idle_drops_texture', 'stale_movie_kept': 'screens.no_row_black',   # S103 HN
      # S103 E7
    'stale_screen_pic': 'screens.pic_refresh',   # S103 SCN: the 805 picture never refreshes (run 38)
    'show_unread': 'show.gate_follows_bits', 'art_erased': 'show.loader_keeps_art', 'idle_black': 'show.idle_draw',   # S103 SCF (SCM.md): the game's show bits; the planted twins = the pre-SCF rules
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's103', 'm')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'screen_planes_unit.exe')
    srcs = [os.path.join(root, 'tools', 'scene', 'screen_planes_unit.cpp'), os.path.join(root, 'src', 'renderer_scene', 'record_io.cpp'), os.path.join(root, 'src', 'renderer_scene', 'live_scene.cpp')]
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-fuse-ld=lld']
    b = subprocess.run([CLANG] + flags + srcs + ['-o', exe], capture_output=True, text=True, cwd=out)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    if b.stderr.strip():
        print(b.stderr.strip())
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
        print('twin %-20s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', name))
        bad += 0 if ok else 1

    def text(rel):
        with open(os.path.join(root, rel), encoding='utf-8') as f:
            return f.read()
    need = [
        ('src/wwe2k14_app.h', 'scene_screens_glue::Register()', 'OnPostSetup installs the screens glue'),
        ('src/d3d_textures.h', 'scene_screens::OnDisplayFlush(base, set)', 'the existing sub_827FB540 hook hands the display set over'),
        ('src/scene_live/screen_planes_glue.h', 'SetScreenFeed(&Feed())', 'the render side gets the YUV feed'),
        ('src/scene_live/screen_planes_glue.h', 'SetScreens(&scr::ScreensFnImpl)', 'the stage-fx contributor gets the rows provider'),
        ('src/scene_live/screen_planes_hook.h', 'Mode::Off) return', 'the flush entry returns at mode 0'),
        ('src/renderer_scene/live_assets.cpp', 'stage_media::Cfg().all_keys = true', 'the live arena load takes every screen key 800-809 (the game exe never sets wrestler: it used to load only the match set)'),
        ('src/renderer_scene/rf_screens.cpp', 'stage_look::Entrance(ctx.t)', 'the applier picks the screen set on the front end own look signal'),
        ('src/renderer_scene/rf_c_core.cpp', 'Part::ScreensEntrance', 'the arena model has a screens family per look'),
        ('src/renderer_scene/rf_screens.cpp', 'SetIdleScreens(', 'S103 SCF (replaces E7 DropIdleScreens, which rebuilt the arena set when a movie came and went): an idle screen binds the live frame grab per frame, no rebuild'),
        ('src/renderer_scene/rf_c_core.cpp', 'ApplyShowMasks(ctx, *as)', 'S103 SCF: the game show bits (screen keys + art slots 301-304) are a per-frame model mask'),
        ('src/scene_live/stage_fx_contrib.h', 'ShowToRows(f->show', 'S103 SCF: the stage-fx writer publishes the show bits as kFxShow rows'),
        ('src/renderer_scene/rf_screens.cpp', 'kFxfScreenIdle', 'S103 E7: the applier skips an idle row'),
        ('src/scene_live/screen_planes_glue.h', 'SetScreenIdle(', 'S103 E7: the glue gives the stage-fx contributor the no-movie signal'),
        ('src/scene_live/stage_fx_contrib.h', 'stage_look::Entrance(t)', 'the screens gate uses the same look signal as the applier'),
        ('src/renderer_scene/live_assets.cpp', 'all_keys = saved_all_keys', 'N2: the sticky all_keys flag is restored after the live arena load'),
        ('src/renderer_scene/live_scene_draw.cpp', 'InvalidateLiveScreen(', 'S103 SCX (was N3): the slot-800 live screen is dropped only on a scene change (stageless record) or another arena; a drawn-frame gap keeps it'),
        ('src/renderer_scene/rf_screens.cpp', 'holes += 1', "N1: a look whose screen set is missing while the other look's exists is a hole, never open"),
        ('src/scene_live/stage_fx_contrib.h', 's800_required_tron', 'N4: the census counts 800 required because a tron is live'),
        ('src/renderer_scene/rf_screens.cpp', 'NoRowGoesBlack(', 'S103 HN: a shown screen with no sized row goes to the idle black, never keeps the last picture'),
    ]
    for rel, needle, what in need:
        if needle not in text(rel):
            print('FAIL: lint: %s (%s: %s missing)' % (what, rel, needle))
            bad += 1
    # this lane adds no hook of its own: the flush hook exists (d3d_textures.h); a function takes one hook (S101)
    for rel in ('src/scene_live/screen_planes_core.h', 'src/scene_live/screen_planes_contrib.h', 'src/scene_live/screen_planes_glue.h', 'src/scene_live/screen_planes_hook.h'):
        if 'REX_HOOK' in text(rel):
            print('FAIL: lint: %s adds a hook' % rel)
            bad += 1
    # the render thread never reads guest memory: the contrib / source file has no guest reader
    if 'ReadBase' in text('src/scene_live/screen_planes_contrib.h') or 'SafeCopy' in text('src/scene_live/screen_planes_contrib.h'):
        print('FAIL: lint: screen_planes_contrib.h reads guest memory (render-side code must use the store only)')
        bad += 1
    j = subprocess.run([sys.executable, '-I', os.path.join(root, 'tools', 'scene', 'screen_bind_join.py'), '--selftest'], capture_output=True, text=True)
    if j.returncode:
        print('FAIL: screen_bind_join selftest ' + j.stdout)
        bad += 1
    print('RESULT', 'OK' if bad == 0 else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
