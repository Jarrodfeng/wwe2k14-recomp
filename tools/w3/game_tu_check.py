"""S102 W3: syntax-check the game's main translation unit (src\\main.cpp of THIS checkout) with the main build's own flags, so a header the game
compiles (replay_watch.h, nf\\replay_native.h, the switch table) is checked as the GAME compiles it, not as an offline tool does (S102 rule: "a change to
adapter / renderer headers is syntax-checked as the GAME compiles it"). Flags (defines, includes, -std, -msse4.1) come from the MAIN checkout's
out\\build\\win-amd64-release\\build.ninja; the precompiled header is replaced by including its source header; `-I <main>\\src` becomes this
checkout's src (a quoted include already resolves against the including file's own folder first).

    py -I tools\\w3\\game_tu_check.py [--file src\\main.cpp] [--root DIR]

Exit 0 = the unit compiled with no error (-fsyntax-only), 1 = errors (printed), 2 = the inputs are missing (loud, never a pass).
"""
import argparse
import os
import re
import shlex
import subprocess
import sys

CLANG = 'C:/Program Files/LLVM/bin/clang++.exe'
MAIN = r'C:\Users\Jarrod\wwe2k14-recomp'


def flags(main_root):
    ninja = os.path.join(main_root, 'out', 'build', 'win-amd64-release', 'build.ninja')
    if not os.path.exists(ninja):
        print('game_tu_check: %s is missing (no main build to take the flags from)' % ninja)
        sys.exit(2)
    lines = open(ninja, encoding='utf-8', errors='replace').read().splitlines()
    i = next((k for k, l in enumerate(lines) if l.startswith('build CMakeFiles/wwe2k14.dir/src/main.cpp.obj')), None)
    if i is None:
        print('game_tu_check: no main.cpp rule in build.ninja')
        sys.exit(2)
    vals = {}
    for l in lines[i + 1:i + 12]:
        m = re.match(r'\s+(DEFINES|FLAGS|INCLUDES) = (.*)', l)
        if m:
            vals[m.group(1)] = m.group(2)
    f = vals['FLAGS']
    pch_src = re.search(r'-Xclang -include -Xclang (\S+)', f).group(1)
    f = re.sub(r'-Xclang -include-pch -Xclang \S+', '', f)
    f = re.sub(r'-Xclang -include -Xclang \S+', '', f)
    args = shlex.split(vals['DEFINES'].replace('\\"', '"'), posix=True) + shlex.split(f) + shlex.split(vals['INCLUDES'])
    return args, pch_src


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--file', default=r'src\main.cpp')
    ap.add_argument('--root', default=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    args, pch_src = flags(MAIN)
    src_main = (MAIN + '\\src').replace('\\', '/').lower()
    out = []
    for x in args:
        if x.replace('\\', '/').lower() == '-i' + src_main:
            # this checkout's src first; the main src stays after it (a header reaches ..\thirdparty through it, e.g. native_zlib.h's stb_image)
            out.append('-I' + os.path.join(root, 'src').replace('\\', '/'))
        out.append(x)
    target = os.path.join(root, a.file)
    if not os.path.exists(target):
        print('game_tu_check: %s is missing' % target)
        return 2
    tu = os.path.join(os.environ.get('TEMP', '.'), 'w3_game_tu_%d.cpp' % os.getpid())
    open(tu, 'w').write('#include "%s"\n#include "%s"\n' % (pch_src, target.replace('\\', '/')))
    try:
        p = subprocess.run([CLANG, '-fsyntax-only', '-Wno-deprecated-declarations', '-ferror-limit=20', *out, tu], capture_output=True, text=True)
    finally:
        os.unlink(tu)
    errs = [l for l in p.stderr.splitlines() if ' error:' in l or 'fatal error' in l]
    print('game_tu_check: %s: %d error(s), exit %d' % (a.file, len(errs), p.returncode))
    if p.returncode != 0:
        print('\n'.join(p.stderr.splitlines()[:60]))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
