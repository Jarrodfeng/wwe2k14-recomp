#!/usr/bin/env python3
"""Gate check: the game exe's translation units still compile (S102 T11 follow-up; G12 named a struct field `small`, a macro of the Windows RPC headers the GAME build
includes: fview and rplay compiled, the game exe did not, and nobody saw it until the build).

  py -I tools\\gate\\game_syntax.py [--root DIR] [--files src/a.cpp,src/b.cpp] [--no-twin]

clang -fsyntax-only of src\\main.cpp and src\\renderer_adapter\\live.cpp (and any unit --files names; each must be one the main build compiles), with the flags the MAIN
checkout's own build uses (`ninja -t commands` in out\\build\\win-amd64-release: defines, standard, system includes), the tree's src (--root) put first so a worktree's
edits are what gets compiled; includes the main checkout still owns (generated\\default, thirdparty) come after.
Add it to any lane with:   {"name": "<lane>.game_syntax", "cmd": ["py", "-I", "tools/gate/game_syntax.py", "--root", "."], "timeout_s": 900,
                            "needs": ["generated/default", "thirdparty/rexglue-sdk-vulkan/include"], "affects": [<the headers the lane edits>]}
The planted twin (unless --no-twin): src\\main.cpp followed by `struct PlantSmall { int small; };` in a scratch unit under out\\game_syntax must FAIL to compile (the macro
is live in that unit); if it compiles, the check itself is blind and FAILs. A missing ninja / build dir / flags line fails loudly (it never falls back to guessed flags)."""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
DEFAULT_FILES = ['src/main.cpp', 'src/renderer_adapter/live.cpp']
BUILD_DIR = os.path.join('out', 'build', 'win-amd64-release')


def main_checkout(root):
    out = subprocess.run(['git', '-C', root, 'rev-parse', '--git-common-dir'], capture_output=True, text=True)
    if out.returncode:
        return None
    return os.path.dirname(os.path.abspath(os.path.join(root, out.stdout.strip())))


def find_ninja():
    n = shutil.which('ninja')
    if n:
        return n
    for pat in (r'C:\Program Files\Microsoft Visual Studio\2022\*\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe',
                r'C:\Program Files (x86)\Microsoft Visual Studio\2022\*\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'):
        hits = glob.glob(pat)
        if hits:
            return hits[0]
    return None


def build_commands(main):
    ninja = find_ninja()
    if not ninja:
        raise RuntimeError('ninja not found (PATH or the Visual Studio copy)')
    bd = os.path.join(main, BUILD_DIR)
    if not os.path.isfile(os.path.join(bd, 'build.ninja')):
        raise RuntimeError('no build.ninja in ' + bd + ' (the main checkout has never configured the release build)')
    r = subprocess.run([ninja, '-C', bd, '-t', 'commands'], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError('ninja -t commands failed: ' + r.stderr[:500])
    return [ln for ln in r.stdout.splitlines() if ' -c ' in ln]


def flags_for(lines, main, tu_rel):
    """The main build's flags for one unit, without the object / dependency / pch parts."""
    want = (main + '/' + tu_rel).replace('\\', '/').lower()
    for ln in lines:
        m = re.search(r' -c (\S+)\s*$', ln)
        if not m or m.group(1).replace('\\', '/').lower() != want:
            continue
        toks = ln.replace('\\"', '"').split()[1:]
        out, i = [], 0
        while i < len(toks):
            t = toks[i]
            if t in ('-MT', '-MF', '-o', '-c', '-include', '-Xclang'):
                i += 2 if t != '-c' else 2
                continue
            if t in ('-MD', '-MMD', '-Winvalid-pch') or t == m.group(1):
                i += 1
                continue
            out.append(t)
            i += 1
        return out
    raise RuntimeError('the main build has no compile command for %s: only units it compiles can be checked' % tu_rel)


def compile_unit(root, main, flags, unit_path, label):
    wt = os.path.abspath(root).replace('\\', '/')
    m = main.replace('\\', '/')
    # the tree's own roots first; the main checkout's after (generated code, thirdparty, ../thirdparty relative includes)
    inc = ['-I' + wt, '-I' + wt + '/src']
    kept = []
    for f in flags:
        if f.startswith('-I') and f[2:].replace('\\', '/').rstrip('/') in (m, m + '/src'):
            continue   # replaced by the tree's own roots above
        kept.append(f)
    inc += ['-I' + m, '-I' + m + '/src']
    r = subprocess.run([CLANG] + kept + inc + ['-fsyntax-only', '-Wno-everything', unit_path], capture_output=True, text=True)
    return label, r.returncode, (r.stdout + r.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--files', default='')
    ap.add_argument('--no-twin', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    if not os.path.isfile(CLANG):
        print('FAIL: %s not found' % CLANG)
        return 1
    main_dir = main_checkout(root)
    if not main_dir or not os.path.isdir(os.path.join(main_dir, 'generated', 'default')):
        print('FAIL: the main checkout with generated\\default was not found (%s): the game units cannot be checked' % main_dir)
        return 1
    files = DEFAULT_FILES + [f.strip().replace('\\', '/') for f in a.files.split(',') if f.strip()]
    try:
        lines = build_commands(main_dir)
        flags = {f: flags_for(lines, main_dir.replace('\\', '/'), f) for f in files}
    except RuntimeError as e:
        print('FAIL: ' + str(e))
        return 1
    jobs = []
    for f in files:
        p = os.path.join(root, f)
        if not os.path.isfile(p):
            print('FAIL: missing ' + p)
            return 1
        jobs.append((f, flags[f], p, False))
    if not a.no_twin:
        d = os.path.join(root, 'out', 'game_syntax')
        os.makedirs(d, exist_ok=True)
        twin = os.path.join(d, 'plant_small.cpp')
        open(twin, 'w').write('#include "%s/src/main.cpp"\nstruct PlantSmall { int small; };   // planted: `small` is a macro of the Windows RPC headers main.cpp pulls in\n' % root.replace('\\', '/'))
        jobs.append(('planted twin (a member named small after main.cpp)', flags['src/main.cpp'], twin, True))
    with ThreadPoolExecutor(max_workers=4) as ex:
        res = list(ex.map(lambda j: (compile_unit(root, main_dir, j[1], j[2], j[0]), j[3]), jobs))
    bad = 0
    for (label, rc, text), is_twin in res:
        if is_twin:
            if rc == 0:
                print('FAIL: the planted twin compiled: `small` is not a live macro in main.cpp\'s unit, so this check cannot see the G12 class of fault')
                bad += 1
            else:
                print('ok   %s: failed to compile, as planted (%s)' % (label, (re.findall(r'error: [^\n]*', text) or ['?'])[0][:120]))
        else:
            print('%s %s' % ('ok  ' if rc == 0 else 'FAIL', label))
            if rc:
                bad += 1
                print(text[:2000])
    if bad:
        return 1
    print('PASS: %d game translation units compile (-fsyntax-only, the main build\'s flags, this tree\'s src first)%s' % (len(files), '; the planted twin is caught' if not a.no_twin else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
