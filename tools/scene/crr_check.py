#!/usr/bin/env python3
"""S103 lane CRR check: builds tools\\scene\\crr_unit.cpp (a created arena's ALT ring, ids 200-249: src\\renderer_scene\\ring_keys.h, asset_load.h load lists + PlaceRing, ring_low_keys.h, ring_rows_core.h kClassTable,
ring_recompute_core.h alt posts), runs it on the pure rules AND on the player's own pac\\car + pac\\bg\\bg04.pac files (0 game runs, no SDK), then its planted twins (each must exit 1 and fail its NAMED check), then the
wiring lints.
  py -I tools\\scene\\crr_check.py [--root DIR] [--game DIR] [--no-game]
Exit 0 = pass, 1 = fail. The real-file checks are skipped (loudly) with --no-game or when no game folder is found."""
import argparse
import os
import pathlib
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
# plant -> the check it must fail (the first two need no game files? no: skirt_swap and post_loop1_corner0 are pure; the other two read the real files)
PLANTS = {'skirt_swap': ('keys.alt_table', False), 'post_loop1_corner0': ('rows.alt_posts_angles', False), 'old_cover_corners': ('place.covers_by_corner', True), 'normal_cover_pose': ('rows.alt_cover_pose', False), 'logo_as_cover': ('shape.covers_share_geometry', True),
          'class_by_count': ('keys.class_posed', False)}   # S111 TBPAD review


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
    out = os.path.join(root, 'out', 's103', 'crr')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'crr_unit.exe')
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
    src = os.path.join(root, 'tools', 'scene', 'crr_unit.cpp')
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-Wno-unused-function', '-Wno-deprecated-declarations', '-Wno-unused-but-set-variable', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-fuse-ld=lld', '-I' + stb]
    b = subprocess.run([CLANG] + flags + [src, '-o', exe], capture_output=True, text=True, cwd=out)
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
        print('NOTE: no game folder: the real-file checks (bug.* / shape.* / place.*) did not run')
    bad = 0
    for plant, (name, needs_game) in PLANTS.items():
        if needs_game and not game:
            continue
        p = subprocess.run([exe, '--plant', plant] + (['--game', game] if game else []), capture_output=True, text=True, cwd=out)
        failed = [ln for ln in p.stdout.splitlines() if ln.startswith('FAIL')]
        ok = p.returncode != 0 and any(name in ln for ln in failed)
        print('twin %-20s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', name))
        bad += 0 if ok else 1

    def text(rel):
        with open(os.path.join(root, rel), encoding='utf-8') as f:
            return f.read()
    need = [
        ('src/assets/asset_load.h', 'kAltRingHighKeys', 'the created arena loads the alt ring keys'),
        ('src/assets/asset_load.h', 'ring.count(1050)', 'PlaceRing has the alt branch'),
        ('src/renderer_scene/rf_c_core.cpp', 'assets::IsAltRingKey(k)', 'the alt pieces are Part::Ring, not stage slots'),
        ('src/renderer_scene/rf_c_core.cpp', '1017, 1021, 1041, 1050', 'the alt keys get per-key ring pieces'),
        ('src/renderer_scene/rf_ring.cpp', 'ringkeys::IsAltArena(as->arena)', 'the ring path picks the alt keys by the arena id'),
        ('src/renderer_scene/rf_ring.cpp', 'ringkeys::ClassPosed(have[g], offered[g])', 'the ring path decides posed / at rest per class through ClassPosed (S111 TBPAD review, NEEDREMOVE)'),
        ('src/renderer_scene/ring_keys.h', 'return have > 0 && have == offered;', 'one posed rule for ours and the game\'s own rows: every given row drawable, whatever the count (S111 NEEDREMOVE)'),
        ('src/scene_live/ring_recompute_contrib.h', '&po.alt', 'the post read flags the alt ring (no bail)'),
        ('src/scene_live/ring_rows_hook.h', 'AltKeyOf(model)', 'the tracker stores the alt slot key'),
        ('src/assets/view/renderer.cpp', 'key >= 1017 && key <= 1083', 'the alt ring casts the props shadow'),
    ]
    for rel, needle, what in need:
        if needle not in text(rel):
            print('FAIL: lint: %s (%s: %s missing)' % (what, rel, needle))
            bad += 1
    if 'Need(' in text('src/renderer_scene/ring_keys.h') or '{12, 12, 12, 4, 3, 1}' in text('src/renderer_scene/ring_keys.h'):
        print('FAIL: lint: a fixed calls-per-group count is back in ring_keys.h (S111 NEEDREMOVE: the posed rule is ClassPosed alone)')
        bad += 1
    if 'cv_alt_ring); return false' in text('src/scene_live/ring_recompute_contrib.h').replace(' ', '').replace('++(cn.*alt_counter);return false', 'cv_alt_ring);return false'):
        print('FAIL: lint: the alt-ring bail is back in ReadPieceGates')
        bad += 1
    print('RESULT', 'OK' if bad == 0 else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
