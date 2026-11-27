#!/usr/bin/env python3
"""S108 lane CAP check: builds tools\\scene\\car_paint_unit.cpp (a created arena's textures as the GAME painted them: src\\scene_live\\car_paint.h, the stage's own texture list
[stage + 14452] read from forged guest memory, re-read on the unlock flush, bound by name), runs it on synthetic pictures AND on the player's own pac\\car files (0 game runs, no SDK),
then its planted twins (each must exit 1 and fail its NAMED check), then the wiring lints (the latch call, the glue's reader, the applier's bind / identity / reports).
  py -I tools\\scene\\car_paint_check.py [--root DIR] [--game DIR] [--no-game]
Exit 0 = pass, 1 = fail. The real-file checks are skipped (loudly) with --no-game or when no game folder is found."""
import argparse
import os
import pathlib
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {'file_kept': 'bind.guest_pictures', 'no_reread': 'flush.reread', 'silent_fail': 'report.unreadable_named', 'budget_ignored': 'budget.first_take_bounded',
          'last_dup_wins': 'bind.dup_first_wins', 'gen_ignored': 'stage.same_address_new_stage', 'not2d_reported': 'report.not2d_quiet',
          'late_paint_ignored': 'built.rebuilt_with_painted', 'never_ready_unread': 'list.null_ready_file_pictures', 'tail_corner': 'tail.mip0_read',   # S108 CAP2
          'header_unseen': 'key.same_handle_new_header'}   # S111 KEYFRESH
# S108 CAP2: the SDK's own texture utilities, compiled into the unit as the INDEPENDENT side of the tail-read proof (tools\scene\sdk_tex_layout.cpp; never our reader's arithmetic)
SDK_SOURCES = [('src', 'graphics', 'pipeline', 'texture', 'util.cpp'), ('src', 'graphics', 'pipeline', 'texture', 'info_formats.cpp'), ('src', 'core', 'math_msvc.cpp')]


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
    out = os.path.join(root, 'out', 's108', 'cap')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'car_paint_unit.exe')
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
    src = os.path.join(root, 'tools', 'scene', 'car_paint_unit.cpp')
    sdk = os.path.dirname(os.path.dirname(stb))   # the SDK checkout (thirdparty/rexglue-sdk-src)
    objs = []
    for parts in [None] + SDK_SOURCES:
        cpp = os.path.join(root, 'tools', 'scene', 'sdk_tex_layout.cpp') if parts is None else os.path.join(sdk, *parts)
        obj = os.path.join(out, 'sdk_' + os.path.splitext(os.path.basename(cpp))[0] + '.o')
        c = subprocess.run([CLANG, '-std=c++23', '-O2', '-w', '-D_CRT_SECURE_NO_WARNINGS', '-I' + os.path.join(sdk, 'include'), '-c', cpp, '-o', obj], capture_output=True, text=True, cwd=out)
        if c.returncode:
            print('FAIL: the SDK layout did not compile (%s): %s%s' % (cpp, c.stdout, c.stderr[-3000:]))
            return 1
        objs.append(obj)
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-Wno-unused-function', '-Wno-deprecated-declarations', '-Wno-unused-but-set-variable', '-g', '-O2',
             '-D_CRT_SECURE_NO_WARNINGS', '-fuse-ld=lld', '-I' + stb]
    b = subprocess.run([CLANG] + flags + [src] + objs + ['-o', exe], capture_output=True, text=True, cwd=out)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    warn = [ln for ln in b.stderr.splitlines() if 'car_paint' in ln and 'warning' in ln]
    for ln in warn:
        print('WARN ' + ln)
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
        p = subprocess.run([exe, '--plant', plant], capture_output=True, text=True, cwd=out)
        failed = [ln for ln in p.stdout.splitlines() if ln.startswith('FAIL')]
        ok = p.returncode != 0 and any(name in ln for ln in failed)
        print('twin %-15s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', name))
        bad += 0 if ok else 1

    def text(rel):
        with open(os.path.join(root, rel), encoding='utf-8') as f:
            return f.read()
    need = [
        ('src/scene_live/stage_fx_contrib.h', 'carpaint::OnLatch(f.vis.stage, read_, f.vis.gen)', 'the latch reads the stage\'s painted textures, keyed by the stage generation'),
        ('src/scene_live/stage_fx_glue.h', 'carpaint::SetPhys(', 'the glue installs the shared physical reader'),
        ('src/scene_live/stage_fx_glue.h', 'CrowdGuestMem{}.phys(', 'the physical reader is the shared page-checked one'),
        ('src/renderer_scene/rf_c_core.cpp', 'carpaint::Apply(full, *car_paint, ar)', 'the applier binds the game\'s pictures into the created arena model'),
        ('src/renderer_scene/rf_c_core.cpp', 'h = Fnv(h, car_paint->hash)', 'the paint is part of the set identity (a repaint rebuilds)'),
        ('src/renderer_scene/rf_c_core.cpp', 'rdr::DrawShortfall("arena_paint", ms.name.c_str()', 'every texture not taken is reported'),
        ('src/renderer_scene/rf_c_core.cpp', '!car_paint->ready', "the live set waits for a READY snapshot (every entry of the list tried once; no wait on the game)"),
        ('src/scene_live/car_paint.h', 'ReadGameState(gm)', "the latch reads the manager's words fresh at every take (census evidence only)"),
        ('src/d3d_textures.h', 'guest_tex_dirty::NoteFlush(ctx.r3.u32)', 'the unlock flush marks a watched header (both builds)'),
    ]
    for rel, needle, what in need:
        if needle not in text(rel):
            print('FAIL: lint: %s (%s: %s missing)' % (what, rel, needle))
            bad += 1
    print('RESULT', 'OK' if bad == 0 else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
