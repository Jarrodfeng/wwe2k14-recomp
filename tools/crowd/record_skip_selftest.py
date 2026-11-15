"""S102 lane UQ1: runs out/tools/record_skip_test.exe on the shipped logic (must pass) and once per planted fault (each must FAIL, exit 1).
A planted fault that passes means the test cannot see that fault. Exit 0 = the real run passes and every plant fails."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / 'out' / 'tools' / 'record_skip_test.exe'
PLANTS = ['hook_tiled_first', 'tail_not_noted', 'skip_first', 'skip_proof', 'skip_unproven', 'tiled_ignored', 'bit_ignored', 'no_pair', 'peek_counts', 'mode_census', 'member_pair_lost', 'member_tail_not_noted', 'member_class_ignored', 'keep_forced_drop', 'regs_ignored', 'member_regs_ignored', 'member_keep_forced_drop']

if not EXE.exists():
    print(f'FAIL: {EXE} does not exist (build it: tools/crowd/build_record_skip_test.ps1)')
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
print('record_skip_selftest:', 'PASS' if not bad else f'{bad} problem(s)')
sys.exit(1 if bad else 0)
