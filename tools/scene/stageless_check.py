#!/usr/bin/env python3
"""S111 STAGELESS2 check: builds tools/scene/stageless_unit.cpp (the stageless-scene rules of src/renderer_scene/stageless_scene.h; 0 game runs, no SDK), runs it, then its planted
twins (each must exit 1 with its NAMED check among the failures), then the wiring lints (the shipped code calls the rules).
  py -I tools/scene/stageless_check.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {'1': 'stageless.belt_ours', '2': 'stageless.menu_not_ours', '3': 'stageless.required'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's111', 'stageless2')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'stageless_unit.exe')
    src = os.path.join(root, 'tools', 'scene', 'stageless_unit.cpp')
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-fuse-ld=lld']
    b = subprocess.run([CLANG] + flags + [src, '-o', exe], capture_output=True, text=True, cwd=out)
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
        print('twin %-3s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', name))
        bad += 0 if ok else 1

    def text(rel):
        with open(os.path.join(root, rel), encoding='utf-8') as f:
            return f.read()
    need = [
        ('src/scene_live/inputs_contrib.h', 'scene_inputs::ReadWord(read_, rdr_scene::kLoadedStage, holder) && holder == 0;', 'the latch reads the game stage holder'),
        ('src/scene_live/inputs_contrib.h', 'rdr_scene::stageless::NoStageHeldBits(f->no_stage_held)', 'the Emit writes header bit 6'),
        ('src/renderer_scene/live_scene_draw.cpp', 'stageless_ = stageless_gate_.Step(rec.header.flags, walked);', 'NoteStage steps the gate'),
        ('src/renderer_scene/live_scene_draw.cpp', 'rdr_scene::stageless::NoStageBlocks(HasStage(reports), stageless_)', 'BuildTerms asks the no-stage rule'),
        ('src/renderer_scene/live_scene_draw.h', 'return stage_ || stageless_;', 'a stageless scene is no scene boundary'),
        ('src/renderer_host/scene_host.cpp', 'rdr_scene::stageless::RequiredInScene(', 'the never_fed mask drops the stage-only inputs'),
        ('src/draw_origin.h', 'REX_HOOK_RAW(sub_8272BCD0)', 'the event CWeapon ctor hook'),
        ('src/scene_live/weapon_rows_core.h', 'events.ReadLightSets(read_word);', 'the tracker reads the asked light set once a frame'),
        ('src/scene_live/weapon_rows_contrib.h', 'PackAskedLightSet(ev.second.light_set)', 'the event row carries the asked set'),
        ('src/renderer_scene/rf_props.cpp', 'PropLights(ctx, w.object,', 'the drawer lights an event prop by its asked set'),
    ]
    for rel, needle, what in need:
        if needle not in text(rel):
            print('FAIL: lint: %s (%s: %s missing)' % (what, rel, needle))
            bad += 1
    print('RESULT', 'OK' if bad == 0 else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
