# Lane 260 (LEAVE_THE_COMMAND_STREAM 3e, section 10): builds tools/owner/timing_core_test.cpp (the shipped src/owner_timing_core.h) and runs it: the honest arm must pass (exit 0),
# every planted fault must FAIL the check named for it (exit 1). Output: out/tools/timing_core_test.exe (gitignored).
import subprocess, sys
from pathlib import Path
root = Path(__file__).resolve().parents[2]
exe = root / 'out' / 'tools' / 'timing_core_test.exe'
exe.parent.mkdir(parents=True, exist_ok=True)
clang = r'C:\Program Files\LLVM\bin\clang++.exe'
b = subprocess.run([clang, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Wno-unused-parameter', '-o', str(exe), str(root / 'tools' / 'owner' / 'timing_core_test.cpp')], capture_output=True, text=True, timeout=300)
if b.returncode:
    print(b.stdout + b.stderr)
    print('FAIL: build')
    sys.exit(1)
ok = True
r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
print(r.stdout.strip())
if r.returncode != 0:
    ok = False
for plant, named in (('release_early', 'gate.held_behind_flip'), ('flip_never_cleared', 'gate.released_by_vblank'), ('flip_never_cleared', 'waitword.ticks_vblank'),
                     ('missed_vblank', 'vblank.count_over_second'), ('wait_no_tick', 'waitword.ticks_vblank'), ('fixed_ignored', 'scene.fixed_value_reported'),
                     ('regrid_every_vblank', 'vblank.count_over_second')):
    p = subprocess.run([str(exe), 'plant', plant], capture_output=True, text=True, timeout=120)
    failed = [l for l in p.stdout.splitlines() if l.startswith('FAIL ') and named in l]
    caught = p.returncode == 1 and bool(failed)
    print(f'planted {plant}: {"caught by " + named if caught else "NOT CAUGHT by " + named + " (exit %d)" % p.returncode}')
    ok = ok and caught
print('PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
