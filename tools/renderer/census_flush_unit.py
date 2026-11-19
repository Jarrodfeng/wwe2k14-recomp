r"""S102 CEN1: builds and runs src\renderer_adapter\census_flush_test.cpp (no game, no SDK): the generation helper and its marker line. The planted twin (`plant`: a site that forgets its
generation) must FAIL.   py -I tools\renderer\census_flush_unit.py"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
RA = os.path.join(ROOT, 'src', 'renderer_adapter')


def main():
    out = os.path.join(ROOT, 'out', 'cen1')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'census_flush_test.exe')
    b = subprocess.run([CLANG, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', RA, os.path.join(RA, 'census_flush_test.cpp'), '-o', exe], capture_output=True, text=True)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    r = subprocess.run([exe], capture_output=True, text=True)
    print(r.stdout.strip().split('\n')[-1])
    if r.returncode:
        print(r.stdout)
        return 1
    if subprocess.run([exe, 'plant'], capture_output=True, text=True).returncode == 0:
        print('FAIL: the planted twin (a site that forgets its generation) was not caught')
        return 1
    print('census_flush twin: planted fault caught')
    return 0


if __name__ == '__main__':
    sys.exit(main())
