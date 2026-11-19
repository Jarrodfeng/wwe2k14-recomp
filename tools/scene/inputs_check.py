#!/usr/bin/env python3
"""S103 lane A gate check: builds tools\\scene\\inputs_unit.cpp (the scene inputs' stamping, the post settings reader, the latch contributor, the record round trip: 0 game runs, no game, no SDK),
runs it, then its seven planted twins (each must exit 1 and fail its NAMED check), then the wiring lints (the readers are in the files they must be in; no hook added).
  py -I tools\\scene\\inputs_check.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {
    'lh_old_hold': 'lh.default_equal_entry_plans_no_hold',   # S103 LH: the old rule (a default slot's equal learned entry ages and plans a refresh walk)
    'lag0_is_lag1': 'stamp.lag0_rejects_old', 'stamp_next': 'stamp.same_frame', 'skip_unreadable': 'post.unreadable_no_stamp', 'word_order': 'post.words',
    'alpha_off': 'model.alpha', 'pass_mode_off': 'model.pass_mode', 'param_read_slot0': 'params.live_source', 'param_size_count': 'params.live_source', 'cam_slot1': 'camera.eye', 'deferred_read': 'emit.deferred_no_read', 'sentinel0': 'stamp.fresh_not_satisfied', 'mode_ignored': 'mode.off_reads_nothing', 'pal_decoy': 'palette.values', 'dead_refeed': 'wrestler.dead_leaves', 'pal_nocount_stamps': 'palette.nocount_no_stamp', 'param_name_hash': 'params.names', 'param_models_list': 'params.liveness_list', 'param_half_null': 'params.half_null', 'param_stride16': 'params.entries', 'param_part_swap': 'params.parts', 'plane_swap': 'plane.matrices', 'plf_wrong_off': 'plf.structure_and_participants', 'plane_dup': 'plane.no_duplicate', 'plane_no_gate': 'plane.stale_refused', 'a3_no_prelatch': 'a3.palette_no_latch', 'a3_slot_off': 'a3.arena_models', 'a3_stamp_lag': 'a3.arena_same_frame', 'a3_cache_off': 'a3.circle_cache', 'a3_ground_flag': 'a3.ground_ring', 'a3_node_off': 'a3.circle_rows', 'a3_light_direct_ignored': 'a3.light_direct_install_object', 'a3_no_derived_head': 'a3.palette_derived_mismatch_withholds', 'a3_piece_unowned': 'a3.piece_owner_dead_not_fed', 'a3_walked_as_ours': 'a3.circle_latch_only_frames', 'a3_light_consumer_ignored': 'a3.light_no_consumer_no_stamp', 'hold_ignored': 'hold.drops_the_held_stamp_only', 'a3_light_no_hold': 'a3.light_planned_hold', 'a4_hole_filled': 'a4.light_hole_not_ok', 'a4_future_fresh': 'a4.light_future_stale', 'a4_flag_ignored': 'a4.light_flag_off_no_stamp', 'a4_moved_hardcoded': 'a4.light_moved_follows_flag', 'a4_stale_fresh': 'a4.light_stale_refused', 'a5_ever_ignored': 'a5.hole_lapsed_vs_never', 'a5_default_filled': 'a5.no_install_stays_hole', 'a5_ask_all': 'a5.only_emitted_asked', 'a5_offscreen_asked': 'a5.offscreen_not_asked', 'a5_emitted_hole_dropped': 'a5.emitted_without_entry_is_hole', 'a3_light_slot': 'a3.light_slot_isolation', 'a3_light_nostamp_empty': 'a3.light_empty_no_stamp', 'a3_proxy_swap': 'a3.proxy_words', 'a3_proxy_stamp_early': 'a3.proxy_stamp_needs_copy', 'a3_mesh_always': 'a3.circle_no_mesh_no_stamp', 'a3_vp_plain': 'a3.circle_module', 'a3_stage_same': 'a3.stage_changed_no_stamp', 'a3_default_vec': 'a3.circle_default_vec', 'light_off44': 'lights.block_offset', 'light_remap_id': 'lights.remap_ids', 'light_lose_sec': 'lights.roundtrip', 'pal_stamp_bad': 'palette.unreadable_no_stamp', 'drop_missing': 'decide.missing_input', 'lose_section': 'record.roundtrip',
    'ce_old_key_rule': 'ce.entrance_latch_stamps', 'ce_keyoff_is_keyed': 'ce.all_keyoff_no_stamp', 'ce_zero_dir_usable': 'ce.zero_dir_lit_key_unusable', 'ce_fit_as_readfail': 'ce.plane_nan_fit_counts_nonfinite_fit',
    'pb_offscreen_withheld': 'pb.offscreen_fit_stamps', 'pb_any_nonfinite_nodraw': 'pb.other_nonfinite_withheld', 'pb_merge_drops_nodraw': 'pb.merge_keeps_nodraw', 'pb_e2_nodraw_hole': 'pb.e2_nodraw_no_hole',
    'dc_slot_ignored': 'drawcam.thread_slot', 'dc_age_ignored': 'drawcam.age', 'dc_hook_clock': 'drawcam.latch_fallback', 'dc_light_not_skipped': 'drawcam.light_skipped', 'l31b_hook_targets': 'l31b.slot_read_after_hook_quiet', 'l31b_guard_off': 'l31b.stale_never_emitted',
    'ausf_refuse': 'l33_ausf.nowalk_mismatch_submitted', 'ausf_drop': 'l33_ausf.stale_held_bounded', 'ausf_hold_hidden': 'l33_ausf.hidden_stale_not_held', 'ausf_hold_slot': 'l33_ausf.held_never_over_fresh_slot', 'ausf_hold_extras': 'l33_ausf.held_keeps_hair_rows', 'ausf_offlatch': 'l33_ausf.offlatch_mismatch_refused',   # S103 AUSF: today's refusal / drop in a no-walk build
    'l31b_pointer_presence': 'l31b.presence_by_slot', 'l31b_drop_invisible': 'l31b.stale_invisible_stays_hole', 'l31b_no_walk_gate': 'l31b.quiet_unseen_is_hole', 'l31b_keep_gone': 'l31b.gone_slot_target_released', 'b4x_long_life': 'b4x.no_long_life_for_any_entry', 'b4x_noid_walk_learned': 'b4x.culled_no_id_slot_never_lapses', 'b4x_f4_not_asked': 'b4x.f4_no_slot_skeleton_asked_with_synthesis', 'b4x_f1_le_read': 'b4x.f1_le_lg_unread', 'b4x_f2_default_replaces': 'b4x.f2_default_never_replaces_learned', 'b4x_f3_no_row_none': 'b4x.f3_no_row_default', 'b4x_f5_oldest_all': 'b4x.f5_unasked_entry_plans_no_hold', 'l33_strict_derived': 'l33.mismatch_walk_equal_accepted', 'census_plane_bit': 'census.plane_bit_dropped_only_for_no_module', 'page_row_pass': 'census.stage_page_row_class', 'unrequire_ignored': 'unrequire.drops_named_bits_only', 'l33_no_evidence': 'l33.mismatch_ours_no_evidence_refused',   # S103 L33F   # S103 L31B: the old hook-liveness targets; the stale guard off
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's103', 'a')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'inputs_unit.exe')
    srcs = [os.path.join(root, 'tools', 'scene', 'inputs_unit.cpp'), os.path.join(root, 'src', 'renderer_scene', 'record_io.cpp'), os.path.join(root, 'src', 'renderer_scene', 'live_scene.cpp'), os.path.join(root, 'src', 'scene_live', 'wrestler_contrib.cpp'), os.path.join(root, 'src', 'renderer_scene', 'live_map_e2_input.cpp'), os.path.join(root, 'src', 'renderer_scene', 'live_map_rope_input.cpp')]
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-fuse-ld=lld']
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
        print('twin %-16s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', name))
        bad += 0 if ok else 1

    def text(rel):
        with open(os.path.join(root, rel), encoding='utf-8') as f:
            return f.read()
    need = [
        ('src/wwe2k14_app.h', 'scene_inputs_glue::Register()', 'OnPostSetup registers the inputs contributor'),
        ('src/scene_live/inputs_glue.h', 'EmittedSkeletons', 'the model list is the wrestlers contributor\'s kept set'),
        ('src/scene_live/sphere_glue.h', 'scene_inputs::Capture(scene_inputs::kInSphereOcc', 'the sphere update hook stamps its input'),
        ('src/scene_live/inputs_contrib.h', 'scene_inputs::Capture(scene_inputs::kInPostSettings', 'the post settings are stamped where they are read'),
        ('src/renderer_scene/record_io.cpp', 'Sec(kScPostParams, t.post_params)', 'the post section is written'),
        ('src/renderer_scene/live_map_plane.cpp', 'if (in.plane_nodraw) {   // S103 PB', 'RecordPlane skips a no-draw plane frame (empty skip = not a hole)'),
    ]
    for rel, needle, what in need:
        if needle not in text(rel):
            print('FAIL: lint: %s (%s: %s missing)' % (what, rel, needle))
            bad += 1
    # S103 B4X (Q1 class): no rule anywhere in bit 4 reads an OURS-only signal or an age / keep state carried on a computed value
    for rel in ('src/scene_live/light_index_core.h', 'src/scene_live/light_index_hook.h', 'src/scene_live/inputs_contrib.h'):
        code = '\n'.join(ln.split('//')[0] for ln in text(rel).split('\n'))
        for tok in ('seen_ours', 'kProvenAge', 'kProveConfirms', 'kProveSpan', 'Keepable', 'KeepCtx', 'arena_kept', 'keep_life', 'g_keep_steady_arena', 'lightidx::Proven', 'scene_frame::Ours'):
            if tok in code:
                print('FAIL: lint: %s uses %s (bit 4 reads no OURS-only signal, S103 B4X)' % (rel, tok))
                bad += 1
    # this lane adds no REX_HOOK (the readers run at the latch: a function takes one hook, S101)
    for rel in ('src/scene_live/inputs_core.h', 'src/scene_live/inputs_contrib.h', 'src/scene_live/inputs_glue.h', 'src/scene_live/post_settings.h'):
        if 'REX_' + 'HOOK' in text(rel):
            print('FAIL: lint: %s adds a hook (the readers run at the latch)' % rel)
            bad += 1
    # the model map has ONE accessor (scene_record.h GetModelMapEntry / SetModelMapEntry): a hand-indexed words[3 * k] / words[4 * k] anywhere else is the E4 triples-vs-quadruples bug waiting to happen
    for sub in ('src/scene_live', 'src/renderer_scene', 'src/renderer_game'):
        d = os.path.join(root, *sub.split('/'))
        for fn in sorted(os.listdir(d)):
            if fn.endswith(('.h', '.cpp')) and fn != 'scene_record.h':
                with open(os.path.join(d, fn), encoding='utf-8') as f:
                    for n, ln in enumerate(f, 1):
                        if 'words[3 * k' in ln.split('//')[0] or 'words[4 * k' in ln.split('//')[0]:
                            print('FAIL: lint: %s:%d indexes the model map by hand (use GetModelMapEntry / SetModelMapEntry)' % (fn, n))
                            bad += 1
    # Windows macros the GAME build includes (near / far / small / interface): never a variable or member name in the scene_live headers (the round-4 `bool near` broke main.cpp; the unit build does not see the macros)
    import re
    rx = re.compile(r'\b(?:bool|int|uint32_t|uint64_t|float|auto|size_t)\s+(?:near|far|small|interface)\b')
    sl = os.path.join(root, 'src', 'scene_live')
    for fn in sorted(os.listdir(sl)):
        if fn.endswith(('.h', '.cpp')):
            with open(os.path.join(sl, fn), encoding='utf-8') as f:
                for n, ln in enumerate(f, 1):
                    if rx.search(ln.split('//')[0]):
                        print('FAIL: lint: %s:%d names a variable near / far / small / interface (a Windows macro in the game build)' % (fn, n))
                        bad += 1
    if bad:
        print('FAIL: %d problem(s)' % bad)
        return 1
    print('PASS: unit, %d planted twins caught, lints' % len(PLANTS))
    return 0


if __name__ == '__main__':
    sys.exit(main())
