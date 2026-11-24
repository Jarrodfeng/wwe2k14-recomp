#!/usr/bin/env python3
"""S103 lane C3 check: builds tools\\scene\\flash_fx_unit.cpp (the crowd flashes' live writer from a fake guest: the item array, the liveness rule, position / scale / colour, the contributor's mode
gate, the record round trip: 0 game runs, no game, no SDK), runs it, then its planted twins (each must exit 1 and fail its NAMED check), then the wiring lints.
  py -I tools\\scene\\flash_fx_check.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {
    'swapped_yz': 'flash.position', 'array_early': 'flash.array', 'all_pending': 'flash.live_rule', 'scale_off': 'flash.scale', 'color_off': 'flash.color', 'not_drawn': 'flash.live_rule', 'test_drawn': 'flash.lifecycle',
    'mode_ignored': 'mode.off_reads_nothing', 'lose_flags': 'record.roundtrip',
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's103', 'c3')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'flash_fx_unit.exe')
    srcs = [os.path.join(root, 'tools', 'scene', 'flash_fx_unit.cpp'), os.path.join(root, 'src', 'renderer_scene', 'record_io.cpp'), os.path.join(root, 'src', 'renderer_scene', 'live_scene.cpp')]
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
        ('src/scene_live/stage_fx_glue.h', 'RegisterFlashFx()', 'the glue registers the flash contributor (OnPostSetup already calls scene_stagefx_glue::Register)'),
        ('src/scene_live/stage_fx_glue.h', 'FlashFx().SetReader(&wwe2k14::scene_inputs_glue::ReadChecked)', 'the flash writer reads guest memory through the page-checked reader'),
        ('src/wwe2k14_app.h', 'scene_stagefx_glue::Register()', 'OnPostSetup registers the stage-fx glue'),
        ('src/renderer_scene/live_flashes.cpp', 'kFxfGameScale', 'the applier honours the game-scale flag'),
    ]
    for rel, needle, what in need:
        if needle not in text(rel):
            print('FAIL: lint: %s (%s: %s missing)' % (what, rel, needle))
            bad += 1
    # this lane adds no hook (the reads run at the latch: a function takes one hook, S101)
    for rel in ('src/scene_live/flash_fx_core.h', 'src/scene_live/flash_fx_contrib.h'):
        if 'REX_HOOK' in text(rel):
            print('FAIL: lint: %s adds a hook' % rel)
            bad += 1
    print('RESULT', 'OK' if bad == 0 else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
