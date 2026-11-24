#!/usr/bin/env python3
"""S102 lane T14b check (offline, 0 game runs): wwe2k14_rdr_origin_index and wwe2k14_rdr_shader_serial.

  py -I tools\\draw_index\\draw_index_check.py [--root DIR] [--syntax] [--build-only]

Default: builds tools\\draw_index\\draw_index_test.cpp against the SHIPPED src\\renderer_adapter\\origin_intern.h and draw_memos.h (with the plant macros) and runs it:
origin index == map path for every id (garbage, wrong check bits, unknown ids, the bound, chunk edges), two tables / a table made again, a writer with two readers, the
census counts; shader epoch rule == the plain whole-microcode path over 120k draws with microcode rewritten at epoch boundaries, a head rewritten inside an epoch seen,
the accepted in-epoch tail case counted. Then the 5 planted faults (`test plant N`) must each make it exit 1 with a FAIL line (a plant that no longer applies is its
own failure). Then lint_reader: the reader.h glue.
--syntax: clang -fsyntax-only of main.cpp and live.cpp with the main build's flags (the game compiles Windows headers fview / rplay do not).
A check that cannot read its input fails loudly."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
TUS = ['src/main.cpp', 'src/renderer_adapter/live.cpp']
GAME_FLAGS = ['-DNOMINMAX', '-DRDR_HAS_STREAMLINE=1', '-DREXGLUE_BUILD_CONFIG="Release"', '-DREX_HAS_D3D12=1', '-DREX_HAS_VULKAN=1', '-DSPDLOG_COMPILED_LIB',
              '-DSPDLOG_FMT_EXTERNAL', '-DWIN32_LEAN_AND_MEAN', '-DWWE2K14_HAS_ICON=1', '-msse4.1', '-D_DLL', '-D_MT', '-O1', '-std=gnu++23', '-fsyntax-only', '-Wno-everything']
PLANTS = [1, 2, 3, 4, 5]
PLANT_TEXT = {1: ('src/renderer_adapter/origin_intern.h', 'RDR_IDX_PLANT(1)'), 2: ('src/renderer_adapter/origin_intern.h', 'RDR_IDX_PLANT(2)'),
              3: ('src/renderer_adapter/origin_intern.h', 'RDR_IDX_PLANT(3)'), 4: ('src/renderer_adapter/draw_memos.h', 'T14B_PLANT(1)'),
              5: ('src/renderer_adapter/draw_memos.h', 'T14B_PLANT(2)')}


def main_checkout(root):
    out = subprocess.run(['git', '-C', root, 'rev-parse', '--git-common-dir'], capture_output=True, text=True)
    if out.returncode:
        return None
    return os.path.dirname(os.path.abspath(os.path.join(root, out.stdout.strip())))


def fail(msg):
    print('FAIL: ' + msg)
    return 1


def build(root):
    if not os.path.isfile(CLANG):
        return None, fail('%s not found' % CLANG)
    src = os.path.join(root, 'tools', 'draw_index', 'draw_index_test.cpp')
    if not os.path.isfile(src):
        return None, fail('missing ' + src)
    out_dir = os.path.join(root, 'out', 't14b')
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, 'draw_index_test.exe')
    if os.path.exists(exe):
        os.remove(exe)
    b = subprocess.run([CLANG, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-DRDR_ORIGIN_INDEX_PLANTS', '-DRDR_ORIGIN_MEMO_PLANTS', '-DT14B_PLANTS', '-DNOMINMAX',
                        '-o', exe, src], capture_output=True, text=True)
    if b.returncode or not os.path.isfile(exe):
        return None, fail('draw_index_test did not compile\n' + b.stdout + b.stderr)
    return exe, 0


def run_test(root, build_only=False):
    exe, rc = build(root)
    if rc:
        return rc
    if build_only:
        print('built ' + exe)
        return 0
    r = subprocess.run([exe], capture_output=True, text=True, timeout=600)
    text = r.stdout
    print(text.rstrip())
    passes = len(re.findall(r'^PASS ', text, re.M))
    fails = len(re.findall(r'^FAIL ', text, re.M))
    if r.returncode != 0 or fails or 'all checks passed' not in text:
        return fail('draw_index_test exit %d, %d FAIL lines' % (r.returncode, fails))
    if passes < 7:
        return fail('expected >= 7 PASS lines, got %d (a check that reads nothing proves nothing)' % passes)
    for needle in ('origin: the index answers what the map answers', 'origin past-bug re-find', 'shaders: the info every draw gets equals', 'origin census'):
        if needle not in text:
            return fail('the arm "%s" did not run' % needle)
    if lint_reader(root):
        return 1
    caught = []
    for n in PLANTS:
        path, needle = PLANT_TEXT[n]
        if needle not in open(os.path.join(root, path), encoding='utf-8').read():   # a plant that no longer applies is its own failure
            return fail('planted fault %d DID NOT APPLY: %s has no %s' % (n, path, needle))
        p = subprocess.run([exe, 'plant', str(n)], capture_output=True, text=True, timeout=600)
        nfail = len(re.findall(r'^FAIL ', p.stdout, re.M))
        if p.returncode != 1 or nfail < 1:
            return fail('planted fault %d was NOT caught (exit %d, %d FAIL lines)' % (n, p.returncode, nfail))
        caught.append('%d (%d FAIL)' % (n, nfail))
    print('PASS planted twins: all %d caught: plant %s' % (len(PLANTS), ', '.join(caught)))
    print('PASS: draw_index_test: %d checks, %d planted twins caught, exit 0' % (passes, len(PLANTS)))
    return 0


def lint_reader(root):
    """The reader.h glue the offline test cannot reach: both getters read the epoch once and pass it to Find AND Remember; the switches are set every frame; the census names exist."""
    text = open(os.path.join(root, 'src', 'renderer_adapter', 'reader.h'), encoding='utf-8').read()
    checks = [('vertex Find takes the epoch', 'g_vs_memo.Find(memo_key, source, serial_epoch)'), ('pixel Find takes the epoch', 'g_ps_memo.Find(memo_key, source, serial_epoch)'),
              ('vertex Remember takes the epoch', 'g_vs_memo.Remember(memo_key, source, info, serial_epoch)'), ('pixel Remember takes the epoch', 'g_ps_memo.Remember(memo_key, source, found, serial_epoch)'),
              ('origin index switch read every frame', 'g_index_on.store(rdr_sw::BoolSeen(rdr_sw::kOriginIndex'), ('shader serial switch read every frame', 'g_shader_serial_on.store(rdr_sw::BoolSeen(rdr_sw::kShaderSerial'),
              ('origin census line', '[rdr][origin_index]'), ('shader census line', '[rdr][shader_serial]')]
    for what, pat in checks:
        if pat not in text:
            return fail('lint DID NOT APPLY / glue missing: %s (%s)' % (what, pat))
    if text.count('g_shader_repeat_epoch : 0;') != 2:
        return fail('lint: the two shader getters must each read g_shader_repeat_epoch once (found %d)' % text.count('g_shader_repeat_epoch : 0;'))
    sw = open(os.path.join(root, 'src', 'renderer_adapter', 'rdr_switches.h'), encoding='utf-8').read()
    for name in ('wwe2k14_rdr_origin_index', 'wwe2k14_rdr_shader_serial'):
        if not re.search(r'"%s", PerFrame, 0\)' % name, sw):
            return fail('lint: %s is not a PerFrame default-off switch in rdr_switches.h' % name)
    print('PASS lint: reader.h glue (the epoch is read once per getter and reaches Find and Remember, switches read every frame, census lines present, both switches PerFrame default off)')
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
    print("PASS: %d game translation units compile (-fsyntax-only, the main build's flags)" % len(TUS))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--syntax', action='store_true')
    ap.add_argument('--build-only', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    return run_syntax(root) if a.syntax else run_test(root, a.build_only)


if __name__ == '__main__':
    sys.exit(main())
