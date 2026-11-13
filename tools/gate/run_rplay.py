r"""Gate helper (S101): plays one capture of the main checkout with this checkout's rplay under validation; fails loudly on any error.
  py -I tools\gate\run_rplay.py CAPTURE_NAME
rplay comes from <this checkout>\out\renderer\dev\rplay.exe (built by core.renderer_build); the capture from the MAIN checkout's
out\captures\ (RDR_MAIN_CHECKOUT, set by gate.py). The picture goes to the main checkout's out\gate\.
"""
import os, subprocess, sys
from pathlib import Path

here = Path(__file__).resolve().parents[2]
main = Path(os.environ.get('RDR_MAIN_CHECKOUT', here))
name = sys.argv[1]
exe = here / 'out' / 'renderer' / 'dev' / 'rplay.exe'
cap = main / 'out' / 'captures' / name
out = main / 'out' / 'gate' / f'{name}.png'
for p in (exe, cap):
    if not p.exists():
        print(f'FAIL missing {p}')
        sys.exit(1)
out.parent.mkdir(parents=True, exist_ok=True)
p = subprocess.run([str(exe), '--capture', str(cap), '--validation', '1', '--out', str(out)], capture_output=True, text=True, timeout=280)
lines = (p.stdout + p.stderr).strip().splitlines()
if p.returncode != 0:
    print('\n'.join(lines[-6:]))
    print(f'FAIL rplay {name}: exit {p.returncode}')
    sys.exit(1)
if not out.exists():
    print(f'FAIL rplay {name}: no picture written')
    sys.exit(1)
print(f'rplay {name}: exit 0, picture {out.name} ({out.stat().st_size} bytes)')
