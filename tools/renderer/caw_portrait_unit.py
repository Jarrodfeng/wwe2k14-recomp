#!/usr/bin/env python3
"""S103 CPB: builds and runs tools\\renderer\\caw_portrait_unit.cpp (src\\renderer_game\\caw_portrait_core.h: the Create a Superstar portrait's G3 filter + writer and G2 tick gate;
no game, no GPU, 0 game runs), then its planted twins (each, built with -DWWE2K14_CAW_PLANT=N, must exit 1), then the wiring lints:
  - caw_portrait.h is included by d3d_textures.h only under #if !WWE2K14_STREAM() (the hooks exist in the stream-free build only);
  - sub_82CFA318's hook runs the original for every lr other than 0x8341B780 and is hooked nowhere else.
  py -I tools\\renderer\\caw_portrait_unit.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {1: 'writer_wrong_byte_order', 2: 'filter_ignores_lr', 3: 'g2_late_not_counted', 4: 'd1_keeps_all_objects', 5: 'retry_count_carries_over', 6: 'census_pieces_from_id_list'}


def build_run(root, out, plant):
    exe = os.path.join(out, 'caw_portrait_unit%s.exe' % ('_p%d' % plant if plant else ''))
    cmd = [CLANG, '-std=c++20', '-O1', '-Wall', '-Wno-unused-function'] + (['-DWWE2K14_CAW_PLANT=%d' % plant] if plant else []) + ['-o', exe, os.path.join(root, 'tools', 'renderer', 'caw_portrait_unit.cpp')]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout + r.stderr)
        return None, 'build failed'
    return subprocess.run([exe], capture_output=True, text=True, timeout=120), None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 'caw_portrait_unit')
    os.makedirs(out, exist_ok=True)
    bad = []
    p, err = build_run(root, out, 0)
    if err:
        print('FAIL ' + err)
        return 1
    sys.stdout.write(p.stdout)
    if p.returncode != 0:
        bad.append('unit arms')
    for n, name in PLANTS.items():
        p, err = build_run(root, out, n)
        caught = err is None and p.returncode == 1 and 'FAIL' in p.stdout
        print(('ok   ' if caught else 'FAIL ') + 'planted twin %d (%s) is caught' % (n, name))
        if not caught:
            bad.append('plant %d' % n)
    dt = open(os.path.join(root, 'src', 'd3d_textures.h'), encoding='utf-8', errors='replace').read()
    m = re.search(r'#if\s+!\s*WWE2K14_STREAM\(\)[^\n]*\n\s*#include\s+"renderer_game/caw_portrait\.h"', dt)
    print(('ok   ' if m else 'FAIL ') + 'd3d_textures.h includes caw_portrait.h only under #if !WWE2K14_STREAM()')
    if not m:
        bad.append('include gate')
    hk = open(os.path.join(root, 'src', 'renderer_game', 'caw_portrait.h'), encoding='utf-8').read()
    i = hk.index('REX_HOOK_RAW(sub_82CFA318)')
    ok = hk.count('REX_HOOK_RAW(sub_82CFA318)') == 1 and 'kUntileLr' in hk[i:] and '__imp__sub_82CFA318(ctx, base);' in hk[i:]
    n_other = 0
    for dp, _, fs in os.walk(os.path.join(root, 'src')):
        for f in fs:
            if f.endswith(('.h', '.cpp')) and f != 'caw_portrait.h':
                if 'REX_HOOK_RAW(sub_82CFA318)' in open(os.path.join(dp, f), encoding='utf-8', errors='replace').read():
                    n_other += 1
    print(('ok   ' if ok and not n_other else 'FAIL ') + 'sub_82CFA318 hooked once, gated by kUntileLr, original run otherwise')
    if not ok or n_other:
        bad.append('hook shape')
    print('caw_portrait_unit.py: ' + ('FAIL ' + ', '.join(bad) if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
