#!/usr/bin/env python3
"""S103 lane C3 check: builds tools\\scene\\ring_recompute_unit.cpp (the ring ropes' node rows recomputed from the rope grid on a fake guest: hand-worked values, the three heights, the rest pose, table mode, the gates, the contributor's mode
gate, the record round trip: 0 game runs, no game, no SDK), runs it, then its planted twins (each must exit 1 and fail its NAMED check), then the wiring lints.
  py -I tools\\scene\\ring_recompute_check.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {
    'grid_stride': 'ring.grid_values', 'desc_swapped': 'ring.descriptors', 'side_stride': 'ring.rest_pose', 'counts_swapped': 'ring.counts', 'layer_swapped': 'ring.rope_height',
    'table_stride': 'ring.table', 'stale_x': 'ring.noted_expires', 'tb_idx_swapped': 'ring.tb_values', 'tb_ang_off': 'ring.tb_values', 'tb_inv_off': 'ring.tb_values', 'clock_early': 'contrib.latch_emit', 'mode_ignored': 'mode.off_reads_nothing', 'lose_flag': 'record.roundtrip',
    # S103 C4: covers, posts, skirts, the whole-ring verdict (input bit 6)
    'cv_desc_stride': 'ring.cv_values', 'cv_ang_base': 'ring.cv_values', 'cv_side_plus': 'ring.cv_values', 'cv_ovr_off': 'ring.cv_override', 'cv_alpha_off': 'ring.cv_alpha', 'post_step_off': 'ring.post_values',
    'skirt_pal_off': 'ring.skirt_values', 'whole_no_covers': 'ring.whole_stamp',
    # S103 C5: skirt visibility, one LOD model
    'skirt_quad_wrong': 'ring.skirt_visible', 'lod_both': 'ring.lod_single',
    # S103 C6: the two address misreads of run 26 (lis 1 | ori; lis -31889 = 0x836F)
    'cv_desc_low': 'ring.cv_values', 'skirt_glob_wrong': 'ring.skirt_visible',
    # S111 RINGLOD: the skirt state words are the ring-SIDE object's (Y), not the ring object's
    'skirt_side_obj': 'ring.skirt_visible',
    # S103 RLB: the rope LOD rule (RLD.md): the 500 distance, the 20 degree angle
    # S105 C4 / S111 RINGLOD: the post / cover far test reads the ring object's X + 0x1F314 / X + 0x1F320 (3000): a guest value of 1e9 means nothing is far, so the far-test check must fail
    'lod_dist': 'ring.lod_far_test',
    # S110 RINGBREAK: the ring in motion posing (rows = the per-slot copies taken right after each pose call) and the canvas row
    'rids_latch_read': 'ring.rids_values', 'rids_table': 'ring.rids_values', 'canvas_rest': 'ring.canvas_row',
    # S111 TBPAD review: the RIDS rows read the corner pad fades too (sub_825CE668 tests them after the RIDS pose)
    'rids_fade_off': 'ring.rids_fade',
    'lod_dist50': 'ring.lod_distance', 'lod_no_angle': 'ring.lod_angle', 'lod_nan_low': 'ring.lod_nan', 'lod_pair_call': 'ring.lod_pairs',
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's103', 'c3r')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'ring_recompute_unit.exe')
    srcs = [os.path.join(root, 'tools', 'scene', 'ring_recompute_unit.cpp'), os.path.join(root, 'src', 'renderer_scene', 'record_io.cpp'), os.path.join(root, 'src', 'renderer_scene', 'live_scene.cpp')]
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
        print('twin %-18s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', name))
        bad += 0 if ok else 1

    def text(rel):
        with open(os.path.join(root, rel), encoding='utf-8') as f:
            return f.read()
    need = [
        ('src/scene_live/stage_fx_glue.h', 'RegisterRingRecompute()', 'the glue registers the ring recompute contributor'),
        ('src/scene_live/stage_fx_glue.h', 'RingRecompute().SetReader(&wwe2k14::scene_inputs_glue::ReadChecked)', 'the recompute reads guest memory through the page-checked reader'),
        ('src/scene_live/scene_skip_nodes.h', 'ringrecompute::NoteRing(ctx.r3.u32 - 24, wwe2k14::scene_skip::HookFrame(), &view)', 'the ring colour callback hook hands over the ring object X'),
        ('src/scene_live/scene_skip_nodes.h', 'ReadRingView(ctx, base, view)', 'S103 RLB: the ring hook reads the draw thread camera for the rope LOD rule'),
        ('src/wwe2k14_app.h', 'scene_stagefx_glue::Register()', 'OnPostSetup registers the stage glue'),
        ('src/scene_live/stage_fx_glue.h', 'scene_inputs::Capture(scene_inputs::kInRingRows, scene_inputs::HookFrameOfRecord(record_frame))', 'the glue stamps input bit 6 (ring_rows) in the hook clock'),
        ('src/scene_live/inputs_core.h', '{kInRingRows, "ring_rows", 2, rdr_scene::live::ringrecompute::kStampRingRows,', 'bit 6 moved follows kStampRingRows (ring_stamp_flag.h; released S103 main)'),
        ('src/scene_live/ring_stamp_flag.h', 'inline constexpr bool kStampRingRows = true;', 'bit 6 released (run 27 + lane C7 compare); the stamp still goes through kStampRingRows'),
        ('src/scene_live/ring_side_hook.h', 'ringrecompute::NoteRingSide(ctx.r3.u32)', 'S111 RINGLOD: the skirts draw hook hands over the ring-side object Y'),
        ('src/draw_origin.h', '#include "scene_live/ring_side_hook.h"', 'S111 RINGLOD: the ring-side hook is compiled (one TU)'),
        ('src/scene_live/ring_recompute_contrib.h', 'ReadSkirtVisibility(read_, RingSide(), svis, census_)', 'S111 RINGLOD: the skirt states are read from Y'),
        ('src/scene_live/ring_recompute_contrib.h', 'if (kStampRingRows && clean && stamp_) stamp_(frame);', 'the stamp call sits behind kStampRingRows'),
        ('src/renderer_scene/rf_ring.cpp', 'if (ctx.warm()) return true;', 'the warm build poses nothing'),
        ('src/renderer_scene/rf_ring.cpp', 'PoseNeeded(rs.hash, skey, rows_hash, live_id >= 0)', 'an unchanged instance is not posed or uploaded again'),
        ('src/renderer_scene/rf_ring.cpp', 'PoolName(as->key, asked)', 'the posed pool is named by the asked key'),
        ('src/renderer_scene/rf_c_core.cpp', 'ctx.models.RetireOther(kv.second.set->key + ":ringpose:", "\\x01");', 'an arena change retires the old set posed ring pools'),
        ('src/renderer_scene/rf_c_core.cpp', 'ctx.models.RetireOther(st.set->key + ":ringpose:", "\\x01");', 'a signature change retires the old set posed ring pools'),
    ]
    for rel, needle, what in need:
        if needle not in text(rel):
            print('FAIL: lint: %s (%s: %s missing)' % (what, rel, needle))
            bad += 1
    # this lane adds no hook (the reads run at the latch: a function takes one hook, S101)
    for rel in ('src/scene_live/ring_recompute_core.h', 'src/scene_live/ring_recompute_contrib.h'):
        if 'REX_HOOK' in text(rel):
            print('FAIL: lint: %s adds a hook' % rel)
            bad += 1
    print('RESULT', 'OK' if bad == 0 else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
