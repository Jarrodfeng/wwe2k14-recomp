# lane 147: builds + runs render_signal_test.cpp (the shipped src/render_signal.h), then checks the long owner route still waits on the signal.
import os, subprocess, sys
from pathlib import Path
root = Path(__file__).resolve().parents[2]
exe = root / 'out' / 'tools' / 'render_signal_test.exe'
exe.parent.mkdir(parents=True, exist_ok=True)
import shutil
cxx = shutil.which('clang++') or r'C:\Program Files\LLVM\bin\clang++.exe'   # standalone LLVM (tools\env.ps1 puts it first)
if not Path(cxx).exists():
    print('FAIL: no clang++ found'); sys.exit(2)
r = subprocess.run([cxx, '-std=c++20', '-O1', '-Wall', '-Wextra', str(root / 'tools/render_signal/render_signal_test.cpp'), '-o', str(exe)], capture_output=True, text=True)
if r.returncode:
    print(r.stdout + r.stderr); sys.exit(2)
r = subprocess.run([str(exe)], capture_output=True, text=True)
print(r.stdout.strip())
if r.returncode:
    sys.exit(3)
route = (root / 'tools/input_scripts/scenes/s101_owner_match_long.txt').read_text()
if 'wait_event render roster_ready' not in route:
    print('FAIL: the long owner route does not wait on roster_ready'); sys.exit(4)
print('render_signal selftest: PASS')
