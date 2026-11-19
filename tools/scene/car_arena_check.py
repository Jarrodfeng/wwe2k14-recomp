#!/usr/bin/env python3
"""S103 lane CAR (BCA G1 piece 1) gate check: builds tools\\scene\\car_arena_unit.cpp (a Create-an-Arena venue from the game's own part pacs: src\\assets\\car_arena.h, asset_load.h LoadCustomArena / OpenCached;
the kFxCar record row; the selection registry), runs it on synthetic parts AND on the player's own pac\\car files (0 game runs, no SDK), then its planted twins (each must exit 1 and fail its NAMED check),
then the wiring lints.
  py -I tools\\scene\\car_arena_check.py [--root DIR] [--game DIR] [--no-game]
Exit 0 = pass, 1 = fail. The real-file checks are skipped (loudly) with --no-game or when no game folder is found."""
import argparse
import os
import pathlib
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
# plant -> the check it must fail (the first four need no game files; old_bg_path needs them)
PLANTS = {'missing_part': 'merge.bands_all_present', 'key_ignored': 'sel.key_distinguishes', 'light_casd': 'merge.carl_lights_win', 'old_bg_path': 'real.loads_created_arena'}


def find_stb(root):
    p = pathlib.Path(root)
    for base in (p, *p.parents):
        cand = base / 'thirdparty' / 'rexglue-sdk-src' / 'thirdparty' / 'stb'
        if (cand / 'stb_image.h').exists():
            return str(cand)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--game', default=None)
    ap.add_argument('--no-game', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's103', 'car')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'car_arena_unit.exe')
    stb = find_stb(root)
    if not stb:
        print('FAIL: stb_image.h not found (thirdparty\\rexglue-sdk-src\\thirdparty\\stb, here or in a parent checkout)')
        return 1
    game = a.game
    if not game and not a.no_game:
        for base in (pathlib.Path(root), *pathlib.Path(root).parents):
            if (base / 'game_data' / 'extracted' / 'pac' / 'car').is_dir():
                game = str(base / 'game_data' / 'extracted')
                break
    src = os.path.join(root, 'tools', 'scene', 'car_arena_unit.cpp')
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-Wno-unused-function', '-Wno-deprecated-declarations', '-Wno-unused-but-set-variable', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-fuse-ld=lld', '-I' + stb]
    extra = [os.path.join(root, 'src', 'scene_live', 'arena_contrib.cpp'), os.path.join(root, 'src', 'renderer_scene', 'record_io.cpp'), os.path.join(root, 'src', 'renderer_scene', 'live_scene.cpp')]
    b = subprocess.run([CLANG] + flags + [src] + extra + ['-o', exe], capture_output=True, text=True, cwd=out)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    args = [exe] + (['--game', game] if game else [])
    r = subprocess.run(args, capture_output=True, text=True, cwd=out)
    print(r.stdout.strip())
    if r.returncode:
        print('FAIL: the unit test failed\n' + r.stderr)
        return 1
    if not game:
        print('NOTE: no game folder: the real-file checks (real.*) did not run')
    bad = 0
    for plant, name in PLANTS.items():
        if plant == 'old_bg_path' and not game:
            continue
        p = subprocess.run([exe, '--plant', plant] + (['--game', game] if game else []), capture_output=True, text=True, cwd=out)
        failed = [ln for ln in p.stdout.splitlines() if ln.startswith('FAIL')]
        ok = p.returncode != 0 and any(name in ln for ln in failed)
        print('twin %-14s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', name))
        bad += 0 if ok else 1

    def text(rel):
        with open(os.path.join(root, rel), encoding='utf-8') as f:
            return f.read()
    need = [
        ('src/assets/asset_load.h', 'LoadCustomArena(', 'the created-arena loader exists (a new function; LoadArena is not restructured)'),
        ('src/assets/asset_load.h', 'car::CustomIdOfRel(rel, car_id)', 'OpenCached serves the merged part pac for ids 200-249 (every arena reader sees the venue)'),
        ('src/renderer_scene/live_assets.cpp', 'assets::LoadCustomArena(', 'the live arena load branches for ids 200-249'),
        ('src/renderer_scene/rf_c_core.cpp', 'CarFromRows(ctx.t.stage_fx', 'the applier reads the part indices row'),
        ('src/renderer_scene/rf_c_core.cpp', 'car_sel.Key()', 'the part selection is part of the set identity / the pristine key'),
        ('src/scene_live/stage_fx_contrib.h', 'ReadCarSel(', 'the latch reads the part indices with the stage'),
        ('src/scene_live/stage_fx_contrib.h', 'CarToRows(', 'the writer emits the kFxCar row'),
        ('src/scene_live/arena_hooks.h', 'REX_HOOK_RAW(sub_8342A8D8)', 'the light index pre-hook'),
        ('src/renderer_adapter/rdr_switches.h', 'wwe2k14_rdr_custom_arena', 'the switch exists'),
        ('src/renderer_adapter/reader.h', 'wwe2k14_rdr_custom_arena', 'the cvar exists'),
        ('src/renderer_scene/stage_slot_vis.h', 'CustomTailSkipped(', 'the 200..249 slot-hide tail is ported'),
        ('src/assets/arena_lm.h', 'RemapId(int id, int bg', 'the light-map record uses the real bg id'),
    ]
    for rel, needle, what in need:
        if needle not in text(rel):
            print('FAIL: lint: %s (%s: %s missing)' % (what, rel, needle))
            bad += 1
    print('RESULT', 'OK' if bad == 0 else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
