#!/usr/bin/env python3
"""S103 lane ONF: builds (WWE2K14_KEEP_STREAM=0) and runs tools\\renderer\\onf_unit.cpp: the decision of a build with no command stream (missing required inputs are counted, never a Walk;
READY is the only gate besides the planned hold), then its four planted twins (each must exit 1), then the lints: the OFF branch is guarded by #if !WWE2K14_STREAM() in scene_frame.h and
scene_host.cpp, and the stream (ON) decision text still holds the InputsMissing rule.
  py -I tools\\renderer\\onf_unit.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's103', 'onf')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'onf_unit.exe')
    b = subprocess.run([CLANG, '-std=c++20', '-Wall', '-Wextra', '-pthread', '-DWWE2K14_KEEP_STREAM=0', '-o', exe, os.path.join(root, 'tools', 'renderer', 'onf_unit.cpp')], capture_output=True, text=True)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    r = subprocess.run([exe], capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode:
        print('FAIL: the unit test failed')
        return 1
    for plant in (1, 2, 3, 4):
        p = subprocess.run([exe, str(plant)], capture_output=True, text=True)
        if p.returncode == 0:
            print('FAIL: planted twin %d was not caught' % plant)
            return 1
        print('twin %d: caught' % plant)

    def text(rel):
        with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
            return f.read()
    bad = 0
    sf = text('src/renderer_game/scene_frame.h')
    i = sf.find('else if (true) why = WhyNot::None;')
    j = sf.rfind('#if', 0, i)
    ok = i > 0 and sf[j:j + 25].startswith('#if !WWE2K14_STREAM()')
    print(('ok   ' if ok else 'FAIL ') + 'scene_frame.h: the count-only rule is inside #if !WWE2K14_STREAM()')
    bad += 0 if ok else 1
    ok = 'why = WhyNot::InputsMissing;' in sf
    print(('ok   ' if ok else 'FAIL ') + 'scene_frame.h: the stream build keeps the InputsMissing rule')
    bad += 0 if ok else 1
    sh = text('src/renderer_host/scene_host.cpp')
    k = sh.find('NoteOursStale(scene_frame::CensusMissing(stale,')
    ok = k > 0 and sh.rfind('#if !WWE2K14_STREAM()', 0, k) > sh.rfind('#endif', 0, k)
    print(('ok   ' if ok else 'FAIL ') + 'scene_host.cpp: the stale-input count-only branch is inside #if !WWE2K14_STREAM()')
    bad += 0 if ok else 1
    print('lints: %d failed' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
