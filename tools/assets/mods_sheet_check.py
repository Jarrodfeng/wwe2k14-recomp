#!/usr/bin/env python3
"""S110 TRONSHEET review: builds tools\\assets\\mods_sheet_unit.cpp (the viewer's sheet picture, src\\assets\\mods_lookup.h ModSheet:
a custom tron slot is a sheet with no original; its default "original" strips come out black, never read through the missing
original), runs it. Host only: no game, no video decoder.
  py -I tools\\assets\\mods_sheet_check.py [--root DIR]
Exit 0 = pass, 1 = fail.
Planted twin (run once at the review, S110): the pre-fix ModSheet (strips copied from `original` whenever the banner is "original")
crashed on case 1 (a null original) instead of printing its rows."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's110', 'tronsheet')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'mods_sheet_unit.exe')
    src = os.path.join(root, 'tools', 'assets', 'mods_sheet_unit.cpp')
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-fuse-ld=lld']
    b = subprocess.run([CLANG] + flags + [src, '-o', exe], capture_output=True, text=True, cwd=out)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    if b.stderr.strip():
        print(b.stderr.strip())
    r = subprocess.run([exe], capture_output=True, text=True, cwd=out)
    print(r.stdout.strip())
    if r.returncode or 'RESULT OK' not in r.stdout:
        print('FAIL: the unit failed (exit %d)%s' % (r.returncode, (': ' + r.stderr.strip()) if r.stderr.strip() else ''))
        return 1
    print('mods_sheet_check PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
