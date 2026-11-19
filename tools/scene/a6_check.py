#!/usr/bin/env python3
"""S103 lane A6 gate check: builds tools\\scene\\a6_unit.cpp (bits 4 / 7 read from memory at the latch: the predictors, the two compares, the board, Table::Find / OnLatchEntry; 0 game runs, no game, no SDK),
runs it, then its planted twins (each must exit 1 and fail its NAMED check), then the wiring lints.
  py -I tools\\scene\\a6_check.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {
    'a6_embedded_2736': 'a6.wrestler_embedded', 'a6_skeleton_52': 'a6.wrestler_embedded', 'a6_gate_ignored': 'a6.wrestler_gated_embedded', 'a6_ring_swapped': 'a6.ring_indices',
    'a6_stale_counted': 'a6.compare_stale_not_counted', 'a6_state_prefix': 'a6.state_difference_named', 'a6_no_feed': 'a6.latch_entry_covers', 'a6_unchanged_counted': 'a6.state_unchanged_not_counted', 'a6_flag_ignored': 'a6.wrestler_manager_unread',
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's103', 'a6')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'a6_unit.exe')
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-fuse-ld=lld']
    b = subprocess.run([CLANG] + flags + [os.path.join(root, 'tools', 'scene', 'a6_unit.cpp'), '-o', exe], capture_output=True, text=True)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    if b.stderr.strip():
        print(b.stderr.strip())
    r = subprocess.run([exe], capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode:
        print('FAIL: the unit test failed')
        return 1
    bad = 0
    for plant, name in PLANTS.items():
        p = subprocess.run([exe, '--plant', plant], capture_output=True, text=True)
        failed = [ln for ln in p.stdout.splitlines() if ln.startswith('FAIL')]
        ok = p.returncode != 0 and any(name in ln for ln in failed)
        print('twin %-18s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', name))
        bad += 0 if ok else 1

    def text(rel):
        with open(os.path.join(root, rel), encoding='utf-8') as f:
            return f.read()
    need = [
        ('src/scene_live/inputs_contrib.h', 'lightlatch::PredictWrestlers', 'the latch builds the wrestler predictions'),
        ('src/scene_live/inputs_contrib.h', 'lightlatch::PredictRing', 'the latch builds the ring predictions'),
        ('src/scene_live/inputs_contrib.h', 'lightlatch::CompareIndex', 'the latch compares them with the walk-learned table'),
        ('src/scene_live/inputs_contrib.h', 'lightlatch::The().Census()', 'the census prints light_index / light_state latch==walk'),
        ('src/scene_live/light_state_contrib.h', 'lightlatch::The().OnWalkFrame', 'the lights contributor pairs the walk frame with the latch'),
        ('src/scene_live/inputs_glue.h', 'c.SetRing(', 'the glue gives the contributor the ring object'),
    ]
    for rel, needle, what in need:
        if needle not in text(rel):
            print('FAIL: lint: %s (%s: %s missing)' % (what, rel, needle))
            bad += 1
    # the switch that feeds predictions to the table stays OFF until the mirror run's census releases it (INPUTS_A6.md section 6); the stamp switches are not this lane's
    if 'inline constexpr bool kLatchEntries = false;' not in text('src/scene_live/light_latch.h'):
        print('FAIL: lint: kLatchEntries is not false in light_latch.h')
        bad += 1
    if 'kStampLightStates = true' not in text('src/scene_live/light_stamp_flag.h'):   # S103 L1C: flipped ON for landing 1 (hold at run time: --wwe2k14_rdr_scene_hold=4)
        print('FAIL: lint: kStampLightStates is not true')
        bad += 1
    # this lane reads memory only: no hook, no guest write
    for rel in ('src/scene_live/light_latch.h', 'src/scene_live/light_latch_board.h'):
        t = text(rel)
        if 'REX_' + 'HOOK' in t or 'StoreGuest' in t or 'WriteGuest' in t:
            print('FAIL: lint: %s adds a hook or a guest write' % rel)
            bad += 1
    if bad:
        print('FAIL: %d problem(s)' % bad)
        return 1
    print('OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
