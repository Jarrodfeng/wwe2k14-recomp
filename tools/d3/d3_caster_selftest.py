# Lane 211 (Caster D3 E2 binder part + E3 register binding, INERT): builds tools/d3/d3_caster_test.cpp (the shipped d3_binder.h / d3_record.h / light_state_rows.h) and runs it:
#   the honest arm must pass (exit 0); every planted fault must FAIL the check named below (exit 1); and, when a kept J1 capture exists, the camera-row rules are checked against its banks
#   (tools/caster/caster_cam_vectors.py -> `d3_caster_test.exe cam`), with the plant camera_viewproj_wrong caught by cam.props_map_viewproj_and_depth.
#   py -I tools\d3\d3_caster_selftest.py [--j1 CAPTURE_DIR]      0 game runs; ~1 min. Output: out/tools/d3_caster_test.exe, out/lane211/cam_j1.txt (gitignored).
import argparse, os, subprocess, sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
exe = root / 'out' / 'tools' / 'd3_caster_test.exe'
clang = r'C:\Program Files\LLVM\bin\clang++.exe'

ap = argparse.ArgumentParser()
ap.add_argument('--j1', default='')
ap.add_argument('--no-build', action='store_true')
args = ap.parse_args()

if not args.no_build:
    exe.parent.mkdir(parents=True, exist_ok=True)
    b = subprocess.run([clang, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Wno-unused-parameter', '-D_CRT_SECURE_NO_WARNINGS', '-o', str(exe), str(root / 'tools' / 'd3' / 'd3_caster_test.cpp'),
                        str(root / 'src' / 'renderer_scene' / 'record_io.cpp'), '-I' + str(root / 'src')], capture_output=True, text=True, timeout=600)
    if b.returncode:
        print(b.stdout + b.stderr)
        print('FAIL: build')
        sys.exit(1)

ok = True
r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=600)
print(r.stdout.strip())
if r.returncode != 0:
    ok = False

# plant -> the check it must fail (the test prints one FAIL line per failed check)
plants = (('stale_slots', 'caster.proof'),                    # THE PAST BUG of the wrestlers' palette (bone slots past the part's list taken as rows)
          ('ordinal_merge', 'caster.marks'),                  # map A and map B keyed alike
          ('key_by_pass_wrong', 'caster.marker_bank_equal'),  # the pass dropped from the key (props map + props rim merge)
          ('camera_prev_frame', 'caster.proof'),              # the camera run one frame old
          ('props_camera_for_wrestlers', 'caster.proof'),     # the two cameras of 17.2 item 2 swapped
          ('camera_viewproj_wrong', 'caster.proof'),          # ViewProj from M(R+976)
          ('record_offset0', 'caster.proof'),                 # the records joined at hook-to-stream offset 0
          ('offset0', 'caster.marker_bank_equal'),            # the packet joined at another frame (a hole)
          ('catchup_record', 'caster.proof'),                 # SF1: a catch-up record's earlier-frame calls paired with this record's bones
          ('first_match', 'caster.proof'),                    # SF2: duplicated bones, the first match taken
          ('marker_count0', 'marker.claim_replayed'),         # lane 204's past bug: the marker claimed at replayed count 0 (the handler delivers 2)
          ('freeze_off', 'caster.slots_frozen'),            # lane 301 S3: the code before the freeze (ambiguous bones in a proof frame lose the certified slot map)
          ('caster_plane_no_guard', 'caster.plane_guard'),    # lane 299 (review SF2): no call-count guard for the plane pass on the caster route
          ('shared_globals', 'caster.isolated_from_arena'))  # lane 217 review S1: the caster chain wired to the arena's live g_packets / g_marks / g_gate / g_tally         # lane 204's past bug: the marker claimed at replayed count 0 (the handler delivers 2)
for plant, named in plants:
    p = subprocess.run([str(exe), 'plant', plant], capture_output=True, text=True, timeout=600)
    failed = [l for l in p.stdout.splitlines() if l.startswith('FAIL ') and named in l]
    caught = p.returncode == 1 and bool(failed)
    print(f'planted {plant}: {"caught by " + named if caught else "NOT CAUGHT by " + named + " (exit %d)" % p.returncode}')
    ok = ok and caught

# the real-capture camera rules (J1, run 7086: the one capture with light states); FAIL when the capture is not there after a restore attempt
rel = Path('out') / 's101' / 'lane92' / 'case_20261112_172341_872_0_pc_run_exit' / 'capture'
cands = [args.j1, str(root / rel)]
if root.parent.name == 'worktrees' and len(root.parents) > 2:   # an agent worktree: the main checkout (.claude/worktrees/<id>) holds the kept captures
    cands.append(str(root.parents[2] / rel))
j1 = next((c for c in cands if c and os.path.isfile(os.path.join(c, 'images.bin'))), '')
if not j1:   # lane 300: the unpacked capture is a cache of the kept case zip: try the restore, then FAIL (never SKIP) when it is still missing
    try:
        sys.path.insert(0, str(root / 'tools' / 'gate'))
        import inputs as _ginputs
        print('restore attempt:', _ginputs.restore_all(['j1'], quiet=True))
        j1 = next((str(_ginputs.MAIN / _ginputs.INPUTS['j1']['dest'] / 'capture') for _ in [0] if os.path.isfile(str(_ginputs.MAIN / _ginputs.INPUTS['j1']['dest'] / 'capture' / 'images.bin'))), '')
    except Exception as _e:
        print('restore attempt failed:', _e)
if j1:
    vec = root / 'out' / 'lane211' / 'cam_j1.txt'
    vec.parent.mkdir(parents=True, exist_ok=True)
    e = subprocess.run([sys.executable, '-I', str(root / 'tools' / 'caster' / 'caster_cam_vectors.py'), j1, str(vec), '--frames', '30,60,100,150,200,250,300'], capture_output=True, text=True, timeout=900)
    if e.returncode:
        print(e.stdout + e.stderr)
        print('FAIL: caster_cam_vectors.py')
        ok = False
    else:
        c = subprocess.run([str(exe), 'cam', str(vec)], capture_output=True, text=True, timeout=300)
        print(c.stdout.strip())
        ok = ok and c.returncode == 0
        p = subprocess.run([str(exe), 'cam', str(vec), 'camera_viewproj_wrong'], capture_output=True, text=True, timeout=300)
        failed = [l for l in p.stdout.splitlines() if l.startswith('FAIL ') and 'cam.props_map_viewproj_and_depth' in l]
        caught = p.returncode == 1 and bool(failed)
        print(f'planted camera_viewproj_wrong (capture): {"caught by cam.props_map_viewproj_and_depth" if caught else "NOT CAUGHT"}')
        ok = ok and caught
else:
    print('FAIL  input missing after a restore attempt (py -I tools/gate/inputs.py restore): the kept J1 capture out/s101/lane92/case_20261112_172341_872_0_pc_run_exit/capture; the camera rules against real banks were NOT checked')
    ok = False

print('PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
