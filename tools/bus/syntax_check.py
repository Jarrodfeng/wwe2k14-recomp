"""S101 lane 120: a syntax-only compile of src/event_bus.h (the shipped bus header, with the SDK headers it includes), part of the gate lane
rumble_bus. No game, no link, nothing written outside a temp folder.

    py -I tools/bus/syntax_check.py [--root DIR]     DIR = the checkout whose src/ is compiled (default: this one)

C++23, -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY, the SDK include folder of the main checkout (thirdparty/rexglue-sdk-vulkan/include). Fails loudly
when the compiler or the SDK headers are missing (the gate lane lists the SDK folder in "needs", so a machine without it SKIPs, not passes).
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
MAIN = r'C:\Users\Jarrod\wwe2k14-recomp'


def main(argv):
    root = REPO
    if '--root' in argv:
        root = argv[argv.index('--root') + 1]
    sdk = os.path.join(MAIN, 'thirdparty', 'rexglue-sdk-vulkan', 'include')
    if not os.path.isdir(sdk):
        print('FAIL: the SDK include folder %s is missing' % sdk)
        return 1
    cl = None
    for c in (shutil.which('clang++'), r'C:\Program Files\LLVM\bin\clang++.exe', '/usr/bin/clang++'):
        if c and os.path.exists(c):
            cl = c
            break
    if not cl:
        print('FAIL: no clang++ found (tools\\env.ps1 puts the project\'s LLVM on PATH)')
        return 1
    tmp = tempfile.mkdtemp(prefix='l120_syntax_')
    tu = os.path.join(tmp, 'bus_tu.cpp')
    open(tu, 'w').write('#include "event_bus.h"\nint main() { wwe2k14::events::Tick(1); return 0; }\n')
    cmd = [cl, '-std=gnu++23', '-fsyntax-only', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-DSPDLOG_FMT_EXTERNAL', '-DFMT_HEADER_ONLY', '-DSPDLOG_COMPILED_LIB',
           '-I', os.path.join(root, 'src'), '-isystem', sdk, '-isystem', os.path.join(sdk, 'dxc'), tu]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    shutil.rmtree(tmp, ignore_errors=True)
    if r.returncode:
        print('FAIL: src/event_bus.h does not compile:\n' + '\n'.join(l for l in (r.stderr or '').splitlines() if 'error' in l)[:3000])
        return 1
    print('event_bus.h syntax check: 0 problems (clang -fsyntax-only, C++23)')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
