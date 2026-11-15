# S102 lane H2: builds tools/h2/h2_waits_test.cpp (the SHIPPED src/owner_hires_wait.h, src/owner_timing_core.h, src/renderer/fence_wait.h) and runs it: the honest arm must pass
# (exit 0), every planted twin must FAIL the check named for it (exit 1). Windows only (the test says so and passes elsewhere). Output: out/tools/h2_waits_test.exe (gitignored).
import subprocess, sys
from pathlib import Path
root = Path(__file__).resolve().parents[2]
exe = root / 'out' / 'tools' / 'h2_waits_test.exe'
exe.parent.mkdir(parents=True, exist_ok=True)
clang = r'C:\Program Files\LLVM\bin\clang++.exe'
b = subprocess.run([clang, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Wno-unused-parameter', '-o', str(exe), str(root / 'tools' / 'h2' / 'h2_waits_test.cpp')], capture_output=True, text=True, timeout=300)
if b.returncode:
    print(b.stdout + b.stderr)
    print('FAIL: build')
    sys.exit(1)
ok = True
r = subprocess.run([str(exe), '3'], capture_output=True, text=True, timeout=400)
print(r.stdout.strip())
if r.returncode != 0:
    ok = False
for plant, named in (('window_5ms', 'vblank.lateness_in_tolerance'), ('no_catchup_raise', 'vblank.count_kept'), ('slice_5ms', 'fence.lateness_in_tolerance'), ('early_return', 'fence.never_early'), ('catchup_always', 'plan.catchup_60hz_unchanged'), ('gate_ignored', 'plan.no_batch_while_flip_pending')):
    p = subprocess.run([str(exe), 'plant', plant, '2'], capture_output=True, text=True, timeout=400)
    failed = [l for l in p.stdout.splitlines() if l.startswith('FAIL ') and named in l]
    caught = p.returncode == 1 and bool(failed)
    print(f'planted {plant}: {"caught by " + named if caught else "NOT CAUGHT by " + named + " (exit %d)" % p.returncode}')
    if not caught:
        print(p.stdout)
    ok = ok and caught
print('PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
