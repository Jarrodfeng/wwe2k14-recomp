#!/usr/bin/env python3
"""S102 lane T14 check (offline, 0 game runs): wwe2k14_rdr_note_memo, wwe2k14_rdr_shader_memo, wwe2k14_rdr_pin_hash.

  py -I tools\\draw_memos\\draw_memos_check.py [--root DIR] [--syntax]

Default: builds tools\\draw_memos\\draw_memos_test.cpp (against the SHIPPED src\\renderer_adapter\\{draw_memos,thin_draw,reader_thin}.h, with -DT14_PLANTS) and
runs it. Requires: exit 0, "all checks passed", no FAIL line, the byte-equality arms (notes, shaders, pin set; off vs on and flipping every block) all PASS,
and each of the 6 planted faults (`draw_memos_test plant N`) makes the test exit 1 with at least one FAIL line (a plant that no longer applies is its own failure).
--syntax: clang -fsyntax-only of the game's translation units that include the touched headers (main.cpp, live.cpp) with the main build's flags, include paths
pointed at this tree's src first (the game compiles the Windows headers fview / rplay do not: S102 rule).
A check that cannot read its input fails loudly: a missing test, a missing clang, no result line, all FAIL."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
TUS = ['src/main.cpp', 'src/renderer_adapter/live.cpp']
GAME_FLAGS = ['-DNOMINMAX', '-DRDR_HAS_STREAMLINE=1', '-DREXGLUE_BUILD_CONFIG="Release"', '-DREX_HAS_D3D12=1', '-DREX_HAS_VULKAN=1', '-DSPDLOG_COMPILED_LIB',
              '-DSPDLOG_FMT_EXTERNAL', '-DWIN32_LEAN_AND_MEAN', '-DWWE2K14_HAS_ICON=1', '-msse4.1', '-D_DLL', '-D_MT', '-O1', '-std=gnu++23', '-fsyntax-only', '-Wno-everything']
PLANTS = [1, 2, 3, 4, 5, 6, 7]


def main_checkout(root):
    out = subprocess.run(['git', '-C', root, 'rev-parse', '--git-common-dir'], capture_output=True, text=True)
    if out.returncode:
        return None
    common = os.path.abspath(os.path.join(root, out.stdout.strip()))
    return os.path.dirname(common)


def fail(msg):
    print('FAIL: ' + msg)
    return 1


def run_test(root):
    if not os.path.isfile(CLANG):
        return fail('%s not found' % CLANG)
    main = main_checkout(root)
    xxh = os.path.join(main or '', 'thirdparty', 'rexglue-sdk', 'include', 'xxhash.h')
    if not main or not os.path.isfile(xxh):
        return fail('xxhash.h not found under the main checkout (%s)' % xxh)
    src = os.path.join(root, 'tools', 'draw_memos', 'draw_memos_test.cpp')
    if not os.path.isfile(src):
        return fail('missing ' + src)
    out_dir = os.path.join(root, 'out', 't14')
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, 'draw_memos_test.exe')
    if os.path.exists(exe):
        os.remove(exe)
    b = subprocess.run([CLANG, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-DT14_PLANTS', '-DNOMINMAX', '-I', os.path.dirname(xxh), '-o', exe, src],
                       capture_output=True, text=True)
    if b.returncode or not os.path.isfile(exe):
        return fail('draw_memos_test did not compile\n' + b.stdout + b.stderr)
    r = subprocess.run([exe], capture_output=True, text=True, timeout=600)
    text = r.stdout
    print(text.rstrip())
    passes = len(re.findall(r'^PASS ', text, re.M))
    fails = len(re.findall(r'^FAIL ', text, re.M))
    if r.returncode != 0 or fails or 'all checks passed' not in text:
        return fail('draw_memos_test exit %d, %d FAIL lines' % (r.returncode, fails))
    if passes < 14:
        return fail('expected >= 14 PASS lines, got %d (a check that reads nothing proves nothing)' % passes)
    for needle in ('notes: census windows and seen set equal', 'shaders: the info every draw gets equal', "pin hash: the thin query's answers"):
        if needle not in text:
            return fail('the arm "%s" did not run' % needle)
    if lint_reader(root):
        return 1
    caught = []
    header = open(os.path.join(root, 'src', 'renderer_adapter', 'draw_memos.h'), encoding='utf-8').read()
    for n in PLANTS:
        if 'T14_PLANT(%d)' % n not in header:   # a plant that no longer applies is its own failure
            return fail('planted fault %d DID NOT APPLY: draw_memos.h has no T14_PLANT(%d)' % (n, n))
        p = subprocess.run([exe, 'plant', str(n)], capture_output=True, text=True, timeout=600)
        nfail = len(re.findall(r'^FAIL ', p.stdout, re.M))
        if p.returncode != 1 or nfail < 1:
            return fail('planted fault %d was NOT caught (exit %d, %d FAIL lines)' % (n, p.returncode, nfail))
        caught.append('%d (%d FAIL)' % (n, nfail))
    print('PASS planted twins: all %d caught: plant %s' % (len(PLANTS), ', '.join(caught)))
    print('PASS: draw_memos_test: %d checks, %d planted twins caught, exit 0' % (passes, len(PLANTS)))
    return 0


def lint_reader(root):
    """F4: the reader.h glue the offline test cannot reach: every census-window clear is preceded by the memo's Invalidate(); both shader keys are made by MakeKey
    (object, decl, code_addr, bytes, seven_c, strides in one call); the create paths remember only bytes that still hash to the key."""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
    from read_unit import read_path, locate  # a split file is read with its parts expanded; a hit names its part file and line
    text = read_path(os.path.join(root, 'src', 'renderer_adapter', 'reader.h'))
    lines = text.splitlines()
    clears = [i for i, l in enumerate(lines) if re.search(r'\bwindow\.(clear|erase)\(', l)]
    if not clears:
        return fail('lint DID NOT APPLY: no census window clear / erase found in reader.h')
    for i in clears:
        if not any('g_note_memo.Invalidate()' in lines[j] for j in range(max(0, i - 3), i)):
            return fail('%s clears / erases a census window without g_note_memo.Invalidate() just before it' % locate(root, 'src/renderer_adapter/reader.h', i + 1))
    for what, pat in (('vertex', r'ShaderTable<VertexShaderInfo>::MakeKey\(vs, key\.decl, code_addr, bytes, key\.seven_c != 0, strides\)'),
                      ('pixel', r'ShaderTable<PixelShaderInfo>::MakeKey\(ps, 0, code_addr, bytes, false, nullptr\)')):
        if not re.search(pat, text):
            return fail('lint: the %s shader memo key is not made by MakeKey with all its fields (DID NOT APPLY: the call is gone)' % what)
    if text.count('Xxh3(source, bytes) == key.code') != 1 or text.count('Xxh3(source, bytes) == code)') != 1:
        return fail('lint: a shader create path remembers without checking the microcode still hashes to its key')
    print('PASS lint: reader.h glue (Invalidate before every window clear, MakeKey with every field, create paths re-hash)')
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
