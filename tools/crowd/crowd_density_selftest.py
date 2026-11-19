r"""S103 SPBF review M1: runs out\tools\crowd_density_test.exe (build_crowd_density_test.ps1) clean (must pass), then the plant old_early_return (the first stream-free rule: cards ignored the density; must exit 1)."""
import subprocess
import sys
from pathlib import Path

EXE = Path(__file__).resolve().parents[2] / 'out' / 'tools' / 'crowd_density_test.exe'
if not EXE.exists():
    print('FAIL: %s does not exist (build it: tools/crowd/build_crowd_density_test.ps1)' % EXE)
    sys.exit(1)
bad = 0
r = subprocess.run([str(EXE)], capture_output=True, text=True)
print(r.stdout.strip())
if r.returncode != 0:
    bad += 1
r = subprocess.run([str(EXE), '--plant', 'old_early_return'], capture_output=True, text=True)
print('plant old_early_return: %s (exit %d)' % ('caught' if r.returncode == 1 else 'NOT CAUGHT', r.returncode))
bad += r.returncode != 1
print('crowd_density_selftest: %s' % ('FAIL' if bad else 'PASS'))
sys.exit(1 if bad else 0)
