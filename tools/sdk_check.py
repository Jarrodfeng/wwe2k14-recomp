"""Syntax-check ReXGlue SDK source files from any checkout (e.g. a git worktree) with the real Release flags.

Usage: py tools/sdk_check.py [--root <sdk checkout>] <file relative to the SDK root> [...]
   e.g. py tools/sdk_check.py --root C:/Users/Jarrod/wwe2k14-recomp/thirdparty/sdk-wt/cp1 src/graphics/command_processor.cpp

Takes the compile line for each file from the main SDK build (out/build/win-amd64, Ninja Multi-Config, Release),
points the source and include paths at --root instead of the main checkout, drops the precompiled header (it was
built from the main checkout's headers) and runs clang++ -fsyntax-only. Generated include dirs under
out/build/win-amd64 keep pointing at the main build. Exit code = number of files that failed.
For agents that edit SDK code in a worktree and cannot run the real build (builds are serial in the main checkout).
"""
import argparse
import os
import re
import shlex
import subprocess
import sys

MAIN = 'C:/Users/Jarrod/wwe2k14-recomp/thirdparty/rexglue-sdk-src'
BUILD = MAIN + '/out/build/win-amd64'
NINJA = BUILD + '/CMakeFiles/impl-Release.ninja'
CLANG = 'C:/Program Files/LLVM/bin/clang++.exe'


def norm(p):
    return p.replace('\\', '/')


def find_rule(rel):
    target = 'C$:' + MAIN[2:] + '/' + rel
    lines = open(NINJA, encoding='utf-8', errors='replace').read().split('\n')
    for i, line in enumerate(lines):
        if line.startswith('build ') and '/Release/' in line and ('_Release ' + target + ' ') in line + ' ':
            block = {}
            for j in range(i + 1, min(i + 20, len(lines))):
                m = re.match(r'\s+(\w+) = (.*)', lines[j])
                if not m:
                    break
                block[m.group(1)] = m.group(2)
            return block
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=MAIN, help='SDK checkout to check (default: the main checkout)')
    ap.add_argument('files', nargs='+')
    args = ap.parse_args()
    root = norm(os.path.abspath(args.root))
    failed = 0
    for rel in args.files:
        rel = norm(rel).lstrip('/')
        rule = find_rule(rel)
        if rule is None:
            print(f'[sdk_check] {rel}: no Release compile rule found in {NINJA}')
            failed += 1
            continue
        flags = rule.get('FLAGS', '')
        # Drop the PCH (built from the main checkout); keep its -include of cmake_pch.hxx, whose <rex/...> includes
        # resolve through the rewritten -I paths.
        flags = re.sub(r'-Xclang -include-pch -Xclang \S+', '', flags)
        includes = rule.get('INCLUDES', '')
        # Rewrite the SDK's own include dirs (include/, src/, the root) to the worktree. thirdparty/ (git submodules,
        # absent from worktrees) and the generated out/ dirs keep pointing at the main checkout.
        def fix_inc(m):
            p = norm(m.group(1))
            if p.startswith(MAIN) and not p.startswith(MAIN + '/out/') and not p.startswith(MAIN + '/thirdparty/'):
                p = root + p[len(MAIN):]
            return '-I' + p
        includes = re.sub(r'-I(\S+)', fix_inc, includes)
        defines = rule.get('DEFINES', '').replace('\\"', '"')
        cmd = [CLANG, '-fsyntax-only', '-ferror-limit=50'] + shlex.split(defines, posix=True) + \
              shlex.split(flags, posix=True) + shlex.split(includes, posix=True) + [root + '/' + rel]
        r = subprocess.run(cmd, capture_output=True, text=True)
        errors = [l for l in r.stderr.splitlines() if ': error:' in l]
        status = 'OK' if r.returncode == 0 else f'FAILED ({len(errors)} errors)'
        print(f'[sdk_check] {rel}: {status}')
        if r.returncode != 0:
            print(r.stderr[-6000:])
            failed += 1
    sys.exit(failed)


if __name__ == '__main__':
    main()
