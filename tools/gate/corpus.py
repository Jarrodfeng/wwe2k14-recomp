r"""Gate check (S101, arc tool 7 "the replay farm", offline form): plays every capture in the MAIN checkout's out\captures with this
checkout's rplay under validation; any capture that fails or reports validation errors fails the check, by name.
  py -I tools\gate\corpus.py [--list]
A capture is a folder holding images.bin and frames\. Pictures go to the main checkout's out\gate\corpus\ (game imagery: local only).
"""
import os, subprocess, sys
from pathlib import Path

here = Path(__file__).resolve().parents[2]
main = Path(os.environ.get('RDR_MAIN_CHECKOUT', here))
exe = here / 'out' / 'renderer' / 'dev' / 'rplay.exe'
root = main / 'out' / 'captures'
caps = sorted(p for p in root.iterdir() if (p / 'images.bin').exists() and (p / 'frames').is_dir()) if root.is_dir() else []
if '--list' in sys.argv:
    print('\n'.join(p.name for p in caps))
    sys.exit(0)
if not exe.exists() or not caps:
    print(f'FAIL corpus: {"no rplay at " + str(exe) if not exe.exists() else "no captures in " + str(root)}')
    sys.exit(1)
out = main / 'out' / 'gate' / 'corpus'
out.mkdir(parents=True, exist_ok=True)
bad = []
for cap in caps:
    p = subprocess.run([str(exe), '--capture', str(cap), '--validation', '1', '--out', str(out / f'{cap.name}.png')], capture_output=True,
                       text=True, timeout=600)
    if p.returncode != 0:
        bad.append(f'{cap.name} (exit {p.returncode})')
print(f'corpus: {len(caps) - len(bad)} of {len(caps)} captures replay clean under validation' + (': FAIL ' + ', '.join(bad) if bad else ''))
sys.exit(1 if bad else 0)
