#!/usr/bin/env python3
"""S103 lane S2 gate check: builds tools/scene/s2_unit.cpp (bit 11's per-frame verdict, the sentinel walked frames on the real decision, the changes census per input: 0 game runs, no game,
no SDK), runs it, then its planted twins (each must exit 1 and fail its NAMED check), then the wiring lints (the switches exist and default off, the rules are called where they must be).
Bit 10's presence rule is proved by tools/scene/crowd_snapshot_check.py (crowd_snapshot_s2_part.h, the shipped contributor).
  py -I tools/scene/s2_check.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {
    'part_blend_ignored': 'part.unproven_blend_no_stamp',
    'part_texture_ignored': 'part.unnamed_texture_no_stamp',
    'part_empty_drawn_ignored': 'part.empty_but_drawn_counted',
    'sentinel_never': 'sentinel.walks',
    'changes_frame_counted': 'changes.frame_field_ignored',
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's103', 's2')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 's2_unit.exe')
    srcs = [os.path.join(root, 'tools', 'scene', 's2_unit.cpp'), os.path.join(root, 'src', 'renderer_scene', 'record_io.cpp')]
    flags = ['-std=c++20', '-DRDR_S2_PLANTS=1', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-fuse-ld=lld']
    b = subprocess.run([CLANG] + flags + srcs + ['-o', exe], capture_output=True, text=True)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
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
        print('twin %-22s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', name))
        bad += 0 if ok else 1

    def text(rel):
        with open(os.path.join(root, *rel.split('/')), encoding='utf-8') as f:
            return f.read()
    need = [
        ('src/scene_live/particle_source.h', 'presence::StampParticles(kStampParticles, pw) && WholePass(ps)', 'bit 11 is stamped per frame through the presence rule'),
        ('src/scene_live/particle_source.h', 'inline constexpr bool kStampParticles = false;', 'the class constant is unchanged (off)'),
        ('src/scene_live/crowd_contrib.cpp', 'ApplyPresence(w, presence_)', 'bit 10 judges presence under the switch'),
        ('src/scene_live/crowd_contrib.cpp', 'if (!kCrowdOursWhole && !by_presence)', 'bit 10 stamps by presence only with the switch'),
        ('src/scene_live/crowd_snapshot_core.h', 'inline constexpr bool kCrowdOursWhole = false;', 'the class constant is unchanged (off)'),
        ('src/scene_live/presence_rules.h', 'inline std::atomic<bool> g_particles{false};', 'the particle switch defaults off'),
        ('src/scene_live/presence_rules.h', 'inline std::atomic<bool> g_crowd{false};', 'the crowd switch defaults off'),
        ('src/scene_live/input_changes.h', 'inline std::atomic<bool> g_on{false};', 'the changes census defaults off'),
        ('src/renderer_game/scene_frame.h', 'inline std::atomic<uint32_t> g_sentinel_every{0};', 'the sentinel defaults off'),
        ('src/renderer_game/scene_frame.h', 'if (CurrentMode() == Mode::Ours) PlanSentinel(frame);', 'the decision plans the sentinel (mode 2 only)'),
        ('src/renderer_scene/live_scene.cpp', 'inputchanges::The().Note(*tables);', 'every built record feeds the changes census'),
        ('src/renderer_adapter/live.cpp', 'REXCVAR_DEFINE_INT32(wwe2k14_rdr_scene_sentinel, 0,', 'the sentinel cvar defaults 0'),
        ('src/renderer_adapter/live.cpp', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_scene_changes, false,', 'the changes cvar defaults off'),
        ('src/renderer_adapter/live.cpp', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_scene_particles_by_presence, true,', 'the particle presence cvar defaults ON (S103 L1C; --wwe2k14_rdr_scene_hold=11 turns it off)'),
        ('src/renderer_adapter/live.cpp', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_scene_crowd_by_presence, true,', 'the crowd presence cvar defaults ON (S103 L1C; --wwe2k14_rdr_scene_hold=10 turns it off)'),
        ('src/start_settings.cpp', 'dev ? REXCVAR_GET(wwe2k14_rdr_scene_sentinel) : 0', 'the sentinel is dev only'),
        ('src/start_settings.cpp', 'scene_frame::g_sentinel_every.store(a.sentinel_every', 'the sentinel reaches the frame global (S103 SPB review L6)'),
        ('src/renderer_adapter/live.cpp', 'scene_frame::SentinelSheet(scene_frame::HookFrameOf(guest_frame), every)', 'the sentinel frames write mirror pairs'),
    ]
    for rel, needle, what in need:
        if needle not in text(rel):
            print('FAIL: lint: %s (%s: %s missing)' % (what, rel, needle))
            bad += 1
    for rel in ('src/scene_live/presence_rules.h', 'src/scene_live/input_changes.h'):
        t = text(rel)
        if 'REX_' + 'HOOK' in t or re.search(r'#include\s+[<"](rex|xenia)', t):
            print('FAIL: lint: %s adds a hook or a game / SDK include (pure headers)' % rel)
            bad += 1
    if bad:
        print('FAIL: %d problem(s)' % bad)
        return 1
    print('PASS: unit, %d planted twins caught, lints' % len(PLANTS))
    return 0


if __name__ == '__main__':
    sys.exit(main())
