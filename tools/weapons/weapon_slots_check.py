#!/usr/bin/env python3
"""S103 lane PW-1 gate check (offline, 0 game runs): builds tools\\weapons\\weapon_slots_test.cpp against the tree's src\\scene_live\\weapon_mgr_reader.h / weapon_contrib.h and
src\\renderer_scene\\record_io.cpp (+ live_scene.cpp) and runs it: the slot reader on forged guest memory (literal offsets), the cap, the reload, the contributor, section 59 through
SerializeRecord / ParseRecord, and 9 planted twins (4 shifted offsets, a 16-byte secondary stride changed to 4, the vtable constant, k < 4 (the first contract),the cap not applied, the manager floor) that must each be caught.
  py -I tools\\weapons\\weapon_slots_check.py [--root DIR]
Exit 0 = pass. A check that cannot read its input fails loudly (no result line, a missing test, no compiler)."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'


def fail(msg):
    print('FAIL: ' + msg)
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    root = os.path.abspath(ap.parse_args().root)
    if not os.path.isfile(CLANG):
        return fail('%s not found' % CLANG)
    src = os.path.join(root, 'tools', 'weapons', 'weapon_slots_test.cpp')
    if not os.path.isfile(src):
        return fail('missing ' + src)
    out_dir = os.path.join(root, 'out', 'pw1')
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, 'weapon_slots_test.exe')
    if os.path.exists(exe):
        os.remove(exe)
    b = subprocess.run([CLANG, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-DNOMINMAX', '-o', exe, src, os.path.join(root, 'src', 'renderer_scene', 'live_scene.cpp'),
                        os.path.join(root, 'src', 'renderer_scene', 'record_io.cpp')], capture_output=True, text=True)
    if b.returncode or not os.path.isfile(exe):
        return fail('weapon_slots_test did not compile\n' + b.stdout + b.stderr)
    r = subprocess.run([exe], capture_output=True, text=True, timeout=300)
    text = r.stdout
    print(text.rstrip())
    passes = len(re.findall(r'^PASS ', text, re.M))
    fails = len(re.findall(r'^FAIL ', text, re.M))
    twins = len(re.findall(r'^PASS planted twin', text, re.M))
    if r.returncode != 0 or fails or 'all checks passed' not in text:
        return fail('weapon_slots_test exit %d, %d FAIL lines' % (r.returncode, fails))
    if passes < 20 or twins != 9:
        return fail('expected >= 20 PASS lines and 9 planted twins caught, got %d and %d (a check that reads nothing proves nothing)' % (passes, twins))
    print('PASS: weapon_slots_test: %d checks, %d planted twins caught, exit 0' % (passes, twins))
    return 0


if __name__ == '__main__':
    sys.exit(main())
