#!/usr/bin/env python3
"""S103 lane T: builds and runs tools\\renderer\\trace_census_unit.cpp, then its five planted twins (each must exit 1), then lints the census hooks (one hook per function, the
shadow node's line sits before its scene test, nothing else hooks the five functions).
  py -I tools\\renderer\\trace_census_unit.py [--root DIR]      Exit 0 = pass, 1 = fail."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
FUNCS = ['sub_827F10C8', 'sub_82815840', 'sub_827F9D88', 'sub_82C0F528', 'sub_826203E8', 'sub_82812E90']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    root = os.path.abspath(ap.parse_args().root)
    out = os.path.join(root, 'out', 's103', 't')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'trace_census_unit.exe')
    b = subprocess.run([CLANG, '-std=c++20', '-Wall', '-Wextra', '-pthread', '-o', exe, os.path.join(root, 'tools', 'renderer', 'trace_census_unit.cpp')], capture_output=True, text=True)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    r = subprocess.run([exe], capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode:
        print('FAIL: the unit test failed')
        return 1
    for plant in (1, 2, 3, 4, 5):
        if subprocess.run([exe, str(plant)], capture_output=True, text=True).returncode == 0:
            print('FAIL: planted twin %d was not caught' % plant)
            return 1
        print('twin %d: caught' % plant)
    srcs = {}
    for dp, _, files in os.walk(os.path.join(root, 'src')):
        for f in files:
            if f.endswith(('.h', '.cpp')):
                p = os.path.join(dp, f)
                srcs[os.path.relpath(p, root).replace('\\', '/')] = open(p, encoding='utf-8', errors='replace').read()
    bad = 0
    for fn in FUNCS:
        hits = sorted(p for p, s in srcs.items() if re.search(r'^\s*REX_HOOK(_RAW)?\(%s\)' % fn, s, re.M))
        ok = hits == ['src/scene_live/trace_census_hook.h']
        print(('ok   ' if ok else 'FAIL ') + 'exactly one hook on %s: %s' % (fn, ', '.join(hits)))
        bad += 0 if ok else 1
    own = srcs['src/scene_live/own_node_hook.h']
    i = own.index('REX_HOOK_RAW(sub_827E9740) {')
    body = own[i:i + 900]
    ok = 'trace_census::Site::ShadowNode' in body and body.find('trace_census::Site::ShadowNode') < body.find('Ours(')
    print(('ok   ' if ok else 'FAIL ') + 'the shadow node hook carries the census line before its scene test')
    bad += 0 if ok else 1
    hook = srcs['src/scene_live/trace_census_hook.h']
    ok = 'mode == ::scene_frame::Mode::Off) return;' in hook and 'REXCVAR_GET(wwe2k14_rdr_scene_census)' in hook
    print(('ok   ' if ok else 'FAIL ') + 'census counts only while the scene mode is not Off')
    bad += 0 if ok else 1
    print('lints: %d failed' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
