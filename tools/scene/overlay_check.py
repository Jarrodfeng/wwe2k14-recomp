"""S103 AIM: the stream-free build's ImGui overlay path, proven offline (0 game runs, no game files).
Answers: does an OverlayList left in the static mailbox (what overlay_off.h's Drawer does) reach the picture the REAL SceneHost presents, and does the planted fault
(--plant overlay_never: the mailbox never emptied) fail the check?
  py tools\\scene\\overlay_check.py            (needs out\\tools\\host_rig.exe: pwsh tools\\scene\\build_host_rig.ps1)
Exit 0 = the clean run PASSes and the planted run FAILs."""
import pathlib
import subprocess
import sys

exe = pathlib.Path(__file__).resolve().parents[2] / 'out' / 'tools' / 'host_rig.exe'
if not exe.exists():
    sys.exit('overlay_check: build %s first (tools\\scene\\build_host_rig.ps1)' % exe)


def run(*extra):
    p = subprocess.run([str(exe), '--mode', 'overlay', *extra], capture_output=True, text=True, timeout=300)
    out = p.stdout + p.stderr
    return p.returncode, [l for l in out.splitlines() if l.startswith(('overlay ', 'host_rig: FAIL'))]


bad = 0
rc, lines = run()
print('clean run: rc', rc, *lines, sep='\n  ')
if rc != 0 or not any(l == 'overlay result: PASS' for l in lines):
    bad += 1
rc, lines = run('--plant', 'overlay_never')
print('planted fault (overlay_never): rc', rc, *lines, sep='\n  ')
if rc == 0:
    print('overlay_check: the planted fault did NOT fail the check')
    bad += 1
print('overlay_check:', 'FAIL' if bad else 'PASS')
sys.exit(1 if bad else 0)
