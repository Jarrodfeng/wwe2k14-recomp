#!/usr/bin/env python3
"""S102 lane T11b check (offline, 0 game runs): wwe2k14_rdr_scene_strings_once (the live scene record's repeated strings made once, in a pool every frame's table starts as a copy of).

  py -I tools\\crowd\\scene_strings_check.py [--root DIR] [--syntax] [--no-records]

Default: builds tools\\crowd\\scene_strings_test.cpp (with -DRDR_CROWD_IDS_PLANTS; the planted twins exist only in that build) against the tree's own live_scene.cpp, record_io.cpp,
crowd_contrib.cpp and arena_contrib.cpp, extracts ~120 REAL recorded frames (Brock's case zip of the main checkout: capture\\scene\\frame_*.rsc, read-only, extracted into out\\t11b\\scene and
deleted after), runs the test with --records and requires: exit 0, "all checks passed", no FAIL line, every arm (the pool off vs on records equal for ids cached / classic / pose off / run markers /
flipping, a second pool, a moved table, the moved-from shell, live::EndFrame twice), the 11 planted twins caught and their 2 clean twins passing, the real-record arms, and the cost arms.
--syntax: clang -fsyntax-only of the game's translation units that include the touched headers, with the main build's flags (as crowd_ids_check.py).
A check that cannot read its input fails loudly: a missing test, a missing zip, no result line, all FAIL."""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import crowd_ids_check as base  # noqa: E402  (the T11 check: main_checkout, game flags, include walker)

CLANG = base.CLANG
TUS = ['src/main.cpp', 'src/renderer_adapter/live.cpp', 'src/scene_live/crowd_contrib.cpp', 'src/scene_live/arena_contrib.cpp', 'src/renderer_scene/record_io.cpp',
       'src/renderer_scene/live_scene.cpp', 'src/renderer_scene/crowd_ours.cpp', 'src/case/case_scene.cpp']
BROCK_ZIP = os.path.join('out', 's101', 'runs', '20261111_215201_s100_owner_s89_brock')
SOURCES = ['src/renderer_scene/live_scene.cpp', 'src/renderer_scene/record_io.cpp', 'src/scene_live/crowd_contrib.cpp', 'src/scene_live/arena_contrib.cpp']
N_PLANTS = 11
N_FRAMES = 120


def fail(msg):
    print('FAIL: ' + msg)
    return 1


def find_zip(root):
    main = base.main_checkout(root)
    if not main:
        return None
    hits = sorted(glob.glob(os.path.join(main, BROCK_ZIP, 'case_*.zip')))
    return hits[0] if hits else None


def extract_records(root, zip_path):
    out = os.path.join(root, 'out', 't11b', 'scene')
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out, exist_ok=True)
    with zipfile.ZipFile(zip_path) as z:
        names = sorted(n for n in z.namelist() if re.search(r'capture/scene/frame_\d+\.rsc$', n))
        if len(names) < N_FRAMES:
            return None, 'the zip holds only %d scene records' % len(names)
        pick = names[len(names) // 4: len(names) // 4 + N_FRAMES]
        for n in pick:
            with z.open(n) as src, open(os.path.join(out, os.path.basename(n)), 'wb') as dst:
                shutil.copyfileobj(src, dst)
    return out, ''


def run_test(root, records):
    if not os.path.isfile(CLANG):
        return fail('%s not found' % CLANG)
    src = os.path.join(root, 'tools', 'crowd', 'scene_strings_test.cpp')
    if not os.path.isfile(src):
        return fail('missing ' + src)
    out_dir = os.path.join(root, 'out', 't11b')
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, 'scene_strings_test.exe')
    if os.path.exists(exe):
        os.remove(exe)
    b = subprocess.run([CLANG, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-DRDR_CROWD_IDS_PLANTS', '-DNOMINMAX', '-o', exe, src] + [os.path.join(root, s) for s in SOURCES],
                       capture_output=True, text=True)
    if b.returncode or not os.path.isfile(exe):
        return fail('scene_strings_test did not compile\n' + b.stdout + b.stderr)
    args = [exe]
    rec_dir = None
    if records:
        zp = find_zip(root)
        if not zp:
            return fail('Brock case zip (real scene records) not found under the main checkout: %s' % BROCK_ZIP)
        rec_dir, why = extract_records(root, zp)
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
        return fail('scene_strings_test exit %d, %d FAIL lines' % (r.returncode, fails))
    if twins != N_PLANTS or len(re.findall(r'^PASS clean twin', text, re.M)) != 2:
        return fail('expected %d planted twins caught and 2 clean twins passing, got %d / %d' % (N_PLANTS, twins, len(re.findall(r'^PASS clean twin', text, re.M))))
    if len(re.findall(r'^PASS records equal \(strings compacted\)', text, re.M)) != 5:
        return fail('the 5 off-vs-on record equality arms did not all run')
    if len(re.findall(r'^PASS live::EndFrame', text, re.M)) != 2:
        return fail('the 2 live::EndFrame arms did not both run')
    if records and (not re.search(r'^PASS records equal, one field per string, pool off vs on, on \d+ recorded frames', text, re.M) or 'real frames:' not in text):
        return fail('the real-record arm did not run')
    if passes < 30:
        return fail('expected >= 30 PASS lines, got %d (a check that reads nothing proves nothing)' % passes)
    print('PASS: scene_strings_test: %d checks, %d planted twins caught, exit 0' % (passes, twins))
    return 0


def run_syntax(root):
    main = base.main_checkout(root)
    if not main or not os.path.isdir(os.path.join(main, 'generated', 'default')):
        return fail('main checkout with generated\\default not found (%s): the game units cannot be syntax-checked' % main)
    wt = os.path.abspath(root).replace('\\', '/')
    m = main.replace('\\', '/')
    inc = ['-I' + wt, '-I' + wt + '/src', '-I' + m, '-I' + m + '/src', '-I' + m + '/generated/default', '-I' + m + '/thirdparty/streamline/sdk/include',
           '-isystem', m + '/thirdparty/rexglue-sdk-vulkan/include', '-isystem', m + '/thirdparty/rexglue-sdk-vulkan/include/dxc']
    bad = 0
    for tu in TUS:
        p = os.path.join(wt, tu)
        if not os.path.isfile(p):
            return fail('missing ' + p)
        r = subprocess.run([CLANG] + base.GAME_FLAGS + inc + [p], capture_output=True, text=True)
        print('%s %s' % ('ok  ' if r.returncode == 0 else 'FAIL', tu))
        if r.returncode:
            bad += 1
            print((r.stdout + r.stderr)[:1500])
    if bad:
        return fail('%d game unit(s) do not compile' % bad)
    print("PASS: %d game translation units compile (-fsyntax-only, the main build's flags)" % len(TUS))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--syntax', action='store_true')
    ap.add_argument('--no-records', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    return run_syntax(root) if a.syntax else run_test(root, not a.no_records)


if __name__ == '__main__':
    sys.exit(main())
