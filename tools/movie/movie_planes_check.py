#!/usr/bin/env python3
"""S103 lane P4 gate check: builds tools\\movie\\movie_planes_unit.cpp (the movie planes taken from guest memory at the plane flush: the flush copy, the draw-order choice with U3's census, the upload; the rig on the
2D corpus captures: planes from a fake guest built out of the capture's plane bytes vs the stream images, max diff 0; 0 game runs, no game, no SDK), runs it, then its planted twins (each must exit 1 and fail its NAMED
check), then the wiring lints and a syntax check of movie_ours.cpp.
  py -I tools\\movie\\movie_planes_check.py [--root DIR] [--captures DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
FLAGS = ['-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-D_CRT_SECURE_NO_WARNINGS']
CAPTURES = ['c2d_entrance_tron', 'c2d_menu']   # an entrance tron (1280x280 Y, 640x144 chroma: padded rows) and the menu backdrop (512x256)
PLANTS = {
    'stale_plane': 'planes.fresh', 'future_frame': 'order.flush_before_draw', 'any_size': 'select.size', 'no_set_hint': 'select.set', 'swap_chroma': 'planes.chroma_order',
    'wrong_buffer': 'planes.display_buffer', 'half_chroma': 'planes.chroma_size', 'upload_always': 'upload.shared', 'switch_on': 'switch.default_off', 'rig_stale': 'rig.planes_equal',
    'no_ambiguity_guard': 'select.ambiguous_refused', 'no_age_limit': 'order.too_old', 'leak_stream': 'guest.no_stream_image', 'unbounded': 'upload.bounded', 'alpha_required': 'upload.no_alpha_served', 'kind_guard_off': 'upload.kind_guard',
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--captures', default=None, help='folder holding the c2d_* captures (default: <root>/out/captures)')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    caps = os.path.abspath(a.captures) if a.captures else os.path.join(root, 'out', 'captures')
    if not a.captures and not os.path.isdir(caps):   # a worktree: out\ lives in the MAIN checkout (git's common dir parent)
        g = subprocess.run(['git', '-C', root, 'rev-parse', '--git-common-dir'], capture_output=True, text=True)
        if g.returncode == 0:
            alt = os.path.join(os.path.dirname(os.path.abspath(os.path.join(root, g.stdout.strip()))), 'out', 'captures')
            if os.path.isdir(alt):
                caps = alt
    out = os.path.join(root, 'out', 's103', 'p4')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'movie_planes_unit.exe')
    src = os.path.join(root, 'tools', 'movie', 'movie_planes_unit.cpp')
    b = subprocess.run([CLANG] + FLAGS + ['-g', '-O1', '-fuse-ld=lld', src, '-o', exe], capture_output=True, text=True, cwd=out)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    if b.stderr.strip():
        print(b.stderr.strip())
    cap_args = []
    for c in CAPTURES:
        p = os.path.join(caps, c)
        if not os.path.isdir(p):
            print('FAIL: capture missing: ' + p)
            return 1
        cap_args += ['--capture', p]
    r = subprocess.run([exe] + cap_args, capture_output=True, text=True, cwd=out)
    print(r.stdout.strip())
    if r.returncode:
        print('FAIL: the unit / rig failed')
        return 1
    bad = 0
    for plant, name in PLANTS.items():
        p = subprocess.run([exe, '--plant', plant] + cap_args, capture_output=True, text=True, cwd=out)
        failed = [ln for ln in p.stdout.splitlines() if ln.startswith('FAIL')]
        ok = p.returncode != 0 and any(name in ln for ln in failed)
        print('twin %-14s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', name))
        bad += 0 if ok else 1

    def text(rel):
        with open(os.path.join(root, rel), encoding='utf-8') as f:
            return f.read()
    need = [
        ('src/d3d_textures.h', 'movie_planes_tap::OnFlush(set)', 'the existing sub_827FB540 hook hands the display set to the movie planes store'),
        ('src/scene_live/movie_planes_tap.h', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_movie_planes, false', 'the dev switch defaults to false'),
        ('src/scene_live/movie_planes_tap.h', 'wwe2k14::IsDevBuild() && REXCVAR_GET(wwe2k14_rdr_movie_planes)', 'the switch is DEV ONLY: a release build never copies'),
        ('src/scene_live/movie_planes_tap.h', 'if (!Active()) return;', 'the flush entry returns at once with the switch off'),
        ('src/scene_live/movie_planes.h', 'std::atomic<bool> g_guest_planes{false}', 'MovieOurs takes guest planes only when the switch is on'),
        ('src/wwe2k14_app.h', 'if (wwe2k14::movie_planes_tap::Active()) {', 'OnPostSetup arms MovieOurs from the same predicate the flush copy runs on (outside the scene-records block)'),
        ('src/renderer_scene/movie_ours.cpp', 'g_guest_planes.load', 'MovieOurs reads the switch at every draw'),
        ('src/renderer_scene/movie_ours.cpp', 'mp::ServeGuest(mp::Store()', 'MovieOurs asks the plane store through ServeGuest, not the stream draw'),
        ('src/renderer_scene/movie_ours.cpp', 'mp::MakeQuery(h, guest_frame)', 'MovieOurs builds its Query with the shared MakeQuery'),
    ]
    for rel, needle, why in need:
        ok = needle in text(rel)
        print('wire %-7s %s: %s' % ('ok' if ok else 'MISSING', rel, why))
        bad += 0 if ok else 1
    # (S103 P4F review F6: "no stream image is drawn" is proved by the unit's guest.no_stream_image case + its leak_stream twin over ServeGuest with poison ids, not by a text slice.)
    s = subprocess.run([CLANG] + FLAGS + ['-fsyntax-only', os.path.join(root, 'src', 'renderer_scene', 'movie_ours.cpp')], capture_output=True, text=True, cwd=root)
    print('syntax %s movie_ours.cpp' % ('ok' if s.returncode == 0 else 'FAILED\n' + s.stderr))
    bad += 0 if s.returncode == 0 else 1
    print('%d problem(s)' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
