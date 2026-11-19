r"""Lane IXF twin (Session 103): the binder's geometry size guard on the real bad capture (offline rplay, 0 game runs).

  py -I tools\scene\ixf_rplay_check.py [--capture NAME] [--plant]

Plays out\captures\a30_match (frame 0 names another capture's geometry offsets: docs\research\s103\IX_index_overrun.md) WITHOUT dropping frame 0, under validation,
with Problems non-fatal. With the guard in the binder: 0 validation errors (exit 0) and >= 1 'geometry_offset_size_mismatch' Problem lines.
--plant: the same check on the old behaviour is not available as a switch; the planted twin is the unit test (geo_size_guard_test.exe --plant). This script only
fails when the capture is missing, rplay exits non-zero, or no mismatch Problem is logged.
"""
import os
import subprocess
import sys
from pathlib import Path

here = Path(__file__).resolve().parents[2]
main = Path(os.environ.get('RDR_MAIN_CHECKOUT', here))
name = 'a30_match'
if '--capture' in sys.argv:
    name = sys.argv[sys.argv.index('--capture') + 1]
exe = here / 'out' / 'renderer' / 'dev' / 'rplay.exe'
cap = main / 'out' / 'captures' / name
out = main / 'out' / 'gate' / f'ixf_{name}.png'
for p in (exe, cap):
    if not p.exists():
        print(f'FAIL missing {p}')
        sys.exit(1)
out.parent.mkdir(parents=True, exist_ok=True)
p = subprocess.run([str(exe), '--capture', str(cap), '--validation', '1', '--problems-nonfatal', '--out', str(out)], capture_output=True, text=True, timeout=280)
text = p.stdout + p.stderr
mismatch = text.count('geometry_offset_size_mismatch')
print(f'rplay {name}: exit {p.returncode}, geometry_offset_size_mismatch lines {mismatch}')
if p.returncode != 0:
    print('\n'.join(text.strip().splitlines()[-6:]))
    print('FAIL: rplay exit non-zero (validation errors or a crash)')
    sys.exit(1)
if mismatch < 1:
    print('FAIL: no geometry_offset_size_mismatch Problem: the guard did not fire on the bad capture')
    sys.exit(1)
print('ixf rplay check: PASS')
