"""S103 VTG2: runs out/tools/vintage_look_test.exe on the shipped logic (must pass) and once per planted fault (each must FAIL, exit 1)."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / 'out' / 'tools' / 'vintage_look_test.exe'
PLANTS = ['ignore_flag', 'always_skip', 'no_swap']
if not EXE.exists():
    print(f'FAIL: {EXE} does not exist (build it: tools/vintage/build_vintage_look_test.ps1)')
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
        print(f'FAIL plant {p}: exit {r.returncode}, not caught')
        bad += 1
print('vintage_look_selftest:', 'PASS' if not bad else f'{bad} problem(s)')
sys.exit(1 if bad else 0)
