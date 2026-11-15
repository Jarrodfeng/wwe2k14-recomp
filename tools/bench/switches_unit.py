"""S102 S0-4: builds and runs the C++ unit tests of the switch work (no game, no SDK):
  - src\\renderer_adapter\\rdr_switches_test.cpp: the switch table on a fake settings store (change counter gate, one log line, generation, reader
    acknowledgement, start-only refusal). Twin: the driver forgets the counter bump after the flip -> must FAIL.
  - src\\renderer_adapter\\lasting_prune_test.cpp: includes the S0-4 cases (the image bookkeeping keeps running while the switch is off, the catch-up
    debt is cleared). Twin: lasting_prune.h with the OLD `if (by_image)` around the bookkeeping -> must FAIL.
  py -I tools\\bench\\switches_unit.py
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
RA = os.path.join(ROOT, 'src', 'renderer_adapter')


def build(args, exe):
    b = subprocess.run([CLANG, '-Wall', '-Wextra'] + args + ['-o', exe], capture_output=True, text=True)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return False
    return True


def main():
    if not os.path.isfile(CLANG):
        print('FAIL: %s not found' % CLANG)
        return 1
    out_dir = os.path.join(ROOT, 'out', 's0_4')
    os.makedirs(out_dir, exist_ok=True)
    # 1. the table
    exe = os.path.join(out_dir, 'rdr_switches_test.exe')
    if not build(['-std=c++17', '-Werror', '-I', RA, os.path.join(RA, 'rdr_switches_test.cpp')], exe):
        return 1
    ok = subprocess.run([exe], capture_output=True, text=True)
    print('switch table: ' + ok.stdout.strip().split('\n')[-1])
    if ok.returncode:
        print(ok.stdout)
        return 1
    if subprocess.run([exe, 'plant'], capture_output=True, text=True).returncode == 0:
        print('FAIL: the planted fault (no counter bump after the flip) was not caught')
        return 1
    print('switch table twin: planted fault caught')
    # 2. the prune bookkeeping
    exe2 = os.path.join(out_dir, 'lasting_prune_test.exe')
    if not build(['-std=c++20', '-I', os.path.join(ROOT, 'src'), '-I', RA, os.path.join(RA, 'lasting_prune_test.cpp')], exe2):
        return 1
    ok = subprocess.run([exe2], capture_output=True, text=True)
    print('lasting_prune: ' + ok.stdout.strip().split('\n')[-1])
    if ok.returncode:
        print(ok.stdout)
        return 1
    twin_dir = os.path.join(out_dir, 'twin', 'renderer_adapter')
    os.makedirs(twin_dir, exist_ok=True)
    src = open(os.path.join(RA, 'lasting_prune.h'), encoding='utf-8').read()
    marker = '    {   // S102 S0-4 (J)'
    if marker not in src:
        print('FAIL: the S0-4 (J) marker is gone from lasting_prune.h; the twin cannot be made')
        return 1
    start = src.index(marker)
    end = src.index('\n', start)
    open(os.path.join(twin_dir, 'lasting_prune.h'), 'w', encoding='utf-8').write(src[:start] + '    if (by_image) {' + src[end:])
    open(os.path.join(twin_dir, 'lasting_prune_test.cpp'), 'w', encoding='utf-8').write(open(os.path.join(RA, 'lasting_prune_test.cpp'), encoding='utf-8').read())
    exe3 = os.path.join(out_dir, 'lasting_prune_twin.exe')
    if not build(['-std=c++20', '-I', os.path.join(out_dir, 'twin'), '-I', os.path.join(ROOT, 'src'), '-I', RA, os.path.join(twin_dir, 'lasting_prune_test.cpp')], exe3):
        return 1
    if subprocess.run([exe3], capture_output=True, text=True).returncode == 0:
        print('FAIL: the old `if (by_image)` bookkeeping was not caught by lasting_prune_test')
        return 1
    print('lasting_prune twin: the old behaviour is caught')
    return 0


if __name__ == '__main__':
    sys.exit(main())
