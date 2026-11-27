#!/usr/bin/env python3
"""S111 lane FXSKIP check: builds tools/scene/record_kinds_unit.cpp with src/renderer_scene/record_io.cpp (-DRDR_RECORD_KIND_PLANTS), runs it, then its
planted twins (each must exit 1 and fail its NAMED check), then the wiring text checks (every kind-tagged section reader calls KeepKnownKinds; no reader
spells a kind range of its own).
  py -I tools/scene/record_kinds_check.py [--root DIR]
Exit 0 = pass, 1 = fail. 0 game runs, no game files, no SDK."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {'drop_section': 'fx.known_rows_kept', 'keep_unknown': 'fx.unknown_skipped'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's111', 'fxskip')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'record_kinds_unit.exe')
    srcs = [os.path.join(root, 'tools', 'scene', 'record_kinds_unit.cpp'), os.path.join(root, 'src', 'renderer_scene', 'record_io.cpp')]
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-g', '-O1', '-D_CRT_SECURE_NO_WARNINGS',
             '-DRDR_RECORD_KIND_PLANTS', '-fuse-ld=lld']
    b = subprocess.run([CLANG] + flags + srcs + ['-o', exe], capture_output=True, text=True, cwd=out)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    r = subprocess.run([exe], capture_output=True, text=True, cwd=out)
    print(r.stdout.strip())
    if r.returncode:
        print('FAIL: the unit test failed\n' + r.stderr)
        return 1
    bad = 0
    for plant, name in PLANTS.items():
        p = subprocess.run([exe, '--plant', plant], capture_output=True, text=True, cwd=out)
        failed = [ln for ln in p.stdout.splitlines() if ln.startswith('FAIL')]
        ok = p.returncode == 1 and any(name in ln for ln in failed)
        print('twin %-14s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', name))
        bad += 0 if ok else 1

    with open(os.path.join(root, 'src', 'renderer_scene', 'record_io.cpp'), encoding='utf-8') as f:
        rio = f.read()
    need = [
        'KeepKnownKinds(tmp, kAwObject, kAwLast, out.arena_world_errors)',
        'KeepKnownKinds(tmp, kMlSet, kMlLast, out.c_errors)',
        'KeepKnownKinds(tmp, kFxScreen, kFxLast, out.c_errors)',
        'KeepKnownKinds(tmp, kSlEntrance, kSlLast, out.c_errors)',
        'KeepKnownKinds(tmp, kCpSign, kCpLast, out.c_errors)',
        'KeepKnownKinds(tmp, kCsBody, kCsLast, out.c_errors)',
    ]
    for needle in need:
        ok = needle in rio
        print('%s  wire record_io.cpp calls %s' % ('PASS' if ok else 'FAIL', needle))
        bad += 0 if ok else 1
    gone = ['r.kind <= kFx', 'r.kind <= kMlMesh', 'r.kind <= kCpHand', 'w.kind == kAwClock', 'r.kind == 1;', 'r.kind <= kSlLast']
    for needle in gone:
        ok = needle not in rio
        print('%s  wire no reader-side kind range: %s' % ('PASS' if ok else 'FAIL', needle))
        bad += 0 if ok else 1
    print('record_kinds_check: %s' % ('PASS' if bad == 0 else 'FAIL (%d)' % bad))
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
