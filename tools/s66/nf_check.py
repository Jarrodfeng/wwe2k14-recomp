"""Syntax-check game headers (src\\*.h) with the game's real compile flags, without building (Session 66, track F2).

For agents writing native functions (src\\nf\\*.h, src\\native_fn.h): the game build holds the run lock and writes the
exe other runs use, so agents check their headers with this instead. Takes main.cpp's DEFINES / INCLUDES / FLAGS from
out\\build\\win-amd64-release\\build.ninja, drops the precompiled header (includes its source header instead), writes a
one-line translation unit per header and runs clang++ -fsyntax-only. Exit code = headers that failed.

    py tools\\s66\\nf_check.py src\\nf\\memcpy_vmx.h [more headers ...]
"""
import os
import re
import shlex
import subprocess
import sys
import tempfile

R = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NINJA = os.path.join(R, 'out', 'build', 'win-amd64-release', 'build.ninja')
CLANG = 'C:/Program Files/LLVM/bin/clang++.exe'


def flags():
    lines = open(NINJA, encoding='utf-8', errors='replace').read().splitlines()
    i = next(k for k, l in enumerate(lines) if l.startswith('build CMakeFiles/wwe2k14.dir/src/main.cpp.obj'))
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
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    args, pch_src = flags()
    failed = 0
    for h in sys.argv[1:]:
        path = os.path.abspath(h).replace('\\', '/')
        with tempfile.NamedTemporaryFile('w', suffix='.cpp', delete=False) as tu:
            tu.write(f'#include "{pch_src}"\n#include "{path}"\n')
            name = tu.name
        try:
            p = subprocess.run([CLANG, '-fsyntax-only', '-Wno-deprecated-declarations', *args, name],
                               capture_output=True, text=True)
        finally:
            os.unlink(name)
        ok = p.returncode == 0
        # (Session 66) a function another src header already hooks cannot be hooked again (the link fails).
        mine = set(re.findall(r'WWE2K14_NATIVE_FN\((sub_[0-9A-F]{8})', open(h, encoding='utf-8').read()))
        for root, _, files in os.walk(os.path.join(R, 'src')):
            for name in files:
                other = os.path.join(root, name)
                if not name.endswith('.h') or os.path.abspath(other) == os.path.abspath(h):
                    continue
                text = open(other, encoding='utf-8', errors='replace').read()
                for sub in mine:
                    if re.search(r'(REX_HOOK_RAW|REX_HOOK|WWE2K14_NATIVE_FN)\(' + sub + r'\b', text):
                        print(f'[nf_check] {h}: {sub} is already hooked in {os.path.relpath(other, R)}')
                        ok = False
        failed += not ok
        print(f'[nf_check] {h}: {"OK" if ok else "FAILED"}')
        if not ok:
            print('\n'.join(p.stderr.splitlines()[:40]))
    sys.exit(failed)


if __name__ == '__main__':
    main()
