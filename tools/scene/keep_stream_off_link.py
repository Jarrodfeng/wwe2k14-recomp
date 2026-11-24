#!/usr/bin/env python3
"""S103 P9: build and LINK the game exe with WWE2K14_KEEP_STREAM=0 (the stream left out), offline. Never runs the exe.

  py -I tools\\scene\\keep_stream_off_link.py [--root DIR] [--keep 0|1] [--out DIR] [--only a.cpp,b.cpp] [--syntax] [-j N]

Why not cmake: the guest code (generated\\default, ~hundreds of units) is identical in both configurations, so it is not rebuilt. The MAIN checkout's configured release build
(`ninja -t commands wwe2k14.exe` in out\\build\\win-amd64-release) gives the exact flags and the object list; this script
  1. recompiles every unit of OUR sources (src\\*; this worktree's files, the main checkout's flags) with -DWWE2K14_KEEP_STREAM=<keep> into <out>\\obj (the precompiled header of
     main.cpp is replaced by an -include of src\\pch_main.h: a PCH built without the define would not match),
  2. leaves out the units tools\\scene\\keep_stream_sources.txt names when --keep 0 (the same file CMakeLists.txt reads),
  3. links them with the main build's guest objects + SDK libraries into <out>\\wwe2k14_keep<keep>.exe (no staging, no DLL copy, nothing is started).
--syntax stops after the compile pass as -fsyntax-only (fast iteration); --only limits the compile pass to the named units. The undefined symbols the link prints ARE
the list of what still references the stream (COMPILE_OUT_LIST.md section C step 1). Prints wall time and the exe size. Exit 0 = every unit compiled and the link passed.
Needs the main checkout's out\\build\\win-amd64-release (configured once; it is the build game_syntax already reads)."""
import argparse
import os
import re
import shlex
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'compile'))
import game_syntax   # S103 GSP: --syntax shares game_syntax's preprocessed-hash cache (syntax_check / load_cache / save_cache)
sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'admit'))
import admit_py as _admit   # CQW: the machine-wide compile queue for the unit pool (nothing runs at import)

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
BUILD_DIR = os.path.join('out', 'build', 'win-amd64-release')


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def main_checkout(root):
    r = run(['git', '-C', root, 'rev-parse', '--git-common-dir'])
    if r.returncode:
        return None
    return os.path.dirname(os.path.abspath(os.path.join(root, r.stdout.strip())))


def find_ninja():
    import glob
    for pat in (r'C:\Program Files\Microsoft Visual Studio\2022\*\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe',
                r'C:\Program Files (x86)\Microsoft Visual Studio\2022\*\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'):
        hits = glob.glob(pat)
        if hits:
            return hits[0]
    return None


def stream_only(root):
    out = set()
    for ln in open(os.path.join(root, 'tools', 'scene', 'keep_stream_sources.txt'), encoding='utf-8'):
        ln = ln.strip()
        if ln.startswith('src/'):
            out.add(ln)
    return out


def parse_ninja(bd):
    """(objects of the wwe2k14.exe link, LINK_LIBRARIES, LINK_FLAGS) from build.ninja."""
    txt = open(os.path.join(bd, 'build.ninja'), encoding='utf-8').read()
    m = re.search(r'^build wwe2k14\.exe: \S+ (.*?)(?: \|.*)?$', txt, re.M)
    ins = m.group(1).split()
    i = txt.index(m.group(0))
    blk = txt[i:i + 200000].split('\n\n')[0]
    libs = re.search(r'^  LINK_LIBRARIES = (.*)$', blk, re.M).group(1)
    flags = re.search(r'^  LINK_FLAGS = (.*)$', blk, re.M).group(1)
    return [x for x in ins if not x.startswith('||')], libs, flags


def compile_lines(ninja, bd):
    r = run([ninja, '-C', bd, '-t', 'commands', 'wwe2k14.exe'])
    if r.returncode:
        raise RuntimeError('ninja -t commands failed: ' + r.stderr[:500])
    return [ln for ln in r.stdout.splitlines() if ' -c ' in ln and 'clang' in ln.lower()]


SUBDIRS = []
DEPS = {}
CACHE_HITS = []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--keep', type=int, default=0, choices=(0, 1))
    ap.add_argument('--out', default='')
    ap.add_argument('--only', default='')
    ap.add_argument('--syntax', action='store_true')
    ap.add_argument('--deps', action='store_true', help='no compile: write <out>/deps.json = every units include closure (clang -MM) for tools/scene/keep_stream_lint.py')
    ap.add_argument('--extra', default='', help='one more source file, compiled with the flags of src/main.cpp (the lint planted twin)')
    ap.add_argument('-j', type=int, default=12)
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_dir = main_checkout(root)
    bd = os.path.join(main_dir, BUILD_DIR)
    if not os.path.isfile(os.path.join(bd, 'build.ninja')):
        print('FAIL: no build.ninja in ' + bd)
        return 1
    ninja = find_ninja()
    if not ninja:
        print('FAIL: ninja not found')
        return 1
    out = os.path.abspath(a.out or os.path.join(root, 'out', 'p9_off' if a.keep == 0 else 'p9_on'))
    os.makedirs(os.path.join(out, 'obj'), exist_ok=True)
    mfwd = main_dir.replace('\\', '/')
    SUBDIRS.extend(d for d in os.listdir(os.path.join(main_dir, 'src')) if os.path.isdir(os.path.join(main_dir, 'src', d)))
    rfwd = root.replace('\\', '/')
    drop = stream_only(root) if a.keep == 0 else set()
    only = {x.strip().replace('\\', '/') for x in a.only.split(',') if x.strip()}
    objs, libs, lflags = parse_ninja(bd)
    cmds = None
    if a.syntax:   # S106 GSN N14: the flags of the CHECKED tree's own configure (game_syntax.build_lines_for = syntax_cfg), not main's out\build; the object list above stays main's (a new unit is added below)
        try:
            cmds = [ln for ln in game_syntax.build_lines_for(root, main_dir, 'on') if 'clang' in ln.lower()]
            print('flags: this tree\'s own configure (syntax_cfg)')
        except RuntimeError as e:   # ConfigureFailed: the tree does not configure = a real FAIL, never stale flags
            print('FAIL: this tree\'s CMakeLists.txt does not configure: %s' % e)
            return 1
        if not cmds:
            print('note: FALLING BACK to main\'s out\\build flags (--syntax): syntax_cfg gave no compile lines')
            cmds = None
    if cmds is None:
        if a.syntax:
            print('note: FALLING BACK to the main checkout\'s out\\build flags (they may not describe this tree)')
        cmds = compile_lines(ninja, bd)
    by_src = {}
    for ln in cmds:
        m = re.search(r' -c (\S+)\s*$', ln)
        if m:
            by_src[m.group(1).replace('\\', '/')] = ln
    jobs, keep_objs, ours_objs = [], [], []
    off_only = []
    if a.keep == 0:   # units that exist only without the stream: compiled with the flags of src/dev_build.cpp (a plain unit of ours)
        for ln in open(os.path.join(root, 'tools', 'scene', 'keep_stream_off_sources.txt'), encoding='utf-8'):
            if ln.strip().startswith('src/'):
                off_only.append(ln.strip())
    t0 = time.time()
    for o in objs:
        of = o.replace('\\', '/')
        m = re.match(r'CMakeFiles/wwe2k14\.dir/(src/.*)\.obj$', of)
        if not m:
            keep_objs.append(os.path.join(bd, o))     # the guest code, SDK glue, icon: reused as built
            continue
        rel = m.group(1)
        if rel in drop:
            continue
        srcp = mfwd + '/' + rel
        ln = by_src.get(srcp)
        if ln is None:
            print('FAIL: no compile command for ' + rel)
            return 1
        if not os.path.isfile(os.path.join(root, rel)):
            print('FAIL: missing ' + rel)
            return 1
        if only and rel not in only:
            ours_objs.append(None)
            continue
        jobs.append((rel, ln))
    tmpl = by_src.get(mfwd + '/src/dev_build.cpp')
    for rel in off_only:
        if not os.path.isfile(os.path.join(root, rel)):
            print('FAIL: missing ' + rel)
            return 1
        if not only or rel in only:
            jobs.append((rel, tmpl.replace(mfwd + '/src/dev_build.cpp', mfwd + '/' + rel)))
    # S103 PN23: units this tree's CMakeLists.txt lists in WWE2K14_SOURCES that the main checkout's configured build.ninja does not know yet (a new .cpp added in a worktree, e.g. PN1's ui_file_index.cpp): compiled with the
    # flags of src/dev_build.cpp and linked, so the OFF link test and the keep lint see them before the main checkout re-configures. A unit the main build already compiles is never doubled.
    known = set()
    for o in objs:
        km = re.match(r'CMakeFiles/wwe2k14\.dir/(src/.*)\.obj$', o.replace('\\', '/'))
        if km:
            known.add(km.group(1))
    cm = open(os.path.join(root, 'CMakeLists.txt'), encoding='utf-8').read()
    blk = re.search(r'set\(WWE2K14_SOURCES(.*?)\n\s*\)', cm, re.S)
    for rel in re.findall(r'^\s*(src/\S+?\.cpp)\s*(?:#.*)?$', blk.group(1) if blk else '', re.M):
        if rel in known or rel in drop or rel in off_only or not os.path.isfile(os.path.join(root, rel)):
            continue
        print('new unit (not in the main build.ninja yet): ' + rel, flush=True)
        if not only or rel in only:
            jobs.append((rel, tmpl.replace(mfwd + '/src/dev_build.cpp', mfwd + '/' + rel)))
    for ex_src in [x for x in a.extra.split(',') if x]:
        mtmpl = by_src.get(mfwd + '/src/main.cpp')
        jobs.append((os.path.abspath(ex_src).replace('\\', '/'), mtmpl.replace(mfwd + '/src/main.cpp', ex_src)))
    sources_expected = {re.match(r'CMakeFiles/wwe2k14\.dir/(src/.*)\.obj$', o.replace('\\', '/')).group(1) for o in objs if re.match(r'CMakeFiles/wwe2k14\.dir/(src/.*)\.obj$', o.replace('\\', '/'))}
    for d in drop:
        if d not in sources_expected:
            print('FAIL: keep_stream_sources.txt names %s which the exe does not compile' % d)
            return 1

    if a.syntax:
        game_syntax.set_force(rfwd, [r_ for r_, _l in jobs])   # S106 G4-4F (review F1): the real build force-includes this tree's src/pch_units.h into every game unit; same unit set as game_syntax --changed

    def one(job):
        rel, ln = job
        toks = shlex.split(ln.replace('\\"', '"'), posix=False)[1:]
        keep, i = [], 0
        while i < len(toks):
            t = toks[i]
            if t in ('-MT', '-MF', '-o', '-c'):
                i += 2
                continue
            if t in ('-MD', '-MMD'):
                i += 1
                continue
            if t == '-Xclang' and toks[i + 1] in ('-include-pch', '-include'):
                i += 4
                continue
            keep.append(t)
            i += 1
        fixed = []
        for t in keep:
            if t in ('-I' + mfwd, '-I' + mfwd + '/src'):
                t = '-I' + rfwd + t[2 + len(mfwd):]
            fixed.append(t)
        # the main checkout still owns generated\default and thirdparty; this tree's roots come first
        # a worktree has no thirdparty folder: `#include "../../thirdparty/..."` (written for src/<dir>/x.cpp) is found through the main checkout's src subfolders
        fixed += ['-I' + mfwd, '-I' + mfwd + '/src'] + ['-I%s/src/%s' % (mfwd, d) for d in SUBDIRS] + [ '-DWWE2K14_KEEP_STREAM=%d' % a.keep, '-DWWE2K14_KEEP_STREAM_LINK_TEST=1', '-Wno-everything']
        if rel == 'src/main.cpp':
            fixed += ['-include', rfwd + '/src/pch_main.h']
        of = os.path.join(out, 'obj', rel.replace('/', '__') + '.obj')
        if a.deps:
            cmd = [CLANG] + fixed + ['-MM', '-MG', (rfwd + '/' + rel) if not os.path.isabs(rel) else rel]
        elif a.syntax:
            # S103 GSP: the same verdict cache game_syntax.py keeps (key = hash of `clang -E -P` of the unit + its flags + the clang): an unchanged unit is not parsed again, an edited header changes the preprocessed text so it cannot hit
            rc, text, key, hit = game_syntax.syntax_check(fixed, (rfwd + '/' + rel) if not os.path.isabs(rel) else rel, roots=((rfwd, '<ROOT>'), (mfwd, '<MAIN>')))   # (the verdict is stored inside, after a second preprocess agrees)
            CACHE_HITS.append(hit)
            return rel, rc, (text if rc else ''), of
        else:
            cmd = [CLANG] + fixed + ['-o', of, '-c', rfwd + '/' + rel]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if a.deps and r.returncode == 0:
            txt = r.stdout.replace('\\' + '\n', ' ').replace('\\' + '\r' + '\n', ' ')
            toks = [t for t in txt.split()[1:] if t != '\\']
            DEPS[rel] = [t.replace('\\', '/') for t in toks]
        return rel, r.returncode, (r.stdout + r.stderr if r.returncode else ''), of

    print('compiling %d of our units (KEEP_STREAM=%d; %d dropped) ...' % (len(jobs), a.keep, len(drop)), flush=True)
    cache_file = game_syntax.load_cache(main_dir) if a.syntax else None
    bad = 0
    ours = []
    with _admit.hold('cc.game_src', a.j, timeout_s=300) as nj, ThreadPoolExecutor(max_workers=nj) as ex:   # CQW: game src units (a.j = the want and the fall-open value)
        for rel, rc, text, of in ex.map(one, jobs):
            if rc:
                bad += 1
                print('FAIL %s\n%s' % (rel, text[:3000]), flush=True)
            else:
                ours.append(of)
    t1 = time.time()
    print('compile pass: %d failed of %d, %.0f s' % (bad, len(jobs), t1 - t0), flush=True)
    if cache_file:
        game_syntax.save_cache(cache_file)
        print('pp-cache: %d of %d units served by the preprocess hash (no parse)' % (sum(CACHE_HITS), len(CACHE_HITS)), flush=True)
    if a.deps:
        import json
        json.dump(DEPS, open(os.path.join(out, 'deps.json'), 'w'))
        print('deps: %d units' % len(DEPS))
        return 1 if bad else 0
    if bad or a.syntax or only:
        return 1 if bad else 0
    rsp = os.path.join(out, 'link.rsp')
    with open(rsp, 'w') as f:
        for o in keep_objs + ours:
            f.write('"%s"\n' % o.replace('\\', '/'))
        f.write(libs.replace('\\', '/') + '\n')
    exe = os.path.join(out, 'wwe2k14_keep%d.exe' % a.keep)
    lt = shlex.split(lflags.replace('\\', '/'), posix=False)
    lf, i = [], 0
    while i < len(lt):   # drop the map / pdb / debug options (a link test: no map beside the real one, no pdb)
        if lt[i] == '-Xlinker' and i + 1 < len(lt) and (lt[i + 1].startswith('/MAP:') or lt[i + 1].startswith('/PDB:') or lt[i + 1] == '/DEBUG'):
            i += 2
            continue
        lf.append(lt[i])
        i += 1
    cmd = [CLANG, '-nostartfiles', '-nostdlib', '-O3', '-DNDEBUG', '-D_DLL', '-D_MT', '-Xclang', '--dependent-lib=msvcrt'] + \
          lf + ['@' + rsp, '-o', exe, '-Xlinker', '/MANIFEST:EMBED', '-Xlinker', '/version:0.0']
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=bd)
    t2 = time.time()
    text = r.stdout + r.stderr
    und = sorted(set(re.findall(r'undefined symbol: ([^\n]+)', text)))
    if r.returncode:
        print('LINK FAILED (%d undefined symbols), %.0f s' % (len(und), t2 - t1))
        for u in und[:200]:
            print('  undefined: ' + u.strip())
        if not und:
            print(text[:3000])
        return 1
    print('LINK OK: %s  %.1f MB  link %.0f s, compile %.0f s, total %.0f s' % (exe, os.path.getsize(exe) / 1048576.0, t2 - t1, t1 - t0, t2 - t0))
    return 0


if __name__ == '__main__':
    sys.exit(main())
