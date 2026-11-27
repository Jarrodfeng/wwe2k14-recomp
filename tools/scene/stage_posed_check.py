#!/usr/bin/env python3
"""S110 BARRICADE check: builds tools\\scene\\stage_posed_unit.cpp (the stage slots the game draws posed: every slot whose model has more than one node, read from the model's own palette;
0 game runs, no game, no SDK), runs it, then its planted twins (each must exit 1 and fail its NAMED check), then the wiring lints.
  py -I tools\\scene\\stage_posed_check.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {'node1_only': 'posed.five_nodes', 'ignore_296': 'posed.no_node_table_none', 'stride48': 'posed.five_nodes'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's110', 'barricade')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'stage_posed_unit.exe')
    srcs = [os.path.join(root, 'tools', 'scene', 'stage_posed_unit.cpp'), os.path.join(root, 'src', 'renderer_scene', 'record_io.cpp'), os.path.join(root, 'src', 'renderer_scene', 'live_scene.cpp')]
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
        ('src/scene_live/stage_fx_contrib.h', 'ReadPosedSlots(', 'the stage writer reads the posed slots at the latch'),
        ('src/scene_live/stage_fx_contrib.h', 'PosedToRows(posed', 'the stage writer emits the palette rows'),
        ('src/renderer_scene/rf_c_core.cpp', 'kFxSlotPalette', 'the applier moves a palette row\'s slot into its own model'),
        ('src/renderer_scene/rf_c_core.cpp', 'BindMesh(me)', 'a posed slot\'s model is bound to its palette nodes'),
        ('src/renderer_scene/rf_arena_live.cpp', 'PosedAdoptRows(', 'the arena draw adopts the posed slots of ITS arena\'s stage from the record'),
        ('src/scene_live/stage_fx_contrib.h', 'posed_dropped_', 'the stage writer counts the posed slots the row cap dropped (census)'),
        ('src/renderer_scene/rf_arena_live.cpp', 'kObjSkinned', 'a posed slot is drawn skinned'),
        ('src/renderer_scene/record_io.cpp', 'KeepKnownKinds(tmp, kFxScreen, kFxLast, out.c_errors)', 'the record reader keeps every stage-fx kind up to kFxLast (rows_stage_fx.h) and skips a newer one (S111 FXSKIP)'),
        ('src/assets/asset_load.h', 'OFFLINE VIEWER FALLBACK ONLY', 'the baked corner constants are labelled'),
    ]
    for rel, needle, what in need:
        if needle not in text(rel):
            print('FAIL: lint: %s (%s: %s missing)' % (what, rel, needle))
            bad += 1
    # no slot ids / act ids of the barricade in the new rule or the applier
    for rel in ('src/renderer_scene/stage_posed.h',):
        t = text(rel)
        code = '\n'.join(ln.split('//')[0] for ln in t.splitlines())
        for lit in ('390', '394', '2110', '2124'):
            if lit in code:
                print('FAIL: lint: %s names %s in code (the rule is "more than one node", no list)' % (rel, lit))
                bad += 1
    print('RESULT', 'OK' if bad == 0 else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
