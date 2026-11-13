# S101 lane 122 (KR): builds tools/thin/thin_unit_test.cpp (the shipped thin_draw / reader_thin / thin_store / span_timer headers) and runs it: the honest arm must pass (exit 0),
# every planted fault must FAIL it (exit 1). Output: out/tools/thin_unit_test.exe (gitignored).
import subprocess, sys
from pathlib import Path
root = Path(__file__).resolve().parents[2]
exe = root / 'out' / 'tools' / 'thin_unit_test.exe'
exe.parent.mkdir(parents=True, exist_ok=True)
clang = r'C:\Program Files\LLVM\bin\clang++.exe'
b = subprocess.run([clang, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Wno-unused-parameter', '-o', str(exe), str(root / 'tools' / 'thin' / 'thin_unit_test.cpp')], capture_output=True, text=True, timeout=300)
if b.returncode:
    print(b.stdout + b.stderr)
    print('FAIL: build')
    sys.exit(1)
ok = True
r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=600)
print(r.stdout.strip())
if r.returncode != 0:
    ok = False
for plant in ('release_without_ack', 'stale_generation', 'descriptor_stride_64', 'object_sibling', 'no_fill', 'reader_only_fill'):
    p = subprocess.run([str(exe), 'plant', plant], capture_output=True, text=True, timeout=600)
    caught = p.returncode == 1
    why = [l for l in p.stdout.splitlines() if l.startswith('FAIL')]
    print(f'planted {plant}: {"caught (exit 1): " + (why[0] if why else "?") if caught else "NOT CAUGHT, exit %d" % p.returncode}')
    ok = ok and caught
print('PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
