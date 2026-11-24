#!/usr/bin/env python3
"""S103 lane L2d check (every frame kind needs an owner: the flat frame kind + the per-scene required-input masks, src/renderer_game/scene_kind.h).
Builds tools/scene/l2d_unit.cpp (scene_kind.h, the REAL scene_frame.h DecideNow, inputs_core.h RegisterAll / kCawDrop; 0 game runs, no game, no SDK), runs it, then its planted
twins (each must exit 1 and fail its NAMED check), then the wiring lints (the decision reads the kind's mask; RegisterAll registers the scene masks; the switch is dev only).
  py -I tools/scene/l2d_check.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {1: 'mask.match_unchanged', 2: 'flat.hole_falls_back', 3: 'flat.never_with_flag_off', 4: 'kind.lag', 5: 'flat.no_apply_unless_ours', 6: 'walk.sticky_stage_flat', 7: 'caw.classifier', 8: 'caw.leave', 9: 'caw.hysteresis', 10: 'a4.ours_no_walk', 11: 'a4.ours_no_walk'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's103', 'l2d')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'l2d_unit.exe')
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-DNOMINMAX', '-fuse-ld=lld']
    b = subprocess.run([CLANG] + flags + [os.path.join(root, 'tools', 'scene', 'l2d_unit.cpp'), '-o', exe], capture_output=True, text=True)
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
        p = subprocess.run([exe, '--plant', str(plant)], capture_output=True, text=True)
        failed = [ln for ln in p.stdout.splitlines() if ln.startswith('FAIL')]
        ok = p.returncode != 0 and any(ln.startswith('FAIL ' + name + ' ') for ln in failed)
        print('twin %d %-26s %s' % (plant, name, 'caught' if ok else 'NOT CAUGHT'))
        bad += 0 if ok else 1

    # S103 CW1 review fix 1: the OFF (stream-free) arm. cw1_off_unit.cpp is built with -DWWE2K14_KEEP_STREAM=0 (its static_assert refuses any other build): in OFF the Caw mask only changes the never_fed hold (host_core.h SceneFeed::Note).
    off_exe = os.path.join(out, 'cw1_off_unit.exe')
    ob = subprocess.run([CLANG, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-pthread', '-DWWE2K14_KEEP_STREAM=0', '-D_CRT_SECURE_NO_WARNINGS', '-DNOMINMAX', '-fuse-ld=lld',
                         os.path.join(root, 'tools', 'scene', 'cw1_off_unit.cpp'), '-o', off_exe], capture_output=True, text=True)
    if ob.returncode:
        print('FAIL: cw1_off_unit did not compile\n' + ob.stdout + ob.stderr)
        return 1
    orr = subprocess.run([off_exe], capture_output=True, text=True)
    print(orr.stdout.strip())
    if orr.returncode:
        print('FAIL: the OFF unit test failed')
        return 1
    op = subprocess.run([off_exe, '--plant', '1'], capture_output=True, text=True)
    ook = op.returncode != 0 and any(ln.startswith('FAIL off.caw_feed ') for ln in op.stdout.splitlines())
    print('twin off1 %-24s %s' % ('off.caw_feed', 'caught' if ook else 'NOT CAUGHT'))
    bad += 0 if ook else 1
    lint = subprocess.run([sys.executable, '-I', os.path.join(root, 'tools', 'scene', 'off_unit_lint.py'), '--root', root, '--selftest'], capture_output=True, text=True)
    print(lint.stdout.strip())
    bad += 1 if lint.returncode else 0

    def text(rel):
        with open(os.path.join(root, *rel.split('/')), encoding='utf-8') as f:
            return f.read()
    need = [
        ('src/renderer_game/scene_frame.h', 'scene_kind::RequiredFor(kind, s.required_inputs', 'DecideNow asks the inputs of the frame\'s kind'),
        ('src/renderer_game/scene_frame.h', 'scene_kind::NoteGame(kind', 'DecideNow feeds the per-kind census'),
        ('src/scene_live/inputs_core.h', 'RegisterSceneMasks();', 'RegisterAll registers the per-scene masks'),
        ('src/renderer_scene/live_scene_draw.cpp', 'NoteStage(rec, built, reports);', 'Warm / DrawLive note the stage facts of the record they built'),
        ('src/start_settings.cpp', 'a.flat = dev && REXCVAR_GET(wwe2k14_rdr_scene_flat)', 'the flat switch is dev only'),
        ('src/start_settings.cpp', 'scene_kind::g_flat.store(a.flat', 'the flat switch reaches the kind global (S103 SPB review L6)'),
        ('src/start_settings.cpp', 'scene_kind::g_caw_enabled.store(a.caw', 'the CAW drop reaches the kind global (S103 SPB review L6)'),
        ('src/renderer_adapter/live.cpp', 'scene_kind::CensusLine()', 'the [scene][kind] census line is logged'),
        ('src/renderer_scene/live_scene.cpp', 'row.pad = st.pad;', 'the record carries the stamp\'s scene kind'),
        ('src/renderer_adapter/live.cpp', 'scene_kind::StaleBits(kind,', 'the render side\'s freshness check asks the kind\'s mask (StaleBits = RequiredFor, unit-tested)'),
        ('src/renderer_adapter/live.cpp', 'scene_kind::Apply2DAllowed(flat_why)', 'a flat frame takes Apply2D only after an Ours verdict'),
        ('src/start_settings.cpp', 'a.caw = dev && REXCVAR_GET(wwe2k14_rdr_scene_caw)', 'the CAW drop is dev only'),
        ('src/renderer_game/scene_frame.h', 'st.pad = uint8_t(kind);', 'the decision stamps its kind'),
        ('src/renderer_game/scene_skip_core.h', 'rdr_own::g_board.OrPad(rdr_own::Pass::Scene3D, frame, scene_kind::kWalkBit);', 'S103 WB: the walk hooks set the walk bit at ask time'),
        ('src/renderer_scene/live_scene_draw.cpp', 'scene_kind::ApplyWalk(walked, f);', 'S103 WB: FlatFacts3D applies the walk bit (the shared pure step)'),
        ('src/renderer_adapter/live.cpp', 'kind = scene_kind::KindOfPad(s.pad);', 'S103 WB: the render side masks the walk bit off the kind'),
        ('src/renderer_host/scene_host.cpp', 'kind = scene_kind::KindOfPad(s.pad);', 'S103 WB: the host masks the walk bit off the kind'),
        ('src/step_rate.h', 'scene_skip::walk_log::StepLog(', 'S103 WB: the [scene][walk] line is logged once per presented frame'),
        ('src/step_rate.h', 'classifier.Step(frame, seq_id);', 'S103 CW1: the CAW classifier is wired into the per-frame step (an unwired classifier publishes nothing: the editor stays a match frame)'),
        ('src/step_rate.h', 'scene_kind::CawEnterLine(', 'S103 CW1: the first Caw frame prints the [scene][kind] caw line'),
        ('src/start_settings.cpp', 'a.caw = dev ? REXCVAR_GET(wwe2k14_rdr_scene_caw) : true;', 'S103 CW1: the stream-free build turns the Caw drop on (a +dev build can knock it out with =false)'),
        ('src/step_rate.h', 'scene_kind::CawLeaveLine(', 'S103 CW1: leaving Caw prints the [scene][kind] caw leave line'),
        ('src/renderer_host/scene_host.cpp', 'scene_kind::CensusLine()', 'S103 CW1: the OFF host prints the [scene][kind] census next to its [host] one'),
    ]
    # S103 WB lint: the walk-hook list is exactly Model, Crowd, RingColour. A hook added to it silently makes menus 3D again (ModelRecord runs in match-setup menus, Particles in every menu, Post...).
    import re
    m = re.search(r'inline constexpr bool IsWalkHook\(Hook h\) \{ return (.*?); \}', text('src/renderer_game/scene_skip_core.h'))
    names = sorted(re.findall(r'Hook::(\w+)', m.group(1))) if m else None
    if names != ['Crowd', 'Model', 'RingColour']:
        print('FAIL: lint: scene_skip_core.h IsWalkHook must list exactly Model, Crowd, RingColour (found %s): ModelRecord / Particles / Post / Plane ... must never set the walk bit' % names)
        return 1
    for rel, needle, what in need:
        if needle not in text(rel):
            print('FAIL: lint: %s (%s: %s missing)' % (what, rel, needle))
            bad += 1
    if text('src/renderer_scene/live_scene_draw.cpp').count('NoteStage(rec, built, reports);') != 2:
        print('FAIL: lint: NoteStage must follow BOTH live builds (Warm and DrawLive)')
        bad += 1
    print('l2d_check: %s' % ('PASS' if not bad else 'FAIL (%d)' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
