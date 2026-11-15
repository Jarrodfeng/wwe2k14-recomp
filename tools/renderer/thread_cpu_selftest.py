# S102 S0-3: builds tools/renderer/thread_cpu_test.cpp (the shipped src/renderer/thread_cpu.h) and runs it: the honest arm passes, the planted "wall time instead of CPU" fault must FAIL
# the sleep check. Output: out/tools/thread_cpu_test.exe (gitignored).
import subprocess, sys
from pathlib import Path
root = Path(__file__).resolve().parents[2]
exe = root / 'out' / 'tools' / 'thread_cpu_test.exe'
exe.parent.mkdir(parents=True, exist_ok=True)
clang = r'C:\Program Files\LLVM\bin\clang++.exe'
b = subprocess.run([clang, '-std=c++20', '-O1', '-Wall', '-Wextra', '-o', str(exe), str(root / 'tools' / 'renderer' / 'thread_cpu_test.cpp')], capture_output=True, text=True, timeout=300)
if b.returncode:
    print(b.stdout + b.stderr)
    print('FAIL: build')
    sys.exit(1)
ok = True
for attempt in range(3):  # a loaded PC can preempt a spin: the honest arm gets three tries
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    if r.returncode == 0:
        break
print(r.stdout.strip())
ok = r.returncode == 0
p = subprocess.run([str(exe), 'plant', 'wall'], capture_output=True, text=True, timeout=60)
failed = [l for l in p.stdout.splitlines() if l.startswith('FAIL ') and 'sleep_40ms_reads_no_cpu' in l]
caught = p.returncode == 1 and bool(failed)
print('planted wall-time meter: ' + ('caught by sleep_40ms_reads_no_cpu' if caught else 'NOT CAUGHT (exit %d)' % p.returncode))
ok = ok and caught
print('PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
