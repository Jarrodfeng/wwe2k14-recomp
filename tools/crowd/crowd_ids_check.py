#!/usr/bin/env python3
"""S102 lane T11 gate check (offline, 0 game runs): wwe2k14_rdr_crowd_ids_once and wwe2k14_rdr_origin_memo.

  py -I tools\\crowd\\crowd_ids_check.py [--root DIR] [--syntax]

Default: builds tools\\crowd\\crowd_ids_test.cpp (with -DRDR_CROWD_IDS_PLANTS; the planted twins exist only in that build) against the tree's own
src\\renderer_scene\\record_io.cpp and src\\scene_live\\crowd_contrib.cpp, runs it and requires: exit 0, "all checks passed", no FAIL line, the
serialised-bytes arms (4), the allocation and speed arms, the 3 crowd planted twins and the origin-memo twin all reported PASS. It prints the numbers.
--syntax: clang -fsyntax-only of the game's translation units that include the touched headers (main.cpp, live.cpp, crowd_contrib.cpp, record_io.cpp,
crowd_ours.cpp) with the main build's flags (ninja -t commands, out\\build\\win-amd64-release), include paths pointed at this tree's src first.
A check that cannot read its input fails loudly: a missing test, a missing clang, no result line, all FAIL."""
import argparse
import fnmatch
import json
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
TUS = ['src/main.cpp', 'src/renderer_adapter/live.cpp', 'src/scene_live/crowd_contrib.cpp', 'src/renderer_scene/record_io.cpp', 'src/renderer_scene/crowd_ours.cpp']
GAME_FLAGS = ['-DNOMINMAX', '-DRDR_HAS_STREAMLINE=1', '-DREXGLUE_BUILD_CONFIG="Release"', '-DREX_HAS_D3D12=1', '-DREX_HAS_VULKAN=1', '-DSPDLOG_COMPILED_LIB',
              '-DSPDLOG_FMT_EXTERNAL', '-DWIN32_LEAN_AND_MEAN', '-DWWE2K14_HAS_ICON=1', '-msse4.1', '-D_DLL', '-D_MT', '-O1', '-std=gnu++23', '-fsyntax-only', '-Wno-everything']


def main_checkout(root):
    out = subprocess.run(['git', '-C', root, 'rev-parse', '--git-common-dir'], capture_output=True, text=True)
    if out.returncode:
        return None
    common = os.path.abspath(os.path.join(root, out.stdout.strip()))
    return os.path.dirname(common)


def fail(msg):
    print('FAIL: ' + msg)
    return 1


INC = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)


def local_includes(root, roots):
    """Every repo file reached from `roots` by quoted #include."""
    seen, todo = set(), [os.path.normpath(r) for r in roots]
    while todo:
        f = todo.pop()
        if f in seen or not os.path.isfile(f):
            continue
        seen.add(f)
        for m in INC.findall(open(f, encoding='utf-8', errors='replace').read()):
            cand = os.path.normpath(os.path.join(os.path.dirname(f), m))
            if os.path.isfile(cand) and os.path.abspath(cand).startswith(os.path.abspath(root)):
                todo.append(cand)
    return sorted(seen)


def rel_of(root, f):
    return os.path.relpath(f, root).replace('\\', '/')


def uncovered(root, files, affects):
    return [rel_of(root, f) for f in files if not any(fnmatch.fnmatch(rel_of(root, f), g) for g in affects)]


def run_lint(root):
    """F6: every file the test's build reads (the test, the sources it links, every header they include) must match this lane's `affects`, or the cache could pass a stale result."""
    lane = json.load(open(os.path.join(root, 'tools', 'gate', 'lanes', 's102_t11.json'), encoding='utf-8'))
    aff = lane['checks'][0].get('affects') or []
    if not aff:
        return fail('the lane json has no affects for check 1')
    roots = [os.path.join(root, p) for p in ('tools/crowd/crowd_ids_test.cpp', 'src/renderer_scene/record_io.cpp', 'src/scene_live/crowd_contrib.cpp', 'src/renderer_scene/live_scene.cpp')]
    files = local_includes(root, roots)
    missing = uncovered(root, files, aff)
    if missing:
        return fail("files the test builds from are not in the lane's affects: " + ', '.join(missing))
    probe = 'src/renderer_scene/scene_record.h'
    if probe not in [rel_of(root, f) for f in files]:
        return fail('lint input changed: %s is no longer reached from the test' % probe)
    twin = [g for g in aff if not fnmatch.fnmatch(probe, g)]
    if probe not in uncovered(root, files, twin):
        return fail('the affects lint did not notice a header missing from affects (twin)')
    print("PASS: affects lint: the %d files the test builds from are all covered by the lane's affects (the missing-glob twin is caught)" % len(files))
    return 0


def run_test(root):
    if not os.path.isfile(CLANG):
        return fail('%s not found' % CLANG)
    src = os.path.join(root, 'tools', 'crowd', 'crowd_ids_test.cpp')
    if not os.path.isfile(src):
        return fail('missing ' + src)
    out_dir = os.path.join(root, 'out', 't11')
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, 'crowd_ids_test.exe')
    if os.path.exists(exe):
        os.remove(exe)
    b = subprocess.run([CLANG, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-DRDR_CROWD_IDS_PLANTS', '-DRDR_ORIGIN_MEMO_PLANTS', '-DNOMINMAX', '-o', exe, src, os.path.join(root, 'src', 'renderer_scene', 'live_scene.cpp'),
                        os.path.join(root, 'src', 'renderer_scene', 'record_io.cpp'), os.path.join(root, 'src', 'scene_live', 'crowd_contrib.cpp')],
                       capture_output=True, text=True)
    if b.returncode or not os.path.isfile(exe):
        return fail('crowd_ids_test did not compile\n' + b.stdout + b.stderr)
    r = subprocess.run([exe], capture_output=True, text=True, timeout=600)
    text = r.stdout
    print(text.rstrip())
    passes = len(re.findall(r'^PASS ', text, re.M))
    fails = len(re.findall(r'^FAIL ', text, re.M))
    twins = len(re.findall(r'^PASS planted twin', text, re.M))
    if r.returncode != 0 or fails or 'all checks passed' not in text:
        return fail('crowd_ids_test exit %d, %d FAIL lines' % (r.returncode, fails))
    if passes < 26 or twins != 11:
        return fail('expected >= 26 PASS lines and 11 planted twins caught, got %d and %d (a check that reads nothing proves nothing)' % (passes, twins))
    if len(re.findall(r'^PASS EndFrame with a contributor before and after the crowd', text, re.M)) != 2:
        return fail('the 2 mixed-record (live::EndFrame) arms did not both run')
    rc = run_lint(root)
    if rc:
        return rc
    if len(re.findall(r'^PASS serialised record bytes equal', text, re.M)) != 4:
        return fail('the 4 byte-equality arms did not all run')
    print('PASS: crowd_ids_test: %d checks, %d planted twins caught, exit 0' % (passes, twins))
    return 0


def run_syntax(root):
    main = main_checkout(root)
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
        r = subprocess.run([CLANG] + GAME_FLAGS + inc + [p], capture_output=True, text=True)
        print('%s %s' % ('ok  ' if r.returncode == 0 else 'FAIL', tu))
        if r.returncode:
            bad += 1
            print((r.stdout + r.stderr)[:1500])
    if bad:
        return fail('%d game unit(s) do not compile' % bad)
    print('PASS: %d game translation units compile (-fsyntax-only, the main build\'s flags)' % len(TUS))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--syntax', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    return run_syntax(root) if a.syntax else run_test(root)


if __name__ == '__main__':
    sys.exit(main())
