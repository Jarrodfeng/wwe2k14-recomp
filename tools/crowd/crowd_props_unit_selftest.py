#!/usr/bin/env python3
"""Lane 133: builds and runs tools\\crowd\\crowd_props_unit.cpp (the shipped join on synthetic prop sets, no game files) and its planted twins.
  py -I tools\\crowd\\crowd_props_unit_selftest.py [--root DIR]
The clean run must exit 0; count_every_candidate (F1's old counting), normal_tol0 and normal_flip must each exit 1."""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
PLANTS = ('count_every_candidate', 'normal_tol0', 'normal_flip')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    root = os.path.abspath(ap.parse_args().root)
    clang = r'C:\Program Files\LLVM\bin\clang++.exe'
    vk = sorted(os.listdir(r'C:\VulkanSDK')) if os.path.isdir(r'C:\VulkanSDK') else []
    if not os.path.isfile(clang) or not vk:
        print('FAIL  clang++ or the Vulkan SDK headers are missing')
        return 1
    out_dir = os.path.join(root, 'out', 'tools')
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, 'crowd_props_unit.exe')
    p = subprocess.run([clang, '-std=c++20', '-O1', '-w', '-I' + os.path.join(r'C:\VulkanSDK', vk[-1], 'Include'), '-DNOMINMAX', '-D_CRT_SECURE_NO_WARNINGS',
                        os.path.join(root, 'tools', 'crowd', 'crowd_props_unit.cpp'), '-o', exe], capture_output=True, text=True, timeout=600)
    if p.returncode:
        print(p.stdout + p.stderr)
        print('FAIL  build')
        return 1
    bad = 0
    r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    ok = r.returncode == 0 and 'crowd_props_unit: PASS' in r.stdout
    print(('PASS' if ok else 'FAIL') + '  clean: ' + r.stdout.strip().splitlines()[-1])
    bad += not ok
    for plant in PLANTS:
        r = subprocess.run([exe, plant], capture_output=True, text=True, timeout=60)
        ok = r.returncode == 1
        print(('PASS' if ok else 'FAIL') + '  planted %s fails (exit %d)' % (plant, r.returncode))
        bad += not ok
    print('crowd_props_unit_selftest: %s' % ('PASS' if not bad else 'FAIL (%d)' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
