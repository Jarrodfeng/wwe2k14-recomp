#!/usr/bin/env python3
"""S103 L33F (review F1): which game translation units can SEE WWE2K14_VERSION, and does a dev-only test anywhere read the macro where it is not defined?

CMakeLists.txt defines WWE2K14_VERSION for ONE game source file per set_property(SOURCE ... WWE2K14_VERSION ...) (main.cpp, src/dev_build.cpp): a target-wide define would rebuild the
generated code on every version change. Any other game .cpp (and every header it includes) that tests `#ifdef WWE2K14_VERSION` / `defined(WWE2K14_VERSION)` or uses the macro sees it UNDEFINED,
so a "+dev only" guard there falls to its #else branch in every build (live.cpp before L33F: `dev = true` in player release builds: force_inputs, sentinel, unrequire honoured).
The rule: outside the TUs that get the define, a dev-only decision calls wwe2k14::IsDevBuild() (src/dev_build.h, compiled in src/dev_build.cpp, which gets the define).

  py -I tools/gate/version_define_lint.py [--root DIR]      the lint (exit 0 = no violation)
  py -I tools/gate/version_define_lint.py --selftest        a planted header that tests the macro, included from a TU without the define, must be caught
  py -I tools/gate/version_define_lint.py --release-check [--plant]   (--plant: the twin, no define = dev, must exit 1) builds src/dev_build.cpp three ways with clang (the release string, the +dev string, no define) and checks IsDevBuild():
                                                            false / true / false (fail closed: no define = not dev); and that CMakeLists.txt gives dev_build.cpp the define
Launcher targets (src/launcher, src/launcher_mobile) get the define target-wide and are not game TUs: skipped."""
import argparse
import os
import re
import subprocess
import sys
import tempfile

INC = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
USE = re.compile(r'WWE2K14_VERSION')
SKIP_DIRS = ('launcher', 'launcher_mobile')
CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'


def find_cmake():   # PATH first (tools/env.ps1 puts VS's on it), else the VS 2022 bundled one
    import glob
    import shutil
    w = shutil.which('cmake')
    if w:
        return w
    g = sorted(glob.glob(r'C:\Program Files\Microsoft Visual Studio\2022\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')) + [r'C:\Program Files\CMake\bin\cmake.exe']
    return next((x for x in g if os.path.isfile(x)), 'cmake')


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    return re.sub(r'//[^\n]*', '', s)


def defined_sources(root):
    with open(os.path.join(root, 'CMakeLists.txt'), encoding='utf-8') as f:
        cm = f.read()
    out = set()
    for m in re.finditer(r'set_property\(\s*SOURCE\s+([^\n]*?)\s+APPEND\s+PROPERTY\s+COMPILE_DEFINITIONS\s+"WWE2K14_VERSION=', cm):
        for s in m.group(1).split():
            out.add(os.path.normpath(os.path.join(root, s)))
    return out


def closure(path, root, seen):
    if path in seen or not os.path.isfile(path):
        return
    seen.add(path)
    with open(path, encoding='utf-8', errors='replace') as f:
        text = f.read()
    for inc in INC.findall(text):
        for base in (os.path.dirname(path), os.path.join(root, 'src')):
            cand = os.path.normpath(os.path.join(base, inc))
            if os.path.isfile(cand):
                closure(cand, root, seen)
                break


def lint(root, extra_tus=()):
    src = os.path.join(root, 'src')
    ok = defined_sources(root)
    tus = list(extra_tus)
    for d, dirs, files in os.walk(src):
        rel = os.path.relpath(d, src).split(os.sep)
        if rel[0] in SKIP_DIRS:
            continue
        tus += [os.path.join(d, f) for f in files if f.endswith('.cpp')]
    bad = {}
    for tu in tus:
        if os.path.normpath(tu) in ok:
            continue
        seen = set()
        closure(os.path.normpath(tu), root, seen)
        for h in seen:
            with open(h, encoding='utf-8', errors='replace') as f:
                code = strip_comments(f.read())
            for n, ln in enumerate(code.splitlines(), 1):
                if USE.search(ln):
                    bad.setdefault((h, n, ln.strip()), os.path.relpath(tu, root))
    return ok, bad


def release_check(root, plant=False):
    cpp = os.path.join(root, 'src', 'dev_build.cpp')
    if plant:   # the planted twin: the old live.cpp rule (no define = dev); the 'no define' case must FAIL
        with open(cpp, encoding='utf-8') as f:
            src = f.read().replace('return "";', 'return "+dev";')
        cpp = os.path.join(tempfile.mkdtemp(prefix='verplant_'), 'dev_build.cpp')
        with open(cpp, 'w', encoding='utf-8') as f:
            f.write(src)
    with open(os.path.join(root, 'CMakeLists.txt'), encoding='utf-8') as f:
        cm = f.read()
    fails = 0
    if os.path.normpath(os.path.join(root, 'src', 'dev_build.cpp')) not in defined_sources(root):
        print('FAIL: CMakeLists.txt does not give src/dev_build.cpp the WWE2K14_VERSION define')
        fails += 1
    if 'src/dev_build.cpp' not in cm.split('set(WWE2K14_SOURCES', 1)[-1]:
        print('FAIL: src/dev_build.cpp is not a game source in CMakeLists.txt')
        fails += 1
    tmp = tempfile.mkdtemp(prefix='verdef_')
    main = os.path.join(tmp, 'm.cpp')
    with open(main, 'w') as f:
        f.write('#include "dev_build.h"\n#include <cstdio>\nint main(){ std::printf("%d\\n", wwe2k14::IsDevBuild() ? 1 : 0); return 0; }\n')
    # the version block of CMakeLists.txt itself, configured in script mode with WWE2K14_RELEASE ON (make_release.ps1) and OFF (every other build): the strings the real configure gives dev_build.cpp
    lines = cm.split('file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" WWE2K14_VERSION', 1)
    configured = {}
    if len(lines) != 2:
        print('FAIL: the version block (file(STRINGS ... VERSION ...)) is not in CMakeLists.txt')
        fails += 1
    else:
        block = ('file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" WWE2K14_VERSION' + lines[1]).split('message(STATUS "wwe2k14 version', 1)[0]
        keep = [ln for ln in block.splitlines() if ln.strip().startswith(('file(STRINGS', 'string(APPEND', 'if(', 'endif(', 'else('))]
        for rel in ('ON', 'OFF'):
            sp = os.path.join(tmp, 'ver_%s.cmake' % rel)
            with open(sp, 'w') as f:
                f.write('set(CMAKE_CURRENT_SOURCE_DIR "%s")\nset(WWE2K14_RELEASE %s)\nset(WWE2K14_VERSION_SUFFIX "")\n%s\nmessage("VER=${WWE2K14_VERSION}")\n' % (root.replace('\\', '/'), rel, '\n'.join(keep)))
            c = subprocess.run([find_cmake(), '-P', sp], capture_output=True, text=True)
            m = re.search(r'VER=(\S+)', c.stdout + c.stderr)
            if c.returncode or not m:
                print('FAIL: cmake -P of the version block (RELEASE %s) failed: %s' % (rel, (c.stdout + c.stderr)[-500:]))
                fails += 1
            else:
                configured[rel] = m.group(1)
                print('configure WWE2K14_RELEASE=%s -> WWE2K14_VERSION "%s"' % (rel, m.group(1)))
    cases = [('release', ['-DWWE2K14_VERSION="v0.3.3"'], 0), ('playtest', ['-DWWE2K14_VERSION="v0.3.3-playtest"'], 0), ('dev', ['-DWWE2K14_VERSION="v0.3.3+dev"'], 1), ('no define', [], 0)]
    if 'ON' in configured:
        cases.append(('configured RELEASE=ON', ['-DWWE2K14_VERSION="%s"' % configured['ON']], 0))
    if 'OFF' in configured:
        cases.append(('configured RELEASE=OFF', ['-DWWE2K14_VERSION="%s"' % configured['OFF']], 1))
    for k, (name, define, want) in enumerate(cases):
        exe = os.path.join(tmp, 'm_%d.exe' % k)
        b = subprocess.run([CLANG, '-std=c++20', '-fuse-ld=lld', '-I', os.path.join(root, 'src'), main, cpp, '-o', exe] + define, capture_output=True, text=True)
        if b.returncode:
            print('FAIL: %s: did not build\n%s' % (name, b.stderr[-2000:]))
            fails += 1
            continue
        got = subprocess.run([exe], capture_output=True, text=True).stdout.strip()
        print('%s %-10s IsDevBuild() = %s (want %d)' % ('PASS' if got == str(want) else 'FAIL', name, got, want))
        fails += 0 if got == str(want) else 1
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--release-check', action='store_true')
    ap.add_argument('--plant', action='store_true', help='with --release-check: the planted twin (no define = dev) must fail')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    if a.release_check:
        f = release_check(root, a.plant)
        print('release_check%s: %s' % (' (planted twin)' if a.plant else '', 'PASSED' if not f else '%d FAIL' % f))
        return 1 if f else 0
    if a.selftest:
        tmp = tempfile.mkdtemp(prefix='verlint_')
        os.makedirs(os.path.join(tmp, 'src', 'x'))
        with open(os.path.join(tmp, 'CMakeLists.txt'), 'w') as f:
            f.write('set_property(SOURCE src/main.cpp APPEND PROPERTY COMPILE_DEFINITIONS "WWE2K14_VERSION=\\"${WWE2K14_VERSION}\\"")\n')
        with open(os.path.join(tmp, 'src', 'x', 'guard.h'), 'w') as f:
            f.write('// WWE2K14_VERSION in a comment is fine\n#ifdef WWE2K14_VERSION\nconst bool dev = false;\n#else\nconst bool dev = true;\n#endif\n')
        with open(os.path.join(tmp, 'src', 'x', 'clean.h'), 'w') as f:
            f.write('#include "dev_build.h"\nconst bool dev2 = true;\n')
        with open(os.path.join(tmp, 'src', 'main.cpp'), 'w') as f:
            f.write('#include "x/guard.h"\n')   # main.cpp has the define: not a violation
        with open(os.path.join(tmp, 'src', 'x', 'live.cpp'), 'w') as f:
            f.write('#include "clean.h"\n')
        _, bad0 = lint(tmp)
        with open(os.path.join(tmp, 'src', 'x', 'live.cpp'), 'w') as f:
            f.write('#include "clean.h"\n#include "guard.h"\n')   # the planted twin: a TU without the define reads the macro through a header
        _, bad1 = lint(tmp)
        ok = not bad0 and len(bad1) == 1 and list(bad1)[0][1] == 2
        twin = release_check(root, True)   # the release-check twin (no define = dev) must be caught
        ok = ok and twin > 0
        print('selftest %s (clean %d, planted %d, release twin %s)' % ('PASSED' if ok else 'FAILED', len(bad0), len(bad1), 'caught' if twin else 'NOT CAUGHT'))
        return 0 if ok else 1
    ok, bad = lint(root)
    for (h, n, ln), tu in sorted(bad.items()):
        print('FAIL: %s:%d reads WWE2K14_VERSION but %s does not get the define (use wwe2k14::IsDevBuild() / VersionString(), src/dev_build.h): %s' % (os.path.relpath(h, root), n, tu, ln))
    print('version_define_lint: %d TU(s) with the define (%s), %d violation(s)' % (len(ok), ', '.join(sorted(os.path.relpath(x, root) for x in ok)), len(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
