# Lane 229 (Ring D3 R3, INERT; docs/renderer/SKIP_GAME_WORK_MAP.md 16.8): builds tools/d3/d3_ring_test.cpp (the shipped d3_binder.h / d3_record.h in the ring class's route, a synthetic ring-rows record) and runs it:
#   the honest arm must pass (exit 0); every planted fault must FAIL the check named below (exit 1). 0 game runs.
#   py -I tools\d3\d3_ring_selftest.py [--no-build]      ~1 min. Output: out/tools/d3_ring_test.exe (gitignored).
import argparse, subprocess, sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
exe = root / 'out' / 'tools' / 'd3_ring_test.exe'
clang = r'C:\Program Files\LLVM\bin\clang++.exe'

ap = argparse.ArgumentParser()
ap.add_argument('--no-build', action='store_true')
args = ap.parse_args()

if not args.no_build:
    exe.parent.mkdir(parents=True, exist_ok=True)
    b = subprocess.run([clang, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Wno-unused-parameter', '-D_CRT_SECURE_NO_WARNINGS', '-o', str(exe), str(root / 'tools' / 'd3' / 'd3_ring_test.cpp'),
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
plants = (('stale_slots_counted', 'ring.marks'),            # THE PAST BUG: the bone slots past a part's palette composed like any other (the don't-care cut off): the rope is never marked
          ('dedupe_model_r5', 'ring.marks'),                # the rows joined by (frame, model, r5) like the arena tracker: the rope's three calls collapse to one entry
          ('record_offset0', 'ring.marks'),                 # the records joined at hook-to-stream offset 0
          ('late_record', 'ring.marks'),                    # record i reaches the binder at frame i + 1 (the live pairing as it was): nothing may be marked
          ('offset0', 'ring.marker_bank_equal'),            # the packet joined at another frame (a hole)
          ('canvas_marked', 'ring.compose_registers'),      # the canvas' own registers c230-c237 (no source) taken from the template and compared: the proof compare sees them
          ('alpha_zero', 'ring.compose_registers'),         # c185 MdlAlpha taken as zero where the record route would source it
          ('lod_flip_ordinal', 'ring.guard'),               # the call-count guard off: the rope stays marked across an LOD flip
          ('shared_globals', 'ring.isolated_from_arena'))   # the ring chain wired to the arena's live g_packets / g_marks / g_gate / g_tally
for plant, named in plants:
    p = subprocess.run([str(exe), 'plant', plant], capture_output=True, text=True, timeout=600)
    failed = [l for l in p.stdout.splitlines() if l.startswith('FAIL ') and named in l]
    caught = p.returncode == 1 and bool(failed)
    print(f'planted {plant}: {"caught by " + named if caught else "NOT CAUGHT by " + named + " (exit %d)" % p.returncode}')
    if not caught:
        print(p.stdout.strip())
    ok = ok and caught
print('PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
