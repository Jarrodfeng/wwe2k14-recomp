#!/usr/bin/env python3
"""S103 lane MP check (menu particles: empty-is-whole + every node call recorded with the node's own camera and 2D row position).
Builds tools/scene/mp_unit.cpp (the SHIPPED particle_source.h / particle_contrib.cpp / particle_reader.cpp / record_io.cpp / scene_kind.h on synthetic guest memory; 0 game runs), runs it, then its planted
twins (each must exit 1 and fail its NAMED check), then the wiring lints (the hook passes the thread block and the row count; the glue wires the empty source; the flat verdict reads the record).
  py -I tools/scene/mp_check.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {3: 'empty.node_call_not_empty', 4: 'empty.zero_words', 5: 'groups.every_call', 6: 'camera.node_own', 7: 'groups.row_position', 8: 'empty.every_call', 9: 'calls.overflow', 10: 'cae.particles_draw_camera', 11: 'nodes.every_node', 12: 'nodes.high_ids', 13: 'nodes.cam_mismatch', 14: 'nodes.stable_order', 15: 'soft.flat_hard', 16: 'nodes.entry_list', 17: 'groups.scene_call_unstamped', 18: 'camera.game_ortho'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's103', 'mp')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'mp_unit.exe')
    s = os.path.join(root, 'src')
    srcs = [os.path.join(root, 'tools', 'scene', 'mp_unit.cpp'), os.path.join(s, 'renderer_scene', 'record_io.cpp'), os.path.join(s, 'scene_live', 'particle_contrib.cpp'),
            os.path.join(s, 'scene_live', 'particle_reader.cpp')]
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-DNOMINMAX', '-fuse-ld=lld']
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
        p = subprocess.run([exe, '--plant', str(plant)], capture_output=True, text=True)
        failed = [ln for ln in p.stdout.splitlines() if ln.startswith('FAIL')]
        ok = p.returncode != 0 and any(ln.startswith('FAIL ' + name + ' ') or ln.startswith('FAIL ' + name + '\n') or ln == 'FAIL ' + name for ln in failed)
        print('twin %d %-26s %s' % (plant, name, 'caught' if ok else 'NOT CAUGHT'))
        bad += 0 if ok else 1

    def text(rel):
        with open(os.path.join(root, *rel.split('/')), encoding='utf-8') as f:
            return f.read()
    need = [
        ('src/scene_live/scene_skip_nodes.h', 'NoteNode(ctx.r3.u32, ctx.r13.u32, wwe2k14::q2d_tap::RowsThisFrame(), wwe2k14::q2d_tap::SeqNow())', 'the node hook passes the thread block (the node\'s camera), the 2D row count and the 2D store\'s last ticket (S111 FLATSEQ)'),
        ('src/scene_live/quads2d_tap.h', 'inline uint32_t SeqNow()', 'the q2d tap exposes the store\'s last ticket (S111 FLATSEQ)'),
        ('src/scene_live/particle_glue.h', 'SetEmptySource(', 'the glue wires the empty-pass source (no node call = whole empty)'),
        ('src/scene_live/particle_glue.h', 'ReadNodeCamera(', 'the glue reads the node\'s own camera at the entry'),
        ('src/scene_live/quads2d_tap.h', 'inline uint32_t RowsThisFrame()', 'the q2d tap exposes the frame\'s running row count'),
        ('src/renderer_adapter/live.cpp', 'scene_kind::PartsOf(rec.particles.EmptyPass()', 'the flat verdict reads the record\'s particle verdict'),
        ('src/renderer_scene/record_io.cpp', 'case kScParticleScreen:', 'the record loads the screen-camera section'),
        ('src/renderer_scene/record_io.cpp', 'Sec(kScParticleScreen,', 'the record writes the screen-camera section'),
    ]
    for rel, needle, what in need:
        if needle not in text(rel):
            print('FAIL: lint: %s (%s: %s missing)' % (what, rel, needle))
            bad += 1
    # S103 MPBF 10 / 9 lints. (a) a particle-status `reasons` read test must go through kPiInfoMask (the info bits are never a reason); a bit test of ONE named reason is fine; a line that only decides to WRITE the row
    # carries the marker `lint:reasons-ok`. (b) every ParticleTables vector is listed in the swap-once size guard's FillEverything and its sz() list. (c) the live.cpp PartsOf call site asserts the info bits.
    import re
    scope = ['src/scene_live/particle_source.h', 'src/scene_live/particle_contrib.cpp', 'src/scene_live/particle_contrib.h', 'src/scene_live/presence_rules.h', 'src/scene_live/input_changes.h',
             'src/renderer_scene/particle_tables.h', 'src/renderer_game/scene_kind.h', 'src/renderer_adapter/live.cpp']
    pat = re.compile(r'(!\s*[\w.\[\]>-]*reasons\b(?!\s*[&|=])|\breasons\s*\)?\s*[=!]=\s*0|\bif\s*\(\s*[\w.\[\]>-]*reasons\s*\)|\breasons\s*&\s*~)')
    for rel in scope:
        for n, ln in enumerate(text(rel).splitlines(), 1):
            code = ln.split('//')[0]
            if pat.search(code) and 'kPiInfoMask' not in code and 'lint:reasons-ok' not in ln and 'kInfo' not in code:
                print('FAIL: lint: %s:%d tests `.reasons` without kPiInfoMask: %s' % (rel, n, ln.strip()[:120]))
                bad += 1
    pt = text('src/renderer_scene/particle_tables.h')
    m = re.search(r'struct ParticleTables \{(.*?)\n\};', pt, re.S)
    vecs = re.findall(r'std::vector<\w+>\s+(\w+);', m.group(1)) if m else []
    sw = text('tools/swap/swap_once_test.cpp')
    for v in vecs:
        if not re.search(r't\.particles\.%s\.resize' % v, sw) or not re.search(r't\.particles\.%s\.size\(\)' % v, sw):
            print('FAIL: lint: ParticleTables::%s is not in swap_once_test.cpp FillEverything / the sz() list' % v)
            bad += 1
    if not vecs:
        print('FAIL: lint: could not read the ParticleTables vectors')
        bad += 1
    if 'static_assert(rdr_scene::kPiEmptyPass == 256' not in text('src/renderer_adapter/live.cpp'):
        print('FAIL: lint: live.cpp must static_assert the info bits next to its PartsOf call')
        bad += 1
    print('mp_check: %s' % ('PASS' if not bad else 'FAIL (%d)' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
