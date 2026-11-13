# Lane 138 (D3 skip the object): builds tools/d3/d3_unit_test.cpp (the shipped d3_arena.h / d3_guest_core.h headers) and runs it: the honest arm must pass (exit 0),
# every planted fault must FAIL it (exit 1). Output: out/tools/d3_unit_test.exe (gitignored).
import subprocess, sys
from pathlib import Path
root = Path(__file__).resolve().parents[2]
exe = root / 'out' / 'tools' / 'd3_unit_test.exe'
exe.parent.mkdir(parents=True, exist_ok=True)
clang = r'C:\Program Files\LLVM\bin\clang++.exe'
b = subprocess.run([clang, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Wno-unused-parameter', '-o', str(exe), str(root / 'tools' / 'd3' / 'd3_unit_test.cpp'), str(root / 'src' / 'renderer_scene' / 'record_io.cpp')], capture_output=True, text=True, timeout=300)
if b.returncode:
    print(b.stdout + b.stderr)
    print('FAIL: build')
    sys.exit(1)
ok = True
r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=600)
print(r.stdout.strip())
if r.returncode != 0:
    ok = False
for plant, named in (('hole_no_packet', 'scenario.no_hole'), ('skip_in_proof_frame', 'scenario.proof_frames_full'), ('late_packet', 'scenario.no_hole'),
                     ('gate_ignored', 'scenario.gate_respected'), ('ordinal_off', 'scenario.no_hole'), ('path_ignored', 'scenario.cache_paths'), ('path2_skip', 'path2.no_skip'), ('no_end_on_skip', 'twocall.steady'),
                     ('skip_unreadable', 'probe.state_before_decision'), ('no_latch', 'gate.latch'), ('packet_on_cache_path', 'packet.only_when_skippable'),
                     ('probe_old_frame', 'probe.old_frame'), ('call_index_from_hook', 'packet.call_is_ref_ordinal'), ('marker_count0', 'marker.claim_replayed'),
                     ('caster_hole_no_packet', 'caster.no_hole'), ('caster_skip_in_proof_frame', 'caster.proof_frames_full'), ('caster_stale_record', 'caster.record_backed'),
                     ('caster_withdraw_off', 'caster.record_backed'), ('caster_path2_skipped', 'caster.path2_never_skipped'), ('caster_plane_skipped', 'caster.plane_never_skipped'), ('caster_plane_cap4', 'caster.plane_calls_4_to_11'), ('caster_plane_cap4', 'caster.plane_skip_replay'), ('caster_plane_no_replay', 'caster.plane_skip_replay'),
                     ('caster_plane_skip_in_proof_frame', 'caster.plane_skip_refusals'), ('caster_plane_unproven', 'caster.plane_skip_refusals'), ('caster_plane_mismatch_skip', 'caster.plane_skip_refusals'), ('caster_plane_ledger_sticky', 'caster.plane_part_change'),   # lane 299: the plane skip arm (lane 290's fault again, no replay, a skip in a proof frame, a skip with no learned part list, a skip of an unmarked object)
 ('caster_outer_ignored', 'caster.outer_never_marked'),
                     ('ring_path2_skipped', 'ring.path2_never_skipped'), ('ring_path_unset', 'ring.path2_never_skipped'), ('ring_hole_no_packet', 'ring.no_hole'), ('ring_outer_ignored', 'ring.outer_never_marked'),
                     ('ring_wrong_chain', 'ring.one_chain'), ('ring_rows_unbacked', 'ring.rows_backed'), ('ring_skip_in_proof_frame', 'ring.proof_frames_full'), ('ring_marker_unclaimed', 'ring.marker_claimed'), ('ring_parts_dropped', 'ring.mark_parts'),   # lane 289: the mark set loses the part triples (the ordinal fill would have nothing to count)
                        # lane 246 (ring hook): path 2 skipped / the path never set (the default Fresh1), a marker with no packet, the outer recording ignored, a call decided by both chains, a skip with no ring rows behind it, a skip in a proof frame, the reader not claiming a ring marker
                     ('wrestler_skip_in_proof_frame', 'wrestler.proof_frames_full'), ('wrestler_hole_no_packet', 'wrestler.no_hole'), ('wrestler_stale_record', 'wrestler.record_backed'),
                     ('wrestler_path_default_fresh1', 'wrestler.only_fresh1'), ('wrestler_path2_skipped', 'wrestler.only_fresh1'), ('wrestler_outer_ignored', 'wrestler.outer_never_marked'),
                     ('wrestler_camera_ignored', 'wrestler.camera_required'),
                     ('wrestler_sphere_ignored', 'wrestler.sphere_row_required'), ('wrestler_wrong_chain', 'wrestler.three_scopes')):   # lane 245 (wrestler hook, 18.9): a skip in a proof frame, a marker with no packet, a stale record, the old Fresh1 default, a path-2 skip, an outer recording ignored, the caster records not required   # lane 228 (caster hooks): the three plants of the brief (a skip in a proof frame, a marker with no packet, a stale record) + the withdraw and the path-2 twins   # lane 152 (16): a skip decided before / without the state read
    p = subprocess.run([str(exe), 'plant', plant], capture_output=True, text=True, timeout=600)
    failed = [l for l in p.stdout.splitlines() if l.startswith('FAIL ') and named in l]
    caught = p.returncode == 1 and bool(failed)
    print(f'planted {plant}: {"caught by " + named if caught else "NOT CAUGHT by " + named + " (exit %d)" % p.returncode}')
    ok = ok and caught
print('PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
