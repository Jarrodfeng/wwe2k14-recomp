"""Lane 190 (S101): runs out/tools/copy_alias_test.exe on the shipped rule (must pass) and once per planted fault (each must FAIL, exit 1).
A planted fault that passes means the test cannot see that fault (LESSONS: a positive control). Exit 0 = the real run passes and every plant fails.
The `outlive` plant lives in the binder (the alias kept into the next frame), not in the planner: fview's proof catches it (tools/skip/copy_alias_check.py)."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / 'out' / 'tools' / 'copy_alias_test.exe'
PLANTS = ['empty', 'src_dirty', 'sub_rect', 'format', 'read_first', 'late', 'no_draw_writes']

if not EXE.exists():
    print(f'FAIL: {EXE} does not exist (build it: tools/skip/build_copy_alias_test.ps1)')
    sys.exit(2)
bad = 0
r = subprocess.run([str(EXE)], capture_output=True, text=True, timeout=60)
print(r.stdout.strip())
if r.returncode != 0:
    print('FAIL: the shipped rule fails its own test')
    bad += 1
for p in PLANTS:
    r = subprocess.run([str(EXE), '--plant', p], capture_output=True, text=True, timeout=60)
    if r.returncode == 1:
        first = next((l for l in r.stdout.splitlines() if l.startswith('FAIL')), '')
        print(f'plant {p}: caught (exit 1): {first[:110]}')
    else:
        print(f'FAIL plant {p}: exit {r.returncode}, the test did not catch it')
        bad += 1
print('copy_alias_selftest:', 'PASS' if not bad else f'{bad} problem(s)')
sys.exit(1 if bad else 0)
