"""Session 102 lanes W1/W2: read the compile flags a configured build folder would use (build.ninja) for one generated guest TU and one of OUR
source TUs, and check them against what the release wants. Read-only, no build, no game.
    py -I tools\\release\\check_build_flags.py BUILD_DIR --guest v3|base --ours v3|base [--profdata PATH|none] [--pgo-ours yes|no]
Exit 0 = every expectation holds; 1 = one does not (the lines say which). `--selftest` is run by tools\\release\\prove_build_flags.ps1 with a planted
mismatch (a base build checked against v3 expectations) that must exit 1.
"""
import argparse, re, sys

V3 = ['-mavx2', '-mfma', '-mbmi', '-mbmi2', '-mmovbe', '-mlzcnt', '-mpopcnt', '-ffp-contract=off']
GUEST_TU = r'build CMakeFiles/wwe2k14_recomp\.dir/generated/default/wwe2k14_recomp\.\d+\.cpp\.obj:'
OURS_TU = r'build CMakeFiles/wwe2k14\.dir/src/renderer_scene/[A-Za-z0-9_]+\.cpp\.obj:'


def flags_of(lines, pat):
    for i, l in enumerate(lines):
        if re.match(pat, l):
            for j in range(i + 1, min(i + 12, len(lines))):
                if lines[j].startswith('  FLAGS = '):
                    return l.split(':')[0], lines[j]
            raise SystemExit(f'FAIL: no FLAGS line under {l[:90]}')
    raise SystemExit(f'FAIL: no build statement matching {pat} (is the folder configured, and does it hold the generated code?)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('build_dir')
    ap.add_argument('--guest', choices=['v3', 'base'], required=True)
    ap.add_argument('--ours', choices=['v3', 'base'], required=True)
    ap.add_argument('--profdata', default='none')
    ap.add_argument('--pgo-ours', choices=['yes', 'no'], default='no')
    ap.add_argument('--expect-stream', action='store_true', help='the folder is a stream build (default: the player build is stream-free)')
    a = ap.parse_args()
    try:
        lines = open(a.build_dir.rstrip('\\/') + '/build.ninja', encoding='utf-8', errors='replace').read().splitlines()
    except OSError as e:
        print('FAIL: cannot read build.ninja:', e)
        return 1
    bad = 0
    # 2026-11-18: the release is stream-free: the CMake cache must say KEEP_STREAM=OFF and KEEP_STREAM_RUN=ON (a stale ON cache fails here).
    try:
        cache = open(a.build_dir.rstrip('\\/') + '/CMakeCache.txt', encoding='utf-8', errors='replace').read()
    except OSError as e:
        print('FAIL: cannot read CMakeCache.txt:', e)
        return 1
    for var, want in (('WWE2K14_KEEP_STREAM', 'ON' if a.expect_stream else 'OFF'), ('WWE2K14_KEEP_STREAM_RUN', 'OFF' if a.expect_stream else 'ON')):
        m = re.search(r'^' + var + r':BOOL=(\S+)', cache, re.M)
        got = m.group(1) if m else 'unset'
        if got != want:
            print(f'FAIL cache: {var}={got}, the release wants {want}')
            bad += 1
    for tag, pat, want_isa, want_prof in (('guest', GUEST_TU, a.guest, a.profdata != 'none'),
                                          ('ours', OURS_TU, a.ours, a.profdata != 'none' and a.pgo_ours == 'yes')):
        name, fl = flags_of(lines, pat)
        if tag == 'ours':   # 2026-11-18: an OURS TU's DEFINES must carry WWE2K14_KEEP_STREAM=0 (stream-free) or =1 (--expect-stream): the define the code reads
            want_def = '-DWWE2K14_KEEP_STREAM=' + ('1' if a.expect_stream else '0')
            st = next(k for k, l in enumerate(lines) if l.startswith(name + ':'))   # name already starts with 'build ' (flags_of); v0.4.0 prepare: 'build build ...' never matched
            dl = next((l for l in lines[st + 1:st + 12] if l.startswith('  DEFINES = ')), '')
            if (' ' + want_def + ' ') not in (dl + ' '):
                print(f'FAIL ours: {name.split("/")[-1]} DEFINES lack {want_def}')
                bad += 1
        for f in V3:
            has = (' ' + f + ' ') in (fl + ' ')
            if has != (want_isa == 'v3'):
                print(f'FAIL {tag}: {name.split("/")[-1]} {"lacks" if want_isa == "v3" else "has"} {f}')
                bad += 1
        m = re.search(r'-fprofile-instr-use=(\S+)', fl)
        if want_prof:
            want = a.profdata.replace('\\', '/').lower()
            if not m or m.group(1).replace('\\', '/').lower() != want:
                print(f'FAIL {tag}: wanted -fprofile-instr-use={a.profdata}, flags have {m.group(1) if m else "none"}')
                bad += 1
        elif m:
            print(f'FAIL {tag}: has -fprofile-instr-use={m.group(1)} but none was wanted')
            bad += 1
        if '-fprofile-instr-generate' in fl:
            print(f'FAIL {tag}: is instrumented (-fprofile-instr-generate) in a use/release build')
            bad += 1
        print(f'{"ok  " if not bad else "    "}{tag}: {name.split("/")[-1]}  isa={want_isa} profile={"yes" if want_prof else "no"}')
    print('check_build_flags:', 'PASS' if not bad else f'FAIL ({bad})')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
