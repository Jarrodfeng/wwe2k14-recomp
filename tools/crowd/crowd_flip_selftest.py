"""S110 lane CROWDFLIPS: builds and runs out/tools/crowd_flip_test.exe on the shipped rule (must pass) and once per planted twin (each must FAIL, exit 1).
A planted fault that passes means the test cannot see that fault. Exit 0 = real passes and every plant fails."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / 'out' / 'tools' / 'crowd_flip_test.exe'
PLANTS = ['flip_back_counts_any_change', 'culled_counts_as_flip']

b = subprocess.run(['pwsh', '-NoProfile', '-File', str(ROOT / 'tools' / 'crowd' / 'build_crowd_flip_test.ps1')], capture_output=True, text=True, timeout=600)
if b.returncode != 0 or not EXE.exists():
    print(b.stdout, b.stderr)
    print(f'FAIL: could not build {EXE}')
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
        print(f'plant {p}: caught (exit 1)')
    else:
        print(f'FAIL plant {p}: exit {r.returncode}, the test did not catch it')
        bad += 1
print('crowd_flip_selftest:', 'PASS' if not bad else f'{bad} problem(s)')
sys.exit(1 if bad else 0)
