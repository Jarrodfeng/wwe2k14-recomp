r"""S102 T4 check (offline, 0 game runs): builds tools\fb\fb_test.cpp (clang, AddressSanitizer) four ways and runs each.
  py -I tools\fb\fb_selftest.py [--root DIR]
  real                         must print FB PASS (ids equal legacy, hash well spread, storage kept)
  plant lastword               a hash that ignores the last word: must FAIL on full hash collisions
  plant lastword + nocmp       plus equality trusting the key: must FAIL on an id that differs from legacy
A twin that passes, or a real build that fails, fails this script. A missing compiler fails loudly."""
import argparse, os, shutil, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument('--root', default=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')))
root = ap.parse_args().root
clang = shutil.which('clang++') or r'C:\Program Files\LLVM\bin\clang++.exe'
if not os.path.exists(clang) and not shutil.which('clang++'):
    print('FAIL no clang++ found'); sys.exit(1)
out = os.path.join(root, 'out', 'tools')
os.makedirs(out, exist_ok=True)
src = os.path.join(root, 'tools', 'fb', 'fb_test.cpp')

asan_dir = None
for d in sorted(__import__('glob').glob(r'C:\Program Files\LLVM\lib\clang\*\lib\windows')):
    if os.path.exists(os.path.join(d, 'clang_rt.asan_dynamic-x86_64.dll')):
        asan_dir = d
if asan_dir is None:
    print('FAIL no AddressSanitizer runtime found under the LLVM install (lib\\clang\\*\\lib\\windows)'); sys.exit(1)

def build_run(name, defs):
    exe = os.path.join(out, f'fb_test_{name}.exe')
    p = subprocess.run([clang, '-O2', '-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-fuse-ld=lld', '-fsanitize=address', '-g'] + defs + [src, '-o', exe], capture_output=True, text=True, timeout=600)
    if p.returncode:
        print(p.stdout[-1500:], p.stderr[-2500:]); print(f'FAIL build {name}'); sys.exit(1)
    env = dict(os.environ, PATH=asan_dir + os.pathsep + os.environ.get('PATH', ''))
    r = subprocess.run([exe], capture_output=True, text=True, timeout=900, env=env)
    return r.returncode, r.stdout + r.stderr

rc, text = build_run('real', [])
print(text.strip())
if rc != 0 or 'FB PASS' not in text:
    print('FAIL the real build did not pass'); sys.exit(1)
rc, text = build_run('plant_lastword', ['-DRDR_FB_PLANT_LASTWORD'])
if rc == 0 or 'full 64-bit hash collisions' not in text:
    print(text[-1500:]); print('FAIL plant lastword was not caught by the collision check'); sys.exit(1)
print('plant lastword: caught (full 64-bit hash collisions)')
rc, text = build_run('plant_nocmp', ['-DRDR_FB_PLANT_LASTWORD', '-DRDR_FB_PLANT_NOCMP'])
if rc == 0 or 'an id differs' not in text:
    print(text[-1500:]); print('FAIL plant lastword+nocmp was not caught by the id check'); sys.exit(1)
print('plant lastword + nocmp: caught (an id differs between legacy and fast)')
rc, text = build_run('plant_noclamp', ['-DRDR_PACK_PLANT_NOCLAMP'])
if rc == 0 or 'AddressSanitizer' not in text:
    print(text[-1500:]); print('FAIL plant noclamp (the unclamped bools copy) was not caught by AddressSanitizer'); sys.exit(1)
print('plant noclamp: caught (AddressSanitizer: heap-buffer-overflow on a no-fragment Variant{})')
print('OK fb_selftest')
