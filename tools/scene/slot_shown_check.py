#!/usr/bin/env python3
"""S103 lane V1 (R17) gate check: builds tools\\scene\\slot_shown_unit.cpp (the game's per-slot 'shown' bit read at the latch: a hidden slot is emitted invisible and not asked for light; EmittedSkeletons
asks only slot skeletons; 0 game runs, no game, no SDK), runs it, then its planted twins (each must exit 1 and fail its NAMED check), then the wiring lints.
  py -I tools\\scene\\slot_shown_check.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {
    'ignore_bit': 'arena.hidden_not_emitted', 'no_reader': 'arena.hidden_not_emitted', 'stale_latch': 'arena.flip_picked_up', 'all_skeletons': 'wrestler.non_slot_not_asked',
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's103', 'v1')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'slot_shown_unit.exe')
    srcs = [os.path.join(root, 'tools', 'scene', 'slot_shown_unit.cpp'), os.path.join(root, 'src', 'renderer_scene', 'record_io.cpp'), os.path.join(root, 'src', 'renderer_scene', 'live_scene.cpp'),
            os.path.join(root, 'src', 'scene_live', 'arena_contrib.cpp'), os.path.join(root, 'src', 'scene_live', 'wrestler_contrib.cpp')]
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-fuse-ld=lld']
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
        ('src/scene_live/inputs_glue.h', 'SetSlotRecordReader', 'the glue wires the slot-record reader to the stage contributor'),
        ('src/scene_live/stage_fx_contrib.h', 'ReadSlotRecords', 'the stage contributor reads the records at stage + 1220'),
        ('src/scene_live/arena_contrib.cpp', 'SlotHidden(g_snap, sl.slot)', 'EmittedSlots drops a hidden slot'),
    ]
    for rel, needle, what in need:
        if needle not in text(rel):
            print('FAIL: lint: %s (%s: %s missing)' % (what, rel, needle))
            bad += 1
    print('RESULT', 'OK' if bad == 0 else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
