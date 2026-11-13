"""Lane 175 (S101): runs out/tools/member_thin_test.exe on the shipped logic (must pass) and once per planted fault (each must FAIL, exit 1).
A planted fault that passes means the test cannot see that fault (LESSONS: a positive control). Exit 0 = real passes and every plant fails."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / 'out' / 'tools' / 'member_thin_test.exe'
PLANTS = ['run_per_frame', 'calls_collapsed', 'drop_first', 'no_palette_dropped', 'tiled_double', 'stale_ignored', 'slack_ignored', 'no_latch', 'hole_ignored', 'skipped_as_hole', 'proven_per_card', 'no_proof', 'stamp_ignored', 'weak_check', 'floor_site_dropped_unobserved', 'drop_all', 'refused_as_fullbad', 'repeated_run_boundary', 'stamp_offset0', 'elsewhere_ignored', 'miss_ignored', 'ambiguous_counts', 'stamp_unproven_dropped', 'verify_blind', 'judge_count_only', 'stale_streak_kept']

if not EXE.exists():
    print(f'FAIL: {EXE} does not exist (build it: tools/crowd/build_member_thin_test.ps1)')
    sys.exit(2)
bad = 0
r = subprocess.run([str(EXE)], capture_output=True, text=True, timeout=60)
print(r.stdout.strip())
if r.returncode != 0:
    print('FAIL: the shipped logic fails its own test')
    bad += 1
for p in PLANTS:
    r = subprocess.run([str(EXE), '--plant', p], capture_output=True, text=True, timeout=60)
    if r.returncode == 1:
        print(f'plant {p}: caught (exit 1): {r.stdout.splitlines()[0][:100] if r.stdout else ""}')
    else:
        print(f'FAIL plant {p}: exit {r.returncode}, the test did not catch it')
        bad += 1
print('member_thin_selftest:', 'PASS' if not bad else f'{bad} problem(s)')
sys.exit(1 if bad else 0)
