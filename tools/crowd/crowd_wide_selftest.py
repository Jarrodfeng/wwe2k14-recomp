"""Lane 176 (S101): runs out/tools/crowd_wide_test.exe on the shipped logic (must pass) and once per planted fault (each must FAIL, exit 1).
A planted fault that passes means the test cannot see that fault (LESSONS: a positive control). Exit 0 = real passes and every plant fails."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / 'out' / 'tools' / 'crowd_wide_test.exe'
PLANTS = ['old_16_9', 'no_x', 'no_radius', 'wrong_axis', 'row_not_column', 'section_no_radius', 'section_wrong_col', 'floor_wrong_col', 'record_raw', 'old_not_off', 'stage_no_x', 'stage_old', 'stage_wrong_axis']

if not EXE.exists():
    print(f'FAIL: {EXE} does not exist (build it: tools/crowd/build_crowd_wide_test.ps1)')
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
        print(f'plant {p}: caught (exit 1)')
    else:
        print(f'FAIL plant {p}: exit {r.returncode}, the test did not catch it')
        bad += 1
print('crowd_wide_selftest:', 'PASS' if not bad else f'{bad} problem(s)')
sys.exit(1 if bad else 0)
