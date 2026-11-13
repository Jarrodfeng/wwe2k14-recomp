"""Lane 234: runs out\tools\crowd_rank_test.exe (build_crowd_rank_test.ps1) clean (must pass), then each plant (no_track, no_swap; each must exit 1)."""
import subprocess
import sys
from pathlib import Path

EXE = Path(__file__).resolve().parents[2] / 'out' / 'tools' / 'crowd_rank_test.exe'
if not EXE.exists():
    print('FAIL: %s does not exist (build it: tools/crowd/build_crowd_rank_test.ps1)' % EXE)
    sys.exit(1)
bad = 0
r = subprocess.run([str(EXE)], capture_output=True, text=True)
print(r.stdout.strip().splitlines()[-1])
if r.returncode != 0:
    print(r.stdout)
    bad += 1
for plant in ('no_track', 'no_swap', 'no_guard'):
    r = subprocess.run([str(EXE), '--plant', plant], capture_output=True, text=True)
    print('plant %s: %s (exit %d)' % (plant, 'caught' if r.returncode == 1 else 'NOT CAUGHT', r.returncode))
    bad += r.returncode != 1
print('crowd_rank_selftest: %s' % ('FAIL' if bad else 'PASS'))
sys.exit(1 if bad else 0)
