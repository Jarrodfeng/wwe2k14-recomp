#!/usr/bin/env python3
r"""S103 PW integration check (offline, 0 game runs): builds tools\weapons\weapon_combined_test.cpp (the real PW-1 and PW-2 contributors -> SceneTables -> record file -> the drawer's view and plan) and
runs it clean, then each planted twin, which must exit 1 AND fail its own named check.
  py -I tools\weapons\weapon_combined_check.py [--root DIR]
Exit 0 = the clean run passes and every plant fails the check it names. A check that cannot build or read its input fails loudly."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = [('no_frame', 'combined.old_frame_not_drawn'), ('no_id_at_call', 'combined.two_chairs_once_each'), ('drawer_ignore_frame', 'combined.old_frame_not_drawn'),
          ('count_every_submit', 'combined.other_pass_not_recorded'), ('drawer_secondary_as_main', 'combined.secondary_with_id'), ('drawer_merge', 'combined.two_chairs_once_each')]


def run(cmd, timeout=300):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    root = os.path.abspath(ap.parse_args().root)
    if not os.path.isfile(CLANG):
        print('FAIL: %s not found' % CLANG)
        return 1
    src = os.path.join(root, 'tools', 'weapons', 'weapon_combined_test.cpp')
    out_dir = os.path.join(root, 'out', 'pwi')
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, 'weapon_combined_test.exe')
    if os.path.exists(exe):
        os.remove(exe)
    rc, out = run([CLANG, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-Werror', '-DNOMINMAX', '-D_CRT_SECURE_NO_WARNINGS', '-o', exe, src,
                   os.path.join(root, 'src', 'renderer_scene', 'live_scene.cpp'), os.path.join(root, 'src', 'renderer_scene', 'record_io.cpp')], 900)
    if rc or not os.path.isfile(exe):
        print('FAIL: weapon_combined_test did not compile\n' + out[-3000:])
        return 1
    bad = 0
    rc, out = run([exe])
    print(out.rstrip())
    if rc != 0 or re.search(r'^FAIL ', out, re.M) or len(re.findall(r'^PASS ', out, re.M)) < 8:
        print('FAIL  clean arm (exit %d)' % rc)
        bad += 1
    for plant, check in PLANTS:
        rc, pout = run([exe, '--plant', plant])
        failed = {ln.split()[1] for ln in pout.splitlines() if ln.startswith('FAIL ')}
        ok = rc == 1 and check in failed
        print('%s  twin %-20s exit %d, names %s, fails %s' % ('PASS' if ok else 'FAIL', plant, rc, check, ', '.join(sorted(failed)) or '(nothing)'))
        bad += 0 if ok else 1
    print('weapon_combined_check: %s' % ('ALL PASS' if not bad else '%d FAILED' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
