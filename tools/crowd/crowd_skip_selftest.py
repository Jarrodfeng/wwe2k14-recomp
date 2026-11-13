"""Lane 50 (S101): runs out/tools/crowd_skip_test.exe on the shipped logic (must pass) and once per planted fault (each must FAIL, exit 1).
A planted fault that passes means the test cannot see that fault (LESSONS: a positive control). Exit 0 = real passes and every plant fails."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / 'out' / 'tools' / 'crowd_skip_test.exe'
PLANTS = ['ignore_kept', 'ignore_stale', 'drop_trigger', 'ignore_reads', 'card_without_ours', 'no_clean_wait', 'wrong_ring', 'unknown_group_skips',
          'scene_crowd_open', 'stale_map', 'ignore_census']

if not EXE.exists():
    print(f'FAIL: {EXE} does not exist (build it: tools/crowd/build_crowd_skip_test.ps1)')
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
print('crowd_skip_selftest:', 'PASS' if not bad else f'{bad} problem(s)')
sys.exit(1 if bad else 0)
