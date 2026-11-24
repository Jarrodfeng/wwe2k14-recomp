#!/usr/bin/env python3
"""S102 lane SW1 check (offline, 0 game runs): wwe2k14_rdr_swap_once.

  py -I tools\\swap\\swap_once_check.py [--root DIR] [--no-records]

Builds tools\\swap\\swap_once_test.cpp (-DRDR_SWAP_PLANTS -DRDR_CROWD_IDS_PLANTS: the planted twins exist only in that build) against the tree's live_scene.cpp, record_io.cpp,
crowd_contrib.cpp and arena_contrib.cpp, extracts ~120 REAL recorded frames (Brock's case zip of the main checkout, read-only, extracted under out\\sw1 and deleted after), runs it and
requires: exit 0, "all checks passed", no FAIL line, the record-equality arms, the planted twins (8: pool gives a table back uncleared, Recycle leaves two members, Recycle keeps the
seed link, overlay ignores the shown list, cache keyed by raw address is fooled, two crowd fast-path plants, and Recycle leaving two members: the every-section arm) and the clean twin, the real-record arms.
A check that cannot read its input fails loudly."""
import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'crowd'))
import scene_strings_check as ssc  # noqa: E402  (the T11b check: zip finder, record extractor)

SOURCES = ['src/renderer_scene/live_scene.cpp', 'src/renderer_scene/record_io.cpp', 'src/scene_live/crowd_contrib.cpp', 'src/scene_live/arena_contrib.cpp']


def fail(msg):
    print('FAIL: ' + msg)
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--no-records', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    clang = ssc.CLANG
    if not os.path.isfile(clang):
        return fail('%s not found' % clang)
    src = os.path.join(root, 'tools', 'swap', 'swap_once_test.cpp')
    if not os.path.isfile(src):
        return fail('missing ' + src)
    out_dir = os.path.join(root, 'out', 'sw1')
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, 'swap_once_test.exe')
    if os.path.exists(exe):
        os.remove(exe)
    b = subprocess.run([clang, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-DRDR_SWAP_PLANTS', '-DRDR_CROWD_IDS_PLANTS', '-DNOMINMAX', '-o', exe, src] + [os.path.join(root, s) for s in SOURCES],
                       capture_output=True, text=True)
    if b.returncode or not os.path.isfile(exe):
        return fail('swap_once_test did not compile\n' + b.stdout + b.stderr)
    args = [exe]
    rec_dir = None
    if not a.no_records:
        zp = ssc.find_zip(root)
        if not zp:
            return fail('Brock case zip (real scene records) not found under the main checkout')
        # extract under out\sw1 (the T11b helper writes under out\t11b: use it, then move)
        rec_dir, why = ssc.extract_records(root, zp)
        if not rec_dir:
            return fail(why)
        args += ['--records', rec_dir]
    try:
        r = subprocess.run(args, capture_output=True, text=True, timeout=900)
    finally:
        if rec_dir:
            shutil.rmtree(rec_dir, ignore_errors=True)
    text = r.stdout
    print(text.rstrip())
    passes = len(re.findall(r'^PASS ', text, re.M))
    fails = len(re.findall(r'^FAIL ', text, re.M))
    twins = len(re.findall(r'^PASS planted twin', text, re.M))
    if r.returncode != 0 or fails or 'all checks passed' not in text:
        return fail('swap_once_test exit %d, %d FAIL lines' % (r.returncode, fails))
    if twins != 8 or len(re.findall(r'^PASS clean twin', text, re.M)) != 1:
        return fail('expected 8 planted twins caught and 1 clean twin passing, got %d / %d' % (twins, len(re.findall(r'^PASS clean twin', text, re.M))))
    if len(re.findall(r'^PASS records equal \(strings compacted\), switch off vs', text, re.M)) != 7:
        return fail('the 7 off-vs-on record equality arms did not all run')
    if not a.no_records and 'real frames:' not in text:
        return fail('the real-record arm did not run')
    if passes < 45:
        return fail('expected >= 45 PASS lines, got %d (a check that reads nothing proves nothing)' % passes)
    print('PASS: swap_once_test: %d checks, %d planted twins caught, exit 0' % (passes, twins))
    return 0


if __name__ == '__main__':
    sys.exit(main())
