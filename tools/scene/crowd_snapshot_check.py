#!/usr/bin/env python3
"""S103 lane D3 check: builds tools\\scene\\crowd_snapshot_unit.cpp (required input bit 10, `crowd_snapshot`: the crowd contributor stamps only a whole snapshot; 0 game runs, no game, no SDK),
runs it, then its five planted twins (each must exit 1 and fail its NAMED check), then wiring lints (the sink is set where the contributor is registered; the input is flagged moved; no new hook).
  py -I tools\\scene\\crowd_snapshot_check.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {
    'presence_ignored': 's2.holder_no_stamp',          # S103 S2: hold by presence (crowd_snapshot_s2_part.h)
    'presence_lod_absent': 's2.lod_holder_present',
    'presence_partial_kept': 's2.nothing_shown_stamps',
    'stale_stamped': 'snap.stale_no_stamp',
    'unreadable_ignored': 'snap.unreadable_no_stamp',
    'record_frame_clock': 'snap.hook_clock',
    'mode_off_stamps': 'snap.off_costs_nothing',
    'palette_ignored': 'snap.palette_required',
    'unobserved_ignored': 'snap.floor_unobserved_blocks',   # S103 D3b (review F1)
    'yaw_sign': 'latch.world_matches_walk',     # S103 D3b
    'card_mask': 'latch.cards_rule',
    'remap_ignored': 'latch.palette_remap',
    'floor_near_cut_ignored': 'latch.floor_near_cut',   # review round 2, item 2
    'prop_mul_order': 'latch.prop_world',        # D3c: the sign / foam world is B x M0
    'floor_not_transposed': 'latch.floor_world',    # round 2: the seated floor people
    'card_gate_ignored': 'latch.card_gate',     # review F1
    'prop_distance_ignored': 'latch.prop_distance',      # S103 D5: the game's gates of the sign / foam-hand loops
    'prop_eye_composer': 'latch.prop_distance_handed_eye',   # S110 CUTCROWD: the eye is the handed block's, as ReadSceneCamera's
    'prop_texture_ignored': 'latch.prop_texture',
    'prop_exit_ignored': 'latch.prop_hand_exit',
    'prop_global_ignored': 'latch.prop_sign_global',
    'single_rot_order': 'latch.single_palette',
    'single_parity': 'latch.single_palette',
    'single_chain_order': 'latch.single_skin',   # S103 D6
    'soft_scope_off': 'latch.soft_floor_object',   # S103 D8 (run 27: 8196 of 10596 reads declined unreadable)
    'entry_ring_ignored': 'latch.entry_ring_used',
    'picture_handle_unchecked': 'latch.picture_handle_checked',
    'picture_tiled_ignored': 'latch.picture_blocks',
    'picture_endian_ignored': 'latch.picture_blocks',
    'sign_guest_read_despite_file': 'latch.sign_file_no_guest_read',   # S103 CSF fix 2
    'sign_stale_key_slot': 'latch.sign_file_key_remap',   # S103 CSF review 1
    'sign_check_skipped': 'latch.sign_file_mismatch_guest',   # S103 CSF review 2
    'picture_wrong_stride': 'latch.picture_blocks',   # S103 CSF review C (the hand picture is linear now)
    'picture_huge_pitch_unbounded': 'latch.picture_huge_pitch_refused',
    'picture_linear_padded_cap': 'latch.picture_linear_padded_cap',   # S103 CSF fix 1
    'picture_packed_ignored': 'latch.picture_packed_mips',
    'picture_fail_leaks': 'latch.picture_bad_pointer_local',
    'rows_hidden_ignored': 'rows.hidden_flag',
    'rows_kind_swapped': 'rows.props_appended',
    'walked_props_dropped': 'latch.walked_props_rows',   # S103 SG2 (run 37: signs never entered the record on walked frames)
    'end_frame_no_latch': 'latch.endframe_latches',   # S103 D7 (run 26)
    'end_frame_only_crowd': 'latch.endframe_latches_all',   # S103 D7 class fix: every contributor latched
    'entry_read_ignored': 'latch.entry_read_in_place',   # S103 D9 (run 28: 448 shown-prop diffs, in-place bones read after the next update)
    'entry_stale_consumed': 'latch.entry_snapshot_one_latch',   # S103 D9 review 2
    'pal_hist_wrong_ring': 'latch.palette_history_ring',   # S103 D9 (run 28: palettes 21 equal / 635619 differ)
    'single_forward_any_class': 'latch.single_forward_foreign',   # S103 C1 (R15; run 32: 0x7964668E / 0x79646642 fault pairs)
    'single_data_count_skips_block': 'latch.single_data_object',   # S103 SN (run 34: 52396 shown holders `forward`)
    'single_screen_ignored': 'latch.single_screen_box',
    'screen_scale_ignored': 'latch.single_screen_wide',   # S105 A2: the screen tests' widening scale (also latch.floor_screen_wide)
    'single_lod_as_type': 'latch.single_lod_map',
    'floor_lod_group_ignored': 'latch.floor_lod_group',
    'unread_hole_as_other': 'latch.unread_by_hole',
    'prop_frames_unmodelled': 's2.prop_frame_holder_present',   # S103 C1 (R14-4): census label only
    'single_screen_nan_hides': 'latch.single_screen_nan_draws',   # S103 C1 review fix 1
    'screen_hidden_absent': 's2.screen_hidden_holder_present',   # review fix 2
    'floor_type_byte_skip': 'latch.floor_type_byte',             # review fix 3
    'single_entity_unchecked': 'latch.single_entity_class',      # review fix 5
    'hand_map_ignored': 'rows.hand_mesh_key',                    # S103 SG: the hand mesh is 40999 + h of its type (sub_8250B1E8), not 41016
    'hand_attire_ignored': 'latch.hand_map',                     # S103 SG: the attire index table sub_82506E30
    'crowd_leads': 'period.crowd_takes_previous_latch',          # S111 CUTCROWD2: the crowd takes its own latch's snapshot, one period ahead of the record camera
    'crowd_pairs_off': 'period.mode_switch_pairs_with_camera',   # S111 CUTCROWD2 review: a mode-Off latch feeds the crowd's pairing (the camera's only with the mode on)
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's103', 'd3')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'crowd_snapshot_unit.exe')
    srcs = [os.path.join(root, 'tools', 'scene', 'crowd_snapshot_unit.cpp'), os.path.join(root, 'src', 'scene_live', 'crowd_contrib.cpp'),
            os.path.join(root, 'src', 'renderer_scene', 'record_io.cpp'), os.path.join(root, 'src', 'renderer_scene', 'live_scene.cpp')]
    flags = ['-std=c++20', '-DRDR_CROWD_SNAP_PLANTS=1', '-DRDR_TAIL_PLANTS=1', '-DRDR_CROWD_OURS_WHOLE=1', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-fuse-ld=lld']
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
        print('twin %-20s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', name))
        bad += 0 if ok else 1

    # S103 D3c: the SHIPPED value of kCrowdOursWhole (false): a build without -DRDR_CROWD_OURS_WHOLE judges a whole snapshot whole and withholds the Capture
    exe2 = os.path.join(out, 'crowd_snapshot_unit_shipped.exe')
    flags2 = [f for f in flags if f != '-DRDR_CROWD_OURS_WHOLE=1']
    b2 = subprocess.run([CLANG] + flags2 + srcs + ['-o', exe2], capture_output=True, text=True)
    if b2.returncode:
        print('FAIL: the shipped-value build did not compile\n' + b2.stdout + b2.stderr)
        return 1
    p2 = subprocess.run([exe2, '--shipped'], capture_output=True, text=True)
    print('shipped kCrowdOursWhole=false: ' + ('ok' if p2.returncode == 0 else 'FAIL ' + p2.stdout.strip()))
    bad += 0 if p2.returncode == 0 else 1

    def text(rel):
        with open(os.path.join(root, rel), encoding='utf-8') as f:
            return f.read()
    need = [
        ('src/scene_live/crowd_hooks.h', 'SetStampSink', 'RegisterCrowd gives the contributor its stamp sink'),
        ('src/scene_live/crowd_hooks.h', 'scene_inputs::kInCrowdSnapshot', 'the sink stamps bit 10'),
        ('src/scene_live/crowd_contrib.cpp', 'NoteVerdict(verdict, f.guest_frame)', 'every Emit path stamps through TakeInputs'),
        ('src/scene_live/inputs_glue.h', 'SetLatchReader(&CrowdLatchRead)', 'the glue gives the crowd its latch reader'),
        ('src/scene_live/crowd_hooks.h', 'NoteOursSkip(node', 'the skipped crowd entry says so (the latch then reads the manager)'),
        ('src/scene_live/scene_skip_nodes.h', 'stands_pass', 'the classifier is served only under the entry gates (review item 1)'),
        ('src/scene_live/crowd_hooks.h', 'NoteUnobserved()', 'floor people the observer is off for are counted unread (review F1)'),
        ('src/scene_live/crowd_contrib.cpp', 'NoteVerdict(l.judged, f.guest_frame, &l.presence)', 'the stamp is made at the latch (review F2), with the carried snapshot\'s verdict (S111 CUTCROWD2)'),
        ('src/renderer_scene/live_scene.cpp', 'LatchImpl(guest_frame, capture_frame, /*allow_defer=*/false', 'EndFrame takes the tail latch route (S103 D7)'),
        ('src/scene_live/inputs_core.h','{kInCrowdSnapshot, "crowd_snapshot", 2, true,', 'bit 10 is flagged moved (its stamp lives in its owner)'),
    ]
    for rel, needle, what in need:
        if needle not in text(rel):
            print('FAIL: lint: %s (%s: %s missing)' % (what, rel, needle))
            bad += 1
    # no hook added: the snapshot is the contributor's existing one
    if 'REX_' + 'HOOK' in text('src/scene_live/crowd_snapshot_core.h'):
        print('FAIL: lint: crowd_snapshot_core.h adds a hook')
        bad += 1
    if bad:
        print('FAIL: %d problem(s)' % bad)
        return 1
    print('PASS: unit, %d planted twins caught, lints' % len(PLANTS))
    return 0


if __name__ == '__main__':
    sys.exit(main())
