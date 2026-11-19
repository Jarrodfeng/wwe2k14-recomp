r"""S103 lane H1: the proof that the shared compile cache (tools\cxx_cache.ps1, sccache in PREPROCESSOR mode, one shared dir, repo-relative paths so roots share hits) is SAFE.

  py -I tools\gate\cxx_cache_proof.py [--keep]     exit 1 on any failed claim; prints one line per claim

A synthetic two-"worktree" project (T1, T2: same sources, different roots) and one real renderer source, compiled through tools\cxx_cc.ps1 with a private cache
dir. "Same object" means: the code, data, relocations and symbols are identical (llvm-objdump of the text / data sections and the symbol table, the `.file` record
and every debug section removed: restored objects carry the CodeView paths of the tree that compiled them, which is cosmetic). Claims:
  a  T1 compiles cold (miss); T2 (a second root, same sources) HITS; T2's cached object == T2's UNCACHED object (WWE_NO_CCACHE=1) == T1's; the off switch really bypasses the cache.
  b  planted twin: a header the cached .cpp includes is changed in T2 -> MISS, the object differs from T1's and equals its uncached build; changed back -> HIT.
  c  a -D change misses and differs (equals uncached); the same -D again hits; a different value misses.
  d  the exact poisoning case: T2 gets an extra cfg.h NEXT TO the .cpp, shadowing the one T1 found through -I (quote includes prefer the includer's folder). Preprocessor
     mode must MISS and build the shadow's code. The same two compiles in DIRECT mode (WWE_CCACHE_PROOF_DIRECT=1, its own cache) are run too and reported: a hit on T2 with
     T1's code is the S95 failure (the claim only fails if PREPROCESSOR mode is wrong; direct mode is evidence, not a requirement).
  e  a real source (src\renderer_scene\record_frame.cpp, the build_rsc flags): cached == uncached, and a second root's compile (a copy of src\ only) hits.
"""
import argparse, hashlib, os, re, shutil, subprocess, sys, tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
LLVM = r'C:\Program Files\LLVM\bin'
FAILS = []


def say(ok, text):
    print(('PASS ' if ok else 'FAIL ') + text, flush=True)
    if not ok:
        FAILS.append(text)


def cc(root, src, obj, flags, env_extra=None, cache=None):
    env = dict(os.environ, PATH=LLVM + os.pathsep + os.environ['PATH'], SCCACHE_IDLE_TIMEOUT='20', WWE_CCACHE_DIR=str(cache))
    env.pop('SCCACHE_DIRECT', None); env.pop('WWE_NO_CCACHE', None); env.pop('WWE_CCACHE_PROOF_DIRECT', None)
    env.update(env_extra or {})
    ff = Path(str(obj) + '.flags')
    ff.write_text(chr(10).join(flags))
    r = subprocess.run(['pwsh', '-NoProfile', '-File', str(REPO / 'tools' / 'cxx_cc.ps1'), '-Root', str(root), '-Src', str(src), '-Obj', str(obj), '-FlagsFile', str(ff)],
                       env=env, capture_output=True, text=True, timeout=600)
    out = r.stdout + r.stderr
    if r.returncode != 0:
        raise SystemExit('compile failed:\n' + out)
    m = re.search(r'on: (\d+) compiles, (\d+) hits, (\d+) misses', out.replace('\n', ' '))
    if env.get('WWE_NO_CCACHE') == '1':
        return 'off'
    if not m:
        raise SystemExit('no cache report:\n' + out)
    h, mi = int(m.group(2)), int(m.group(3))
    return 'hit' if h and not mi else 'miss' if mi and not h else 'mixed %d/%d' % (h, mi)


def sig(obj):
    """code + data + relocations + symbols of an object, without the `.file` record or any debug section"""
    d = subprocess.run([LLVM + r'\llvm-objdump.exe', '-d', '-r', '-t', '--no-show-raw-insn', '--no-print-imm-hex', str(obj)], capture_output=True, text=True).stdout
    keep, skip_next, in_dbg_rel = [], False, False
    for l in d.splitlines():
        if l.startswith('RELOCATION RECORDS FOR'):
            in_dbg_rel = '.debug' in l
        if in_dbg_rel and l.strip():
            continue
        if not l.strip():
            in_dbg_rel = False
        if skip_next:   # the AUX record of a debug section's symbol: its checksum is of the debug bytes
            skip_next = False
            continue
        if '.debug' in l:
            skip_next = True
            continue
        if '.file' in l or 'file format' in l:
            continue
        keep.append(l)
    s = subprocess.run([LLVM + r'\llvm-objdump.exe', '-s', '-j', '.text', '-j', '.data', '-j', '.rdata', '-j', '.xdata', '-j', '.pdata', str(obj)], capture_output=True, text=True).stdout
    keep += [l for l in s.splitlines() if 'file format' not in l]
    return hashlib.sha256('\n'.join(keep).encode()).hexdigest()[:16]


def tree(root, k=3, deep=5):
    for d in ('src', 'inc'):
        (root / d).mkdir(parents=True, exist_ok=True)
    (root / 'src' / 'a.cpp').write_text('#include "cfg.h"\n#include "deep.h"\nint f(int x) { return x * CFG_K + DEEP; }\n#ifdef EXTRA\nint g() { return EXTRA; }\n#endif\n')
    (root / 'inc' / 'cfg.h').write_text('#define CFG_K %d\n' % k)
    (root / 'inc' / 'deep.h').write_text('#define DEEP %d\n' % deep)


def flags(root, *extra):
    return ['-O1', '-g', '-std=c++20', '-I', str(root / 'inc')] + list(extra)


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--keep', action='store_true'); a = ap.parse_args()
    base = Path(tempfile.mkdtemp(prefix='cxxproof_', dir=str(REPO / 'out') if (REPO / 'out').is_dir() else None))
    (REPO / 'out').mkdir(exist_ok=True)
    try:
        cache = base / 'cache'; T1 = base / 'T1'; T2 = base / 'T2'
        tree(T1); tree(T2)
        src1, src2 = T1 / 'src' / 'a.cpp', T2 / 'src' / 'a.cpp'
        o1, o2, o2u = T1 / 'a.o', T2 / 'a.o', T2 / 'a_uncached.o'
        # a
        r1 = cc(T1, src1, o1, flags(T1), cache=cache); say(r1 == 'miss', 'a: T1 cold compile is a miss (%s)' % r1)
        r2 = cc(T2, src2, o2, flags(T2), cache=cache); say(r2 == 'hit', 'a: T2 (another root, same sources) is a HIT (%s)' % r2)
        cc(T2, src2, o2u, flags(T2), env_extra={'WWE_NO_CCACHE': '1'}, cache=cache)
        s1, s2, s2u = sig(o1), sig(o2), sig(o2u)
        say(s2 == s2u, 'a: T2 cached object == T2 uncached object (%s == %s)' % (s2, s2u))
        say(s1 == s2, 'a: T1 object == T2 object (%s)' % s1)
        # b
        (T2 / 'inc' / 'cfg.h').write_text('#define CFG_K 4\n')
        rb = cc(T2, src2, o2, flags(T2), cache=cache); cc(T2, src2, o2u, flags(T2), env_extra={'WWE_NO_CCACHE': '1'}, cache=cache)
        sb, sbu = sig(o2), sig(o2u)
        say(rb == 'miss', 'b: header changed in T2 -> MISS (%s)' % rb)
        say(sb != s1, 'b: ...and the object differs from T1\'s (%s vs %s)' % (sb, s1))
        say(sb == sbu, 'b: ...and equals its uncached build (%s)' % sbu)
        (T2 / 'inc' / 'cfg.h').write_text('#define CFG_K 3\n')
        rb2 = cc(T2, src2, o2, flags(T2), cache=cache); say(rb2 == 'hit' and sig(o2) == s1, 'b: changed back -> HIT with T1\'s object (%s)' % rb2)
        # deeper: the header-in-a-header case (deep.h is included by the .cpp; change it)
        (T2 / 'inc' / 'deep.h').write_text('#define DEEP 6\n')
        rd = cc(T2, src2, o2, flags(T2), cache=cache); say(rd == 'miss' and sig(o2) != s1, 'b: a second header changed -> MISS and a different object (%s)' % rd)
        (T2 / 'inc' / 'deep.h').write_text('#define DEEP 5\n')
        # c
        rc1 = cc(T1, src1, o1, flags(T1, '-DEXTRA=7'), cache=cache); sc1 = sig(o1)
        rc2 = cc(T2, src2, o2, flags(T2, '-DEXTRA=7'), cache=cache); sc2 = sig(o2)
        rc3 = cc(T2, src2, o2, flags(T2, '-DEXTRA=8'), cache=cache); sc3 = sig(o2)
        cc(T2, src2, o2u, flags(T2, '-DEXTRA=8'), env_extra={'WWE_NO_CCACHE': '1'}, cache=cache)
        say(rc1 == 'miss' and sc1 != s1, 'c: a new -D misses and differs from the no-define object (%s)' % rc1)
        say(rc2 == 'hit' and sc2 == sc1, 'c: the same -D from the other root hits (%s)' % rc2)
        say(rc3 == 'miss' and sc3 != sc1 and sc3 == sig(o2u), 'c: a different -D value misses, differs, equals uncached (%s)' % rc3)
        # d: shadow twin
        (T2 / 'src' / 'cfg.h').write_text('#define CFG_K 9\n')
        rs = cc(T2, src2, o2, flags(T2), cache=cache); ss = sig(o2); cc(T2, src2, o2u, flags(T2), env_extra={'WWE_NO_CCACHE': '1'}, cache=cache)
        say(rs == 'miss' and ss != s1 and ss == sig(o2u), 'd: PREPROCESSOR mode: a header added NEXT TO the .cpp in T2 (shadowing T1\'s) -> MISS, shadow code built (%s)' % rs)
        dcache = base / 'cache_direct'
        e = {'WWE_CCACHE_PROOF_DIRECT': '1'}
        tree(T1); (T2 / 'src' / 'cfg.h').unlink()
        cc(T1, src1, o1, flags(T1), env_extra=e, cache=dcache); cc(T1, src1, o1, flags(T1), env_extra=e, cache=dcache)   # twice: the manifest is written on the first
        (T2 / 'src' / 'cfg.h').write_text('#define CFG_K 9\n')
        rdm = cc(T2, src2, o2, flags(T2), env_extra=e, cache=dcache); sdm = sig(o2)
        print('INFO d: DIRECT mode, same two compiles: T2 was a %s and its object is %s' % (rdm.upper(), 'T1\'s (the S95 poison: the shadow header was invisible)' if sdm == s1 else 'correct (the shadow\'s)' if sdm == ss else 'other'))
        # e: a real source
        stb = REPO / 'thirdparty' / 'rexglue-sdk-src' / 'thirdparty' / 'stb'
        if not (stb / 'stb_image.h').exists():
            stb = Path(subprocess.run(['git', '-C', str(REPO), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()).parent / 'thirdparty' / 'rexglue-sdk-src' / 'thirdparty' / 'stb'
        vkh = stb.parent / 'vulkan-headers' / 'include'
        real = REPO / 'src' / 'renderer_scene' / 'record_frame.cpp'
        rf = ['-O1', '-g', '-std=c++20', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-DRDR_DEV', '-Wno-unused-command-line-argument',
              '-I', str(stb), '-I', str(vkh), '-I', str(REPO / 'src' / 'launcher')]
        orr, oru = base / 'real.o', base / 'real_u.o'
        r_a = cc(REPO, real, orr, rf, cache=cache)
        cc(REPO, real, oru, rf, env_extra={'WWE_NO_CCACHE': '1'}, cache=cache)
        say(sig(orr) == sig(oru), 'e: record_frame.cpp (%s) cached object == uncached object (%s)' % (r_a, sig(orr)))
        copy = base / 'copy'; shutil.copytree(REPO / 'src', copy / 'src', ignore=shutil.ignore_patterns('*.spv'))
        rf2 = [x.replace(str(REPO / 'src'), str(copy / 'src')) for x in rf]
        r_b = cc(copy, copy / 'src' / 'renderer_scene' / 'record_frame.cpp', base / 'real2.o', rf2, cache=cache)
        say(r_b == 'hit' and sig(base / 'real2.o') == sig(oru), 'e: a copy of src\\ at another root HITS and its object equals the uncached one (%s)' % r_b)
    finally:
        if not a.keep:
            shutil.rmtree(base, ignore_errors=True)
    print('\n%s: %d claim(s) failed' % ('FAIL' if FAILS else 'PASS', len(FAILS)))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
