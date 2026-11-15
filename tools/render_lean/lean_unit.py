r"""S102 lane RT1: the unit test of render_lean.h's pure pieces (tools\render_lean\lean_test.cpp), with planted twins.   py -I tools\render_lean\lean_unit.py [--root DIR]
The real run must pass every case; plant1 (a destroyed image's cached sets are not evicted, so a recreated image at the same view handle reads the OLD set) and plant3 (a staging buffer handed back
while its copy is in flight) must each exit 1: a plant that exits 0 is NOT caught and fails here. Also the past bug this lever could repeat, read from the ledger: a stale descriptor / image
after a recreate (plant1 is that class). 0 game runs."""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.abspath(os.path.join(HERE, '..', '..')))
    root = os.path.abspath(ap.parse_args().root)
    out = os.path.join(root, 'out', 'render_lean')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'lean_test.exe')
    src = os.path.join(root, 'tools', 'render_lean', 'lean_test.cpp')
    clang = r'C:\Program Files\LLVM\bin\clang++.exe'
    p = subprocess.run([clang, '-std=c++20', '-O1', '-Wall', '-Wextra', '-D_CRT_SECURE_NO_WARNINGS', src, '-o', exe], capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stdout + p.stderr)
        print('FAIL  lean_test did not compile')
        return 1
    bad = 0
    r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    print(r.stdout.strip())
    if r.returncode != 0:
        print('FAIL  the real rules failed')
        bad += 1
    else:
        print('PASS  the real rules')
    for plant in ('plant1', 'plant3'):
        r = subprocess.run([exe, plant], capture_output=True, text=True, timeout=120)
        caught = r.returncode == 1
        print(('PASS  ' if caught else 'FAIL  ') + '%s %s' % (plant, 'CAUGHT (the test exits 1)' if caught else 'NOT caught (exit %d)' % r.returncode))
        bad += 0 if caught else 1
    # ---- Q9 Note flat, Q7 arena memo: each built from its own test, a real run and a planted twin
    try:
        common = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=30).stdout.strip()
        main_co = os.path.dirname(common) if common else root
    except Exception:
        main_co = root
    tests = (
        ('plane_sort', ['tools/render_lean/plane_sort_test.cpp'], [], 'plant4'),
        ('note_flat', ['tools/render_lean/note_flat_test.cpp'], [], 'plant'),
        ('arena_memo', ['tools/render_lean/arena_memo_test.cpp', 'src/renderer_scene/ring_ours_core.cpp', 'src/renderer_scene/arena_ours_core.cpp', 'src/renderer_scene/record_io.cpp'],
         ['-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-I', os.path.join(main_co, 'src', 'assets')], 'plant2'),   # (the include path reaches the main checkout's thirdparty stb through ..\..)
    )
    for name, sources, flags, plant in tests:
        exe2 = os.path.join(out, name + '_test.exe')
        p = subprocess.run([clang, '-std=c++20', '-O1', '-I', os.path.join(root, 'src')] + flags + [os.path.join(root, x) for x in sources] + ['-o', exe2], capture_output=True, text=True)
        if p.returncode != 0:
            print(p.stdout + p.stderr)
            print('FAIL  %s did not compile' % name)
            bad += 1
            continue
        r = subprocess.run([exe2], capture_output=True, text=True, timeout=300)
        print(r.stdout.strip().splitlines()[-1])
        print(('PASS  ' if r.returncode == 0 else 'FAIL  ') + '%s: the real rule equals the shipped one' % name)
        bad += 0 if r.returncode == 0 else 1
        for pl in ([plant] + (['plant6'] if name == 'arena_memo' else [])):
            r = subprocess.run([exe2, pl], capture_output=True, text=True, timeout=300)
            caught = r.returncode == 1
            print(('PASS  ' if caught else 'FAIL  ') + '%s %s' % (pl, 'CAUGHT (the test exits 1)' if caught else 'NOT caught (exit %d)' % r.returncode))
            bad += 0 if caught else 1
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
