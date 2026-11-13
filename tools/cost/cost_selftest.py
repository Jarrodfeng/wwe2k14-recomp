# S101 lane 83: builds tools/cost/cost_counters_test.cpp (the shipped src/cost_counters.h) and runs it: the real arm must pass (exit 0) and every
# planted wiring fault must FAIL it (exit 1). Output: out/tools/cost_counters_test.exe (gitignored).
import subprocess, sys
from pathlib import Path
root = Path(__file__).resolve().parents[2]
exe = root / 'out' / 'tools' / 'cost_counters_test.exe'
exe.parent.mkdir(parents=True, exist_ok=True)
clang = r'C:\Program Files\LLVM\bin\clang++.exe'
b = subprocess.run([clang, '-std=c++20', '-O1', '-Wall', '-o', str(exe), str(root / 'tools' / 'cost' / 'cost_counters_test.cpp')],
                   capture_output=True, text=True, timeout=300)
if b.returncode:
    print(b.stdout + b.stderr)
    print('FAIL: build')
    sys.exit(1)
ok = True
r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
lines = r.stdout.strip().splitlines()
print(lines[-1] if lines else '(no output)')
if r.returncode != 0:
    print(r.stdout)
    ok = False
for fault in ('wrong_class', 'wrong_replay_op', 'reader_to_record', 'skip_op'):
    p = subprocess.run([str(exe), 'fault', fault], capture_output=True, text=True, timeout=120)
    caught = p.returncode == 1
    print(f'planted twin {fault}: {"caught (exit 1)" if caught else "NOT CAUGHT, exit %d" % p.returncode}')
    ok = ok and caught
# Overhead microbenchmark (lane 94): the shipped replay counter at 1/16 must cost well under the lane 83 version per record (best of 5 passes each).
bexe = root / 'out' / 'tools' / 'cost_overhead_bench.exe'
bb = subprocess.run([clang, '-std=c++20', '-O2', '-o', str(bexe), str(root / 'tools' / 'cost' / 'cost_overhead_bench.cpp')], capture_output=True, text=True, timeout=300)
if bb.returncode:
    print(bb.stdout + bb.stderr)
    print('FAIL: bench build')
    sys.exit(1)
br = subprocess.run([str(bexe), '4000000'], capture_output=True, text=True, timeout=300)
print(br.stdout.strip())
import re
m = re.search(r'instrument cost ns/record \(on - off\): legacy ([-\d.]+) shipped-1/16 ([-\d.]+)', br.stdout)
if not m or not float(m.group(2)) < 0.6 * float(m.group(1)):
    print('bench: shipped 1/16 is not clearly cheaper than the lane 83 counter')
    ok = False
print('PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
