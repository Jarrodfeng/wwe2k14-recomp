"""Syntax-check native function headers for Android (Session 95): the main.cpp flags of out\\android\\game-arm64\\build.ninja,
the NDK's clang, -fsyntax-only, without building and without the precompiled header. Answers: does src\\nf\\all.h (or any
header given) compile for arm64? Run from a worktree: its src\\ goes first on the include path.

    py tools\\perf\\nf_check_arm.py [src\\nf\\all.h ...]
"""
import glob
import os
import re
import shlex
import subprocess
import sys
import tempfile

MAIN = r'C:\Users\Jarrod\wwe2k14-recomp'
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NINJA = os.path.join(MAIN, 'out', 'android', 'game-arm64', 'build.ninja')


def main():
    lines = open(NINJA, encoding='utf-8', errors='replace').read().splitlines()
    i = next(k for k, l in enumerate(lines) if re.match(r'build \S*src/main\.cpp\.o(bj)?:', l))
    v = {}
    for l in lines[i + 1:]:
        if not l.startswith('  '):
            break
        a, _, b = l.strip().partition(' = ')
        v[a] = b
    ndk = sorted(glob.glob(os.path.expandvars(r'%LOCALAPPDATA%\Android\Sdk\ndk\*')))[-1]
    clang = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', 'windows-x86_64', 'bin', 'clang++.exe')
    flags = [f for f in shlex.split(v.get('FLAGS', '').replace('\\', '/')) if not f.endswith('.pch') and '-include-pch' not in f]
    flags = [f for f in flags if not re.search(r'cmake_pch|-Xclang|^-include$|^-Winvalid-pch', f)]
    defs = shlex.split(v.get('DEFINES', '').replace('\\', '/'))
    incs = shlex.split(v.get('INCLUDES', '').replace('\\', '/'))
    pch = os.path.join(MAIN, 'generated', 'default', 'wwe2k14_pch.h').replace('\\', '/')
    bad = 0
    for h in sys.argv[1:] or [os.path.join('src', 'nf', 'all.h')]:
        h = os.path.abspath(os.path.join(ROOT, h)).replace('\\', '/')
        with tempfile.NamedTemporaryFile('w', suffix='.cpp', delete=False) as f:
            f.write(f'#include "{pch}"\n#include "{h}"\n')
        cmd = [clang, '--target=aarch64-none-linux-android33', '-fsyntax-only', '-w', '-DWWE2K14_NF_ANDROID', f'-I{ROOT}/src'.replace('\\', '/')] + defs + incs + flags + [f.name]
        r = subprocess.run(cmd, capture_output=True, text=True, cwd=os.path.dirname(NINJA))
        print(h, 'OK' if r.returncode == 0 else 'FAILED')
        if r.returncode:
            bad += 1
            print('\n'.join(r.stderr.splitlines()[:30]))
        os.unlink(f.name)
    return bad


if __name__ == '__main__':
    sys.exit(main())
