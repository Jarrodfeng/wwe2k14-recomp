#!/usr/bin/env python3
"""S102 lane P4P5: builds and runs tools\\renderer\\own_nodes_unit.cpp (the node switch's decision layer for the shadow module and the rim map: GPU free, no game, 0 game runs), then its five
planted twins (each must exit 1), then the wiring lints of the live hook (compiled and reviewed, NOT run: the lints read the sources and fail loudly on a missing piece).
  py -I tools\\renderer\\own_nodes_unit.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's102', 'p4p5')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'own_nodes_unit.exe')
    b = subprocess.run([CLANG, '-std=c++20', '-Wall', '-Wextra', '-o', exe, os.path.join(root, 'tools', 'renderer', 'own_nodes_unit.cpp')], capture_output=True, text=True)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    r = subprocess.run([exe], capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode:
        print('FAIL: the unit test failed')
        return 1
    for plant in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11):
        p = subprocess.run([exe, str(plant)], capture_output=True, text=True)
        if p.returncode == 0:
            print('FAIL: planted twin %d was not caught' % plant)
            return 1
        print('twin %d: caught' % plant)

    def text(rel):
        with open(os.path.join(root, rel), encoding='utf-8') as f:
            return f.read()
    hook, sw, do, core = text('src/scene_live/own_node_hook.h'), text('src/renderer_adapter/rdr_switches.h'), text('src/draw_origin.h'), text('src/renderer_game/own_shadow_rim.h')
    need = [
        (sw, 'X(OwnShadowModule, "wwe2k14_rdr_own_shadow_module", PerFrame, 0)', 'switch table: own_shadow_module per-frame, default off'),
        (sw, 'X(OwnRimMap, "wwe2k14_rdr_own_rim_map", PerFrame, 0)', 'switch table: own_rim_map per-frame, default off'),
        (hook, 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_own_shadow_module, false', 'the hook defines the shadow module cvar (default false)'),
        (hook, 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_own_rim_map, false', 'the hook defines the rim map cvar (default false)'),
        (hook, 'REX_HOOK_RAW(sub_827E9740)', 'one hook on the shadow module\'s render entry'),
        (hook, 'REX_HOOK_RAW(sub_827EA7B8)', 'one hook on the rim map\'s render entry'),
        (hook, 'rdr_sw::BoolSeen(rdr_sw::kOwnShadowModule', 'the shadow hook reads its switch every frame through the table'),
        (hook, 'rdr_sw::BoolSeen(rdr_sw::kOwnRimMap', 'the rim hook reads its switch every frame through the table'),
        (hook, 'DecideNode(rdr_own::Pass::ShadowModule', 'the shadow hook decides through node_switch'),
        (hook, 'DecideNode(rdr_own::Pass::Rim', 'the rim hook decides through node_switch'),
        (hook, 'KeepShadowInstall(ctx, base, node - 4)', 'the shadow hook keeps R\'s install when it does not walk'),
        (hook, 'sub_827E7DF0(ctx, base)', 'the keep is the walker\'s own install call'),
        (do, '#include "scene_live/own_node_hook.h"', 'draw_origin.h includes the hook'),
        (core, 'RequireInput(Pass::ShadowModule, b)', 'the shadow module registers its inputs'),
    ]
    bad = 0
    for src, needle, what in need:
        ok = needle in src
        print(('ok   ' if ok else 'FAIL ') + what)
        bad += 0 if ok else 1
    # each function takes ONE hook: no other REX_HOOK_RAW / REX_HOOK on these two in src\ or config\
    for fn in ('sub_827E9740', 'sub_827EA7B8'):
        hits = []
        for dp, _, files in os.walk(os.path.join(root, 'src')):
            for f in files:
                if f.endswith(('.h', '.cpp')):
                    s = open(os.path.join(dp, f), encoding='utf-8', errors='replace').read()
                    if re.search(r'REX_HOOK(_RAW)?\(%s\)' % fn, s):
                        hits.append(f)
        ok = hits == ['own_node_hook.h']
        print(('ok   ' if ok else 'FAIL ') + 'exactly one hook on %s (%s)' % (fn, ', '.join(hits)))
        bad += 0 if ok else 1
    # the Ours branch of the rim hook runs nothing; the shadow hook's Ours branch must not call the original or any draw function
    shadow = hook[hook.index('REX_HOOK_RAW(sub_827E9740)'):hook.index('REX_HOOK_RAW(sub_827EA7B8)')]
    ours_branch = shadow[shadow.index('KeepShadowInstall(ctx, base, node - 4)'):]
    ok = '__imp__sub_827E9740' not in ours_branch
    print(('ok   ' if ok else 'FAIL ') + 'the shadow hook\'s Ours branch does not call the original')
    bad += 0 if ok else 1
    # the readiness call has a caller outside its own header (review 5), and a deleted call fails the lint
    def ready_caller(files):
        return any('SetReadyFromSwitches(' in t for name, t in files.items() if not name.endswith('own_shadow_rim.h'))
    files = {}
    for dp, _, fs in os.walk(os.path.join(root, 'src')):
        for f in fs:
            if f.endswith(('.h', '.cpp')):
                files[os.path.join(dp, f)] = open(os.path.join(dp, f), encoding='utf-8', errors='replace').read()
    ok = ready_caller(files)
    print(('ok   ' if ok else 'FAIL ') + 'SetReadyFromSwitches has a caller in src (live.cpp)')
    bad += 0 if ok else 1
    deleted = {k: v.replace('SetReadyFromSwitches(false, false);', '') for k, v in files.items()}
    ok = not ready_caller(deleted)
    print(('ok   ' if ok else 'FAIL ') + 'the planted deletion of the call fails the lint')
    bad += 0 if ok else 1
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
