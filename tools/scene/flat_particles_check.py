#!/usr/bin/env python3
"""S111 TITLESMOKE check: builds tools\\scene\\flat_particles_unit.cpp (src\\renderer_scene\\flat_particles.h: where a flat frame's particle groups go among its 2D rows; no game, no SDK,
no renderer), runs it, then its planted twins (each must exit 1 and fail its NAMED check), then the wiring lints (the host draws the groups, the drawer filters by position).
  py -I tools\\scene\\flat_particles_check.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {'end': 'title.row_positions', 'off_by_one': 'title.row_positions', 'scene_flat': 'mixed.scene_batch_not_flat', 'old_count': 'seq.refused_row_before_node'}   # S111 FLATSEQ: + old_count


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')))
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 'tools')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'flat_particles_unit.exe')
    flags = ['-std=c++20', '-Wall', '-Wextra', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-DNOMINMAX', '-fuse-ld=lld']
    b = subprocess.run([CLANG] + flags + [os.path.join(root, 'tools', 'scene', 'flat_particles_unit.cpp'), '-I', os.path.join(root, 'src'), '-o', exe], capture_output=True, text=True)
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
        print('twin %-12s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', name))
        bad += 0 if ok else 1

    def text(rel):
        with open(os.path.join(root, rel), encoding='utf-8') as f:
            return f.read()
    need = [
        ('src/renderer_host/scene_host.cpp', 'draw_->DrawFlatParticles(rec, v, pos, fb)', 'DrawRows draws the flat frame\'s particle groups'),
        ('src/renderer_host/scene_host.cpp', 'StopsOf(rec.particles, rec.quads2d.data(), n)', 'DrawRows places the groups by the rows\' seq (S111 FLATSEQ)'),
        ('src/renderer_host/scene_host.cpp', 'if (rdr_scene::flat_particles::HasScreen(rec.particles)) return true;', 'RowsMayDraw says F is drawn for such a frame'),
        ('src/renderer_host/scene_host.cpp', 'while (flat_next < flat_stops.n) draw_parts(flat_stops.at[flat_next++]);', 'the groups after the last row are drawn'),
        ('src/renderer_scene/live_particles.cpp', 'if (flat && (!flat_particles::IsFlat(b) || flat_particles::PosOf(pt, b, rows, nrows) != pos)) continue;', 'RecordFlat draws only the flat (screen-call) batches of its position'),
        ('src/scene_live/particle_source.h', 'out.screen.stamp = rdr_scene::kParticleStampSeq', 'the recorder marks its batches as 2D-ticket stamped (S111 FLATSEQ)'),
        ('src/renderer_scene/live_particles.cpp', 'state.depth_test = flat ? 0 : 1;', 'the flat draw has no depth test (F has no depth)'),
        ('src/renderer_scene/frame_hooks.cpp', 'h.AttachParticles(rec);', 'the 3D frame attaches the stamp and lookup through the shared function'),
        ('src/renderer_scene/live_scene_draw.h', 'live_hooks_->AttachParticles(rec);', 'the flat draw attaches this record\'s stamp and lookup'),
    ]
    for rel, needle, what in need:
        if needle not in text(rel):
            print('FAIL: lint: %s (%s: %s missing)' % (what, rel, needle))
            bad += 1
    if 'no flat particle drawer exists yet' in text('src/renderer_host/scene_host.cpp'):
        print('FAIL: lint: scene_host.cpp still says no flat particle drawer exists')
        bad += 1
    if bad:
        print('FAIL: %d problem(s)' % bad)
        return 1
    print('OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
