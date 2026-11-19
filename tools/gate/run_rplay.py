r"""Gate helper (S101): plays one capture of the main checkout with this checkout's rplay under validation; fails loudly on any error.
  py -I tools\gate\run_rplay.py CAPTURE_NAME
rplay comes from <this checkout>\out\renderer\dev\rplay.exe (built by core.renderer_build); the capture from the MAIN checkout's
out\captures\ (RDR_MAIN_CHECKOUT, set by gate.py). The picture goes to THIS checkout's out\gate\ (S103 GX: it used to go to the main checkout's
out\gate\<name>.png, one path shared by every worktree gating the same capture at the same time: a concurrent gate could read another gate's picture and
turn a FAIL into a PASS). RDR_GATE_PICTURE_DIR overrides the folder.
"""
import os, subprocess, sys
from pathlib import Path


def picture_path(here, name, env=None):
    env = os.environ if env is None else env
    # S103 PS2: gate.py gives every check its own folder per checkout (GATE_CHECK_DIR); two checks of one gate (now side by side) never share a picture path
    d = (Path(env['RDR_GATE_PICTURE_DIR']) if env.get('RDR_GATE_PICTURE_DIR')
         else Path(env['GATE_CHECK_DIR']) if env.get('GATE_CHECK_DIR') else Path(here) / 'out' / 'gate')
    return d / f'{name}.png'


def main():
    here = Path(__file__).resolve().parents[2]
    main_co = Path(os.environ.get('RDR_MAIN_CHECKOUT', here))
    name = sys.argv[1]
    exe = here / 'out' / 'renderer' / 'dev' / 'rplay.exe'
    cap = main_co / 'out' / 'captures' / name
    out = picture_path(here, name)
    for p in (exe, cap):
        if not p.exists():
            print(f'FAIL missing {p}')
            return 1
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        out.unlink()   # a picture left by an earlier run must never be read as this run's
    p = subprocess.run([str(exe), '--capture', str(cap), '--validation', '1', '--out', str(out)], capture_output=True, text=True, timeout=280)
    lines = (p.stdout + p.stderr).strip().splitlines()
    if p.returncode != 0:
        print('\n'.join(lines[-6:]))
        print(f'FAIL rplay {name}: exit {p.returncode}')
        return 1
    if not out.exists():
        print(f'FAIL rplay {name}: no picture written')
        return 1
    print(f'rplay {name}: exit 0, picture {out.name} ({out.stat().st_size} bytes)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
