#!/usr/bin/env python3
"""S103 P9 (review L5): configure-only proof of the WWE2K14_KEEP_STREAM option. Nothing is compiled, built or run.

  py -I tools\\scene\\keep_stream_config_check.py [--root DIR] [--out DIR]

Builds a scratch source tree under <out> (default out\\p9_cfg\\tree) of HARD LINKS: every file git tracks in this tree + the main checkout's generated\\ (read only: links are made
FROM it, nothing is ever written into the main checkout), then `cmake` configures it three times into scratch build dirs:
  off      -DWWE2K14_KEEP_STREAM=OFF -DWWE2K14_KEEP_STREAM_LINK_TEST=ON
  on       -DWWE2K14_KEEP_STREAM=ON  (the default)
  off_bare -DWWE2K14_KEEP_STREAM=OFF with no link-test flag: configure MUST fail (OFF needs KEEP_STREAM_RUN or KEEP_STREAM_LINK_TEST; OFFB 2026-11-17)
and asserts from the generated build.ninja:
  off:  none of the units of tools\\scene\\keep_stream_sources.txt has an object in the wwe2k14 link; src/renderer_game/owner_cvars_off.cpp does; every wwe2k14 compile line of our src
        units carries -DWWE2K14_KEEP_STREAM=0 (and -DWWE2K14_KEEP_STREAM_LINK_TEST=1);
  on:   every compile line carries -DWWE2K14_KEEP_STREAM=1; no owner_cvars_off.cpp; the object list equals the main checkout's configured build today (its own build.ninja: the ON
        configuration must not change what is built).
Exit 0 = all hold. It does not take the run lock (a configure compiles nothing and launches nothing)."""
import argparse
import os
import re
import shutil
import subprocess
import sys

BUILD_DIR = os.path.join('out', 'build', 'win-amd64-release')
OBJ = re.compile(r'^build CMakeFiles/wwe2k14\.dir/(src/[^ ]+?)\.obj:', re.M)


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def main_checkout(root):
    r = run(['git', '-C', root, 'rev-parse', '--git-common-dir'])
    return os.path.dirname(os.path.abspath(os.path.join(root, r.stdout.strip())))


def link_tree(root, main, tree):
    if os.path.isdir(tree):
        shutil.rmtree(tree)
    files = run(['git', '-C', root, 'ls-files']).stdout.split('\n')
    n = 0
    for rel in [f for f in files if f]:
        s = os.path.join(root, rel)
        if not os.path.isfile(s):
            continue
        d = os.path.join(tree, rel)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        os.link(s, d)
        n += 1
    gen = os.path.join(main, 'generated')
    for dp, _dn, fn in os.walk(gen):
        for f in fn:
            s = os.path.join(dp, f)
            d = os.path.join(tree, 'generated', os.path.relpath(s, gen))
            if os.path.exists(d):
                continue
            os.makedirs(os.path.dirname(d), exist_ok=True)
            os.link(s, d)   # a hard link: reads the main checkout's file, writes nothing there
            n += 1
    return n


def find_cmake(cache):
    m = re.search(r'^CMAKE_COMMAND:INTERNAL=(.*)$', cache, re.M)   # the cmake the main build was configured with
    if m and os.path.isfile(m.group(1).strip()):
        return m.group(1).strip()
    return shutil.which('cmake') or 'cmake'


def configure(tree, bdir, main, extra):
    if os.path.isdir(bdir):
        shutil.rmtree(bdir)
    cache = open(os.path.join(main, BUILD_DIR, 'CMakeCache.txt'), encoding='utf-8', errors='replace').read()

    def cv(name):
        m = re.search(r'^%s:[A-Z]+=(.*)$' % re.escape(name), cache, re.M)
        return m.group(1).strip() if m else ''
    cmd = [find_cmake(cache), '-S', tree, '-B', bdir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
           '-DCMAKE_C_COMPILER=' + cv('CMAKE_C_COMPILER'), '-DCMAKE_CXX_COMPILER=' + cv('CMAKE_CXX_COMPILER'),
           '-DCMAKE_MAKE_PROGRAM=' + cv('CMAKE_MAKE_PROGRAM'), '-Drexglue_DIR=' + cv('rexglue_DIR'),
           '-DWWE2K14_PY=' + cv('WWE2K14_PY'), '-DCMAKE_RC_COMPILER=' + cv('CMAKE_RC_COMPILER'), '-DCMAKE_AR=' + cv('CMAKE_AR'),
           '-DCMAKE_LINKER=' + cv('CMAKE_LINKER')] + extra
    return run(cmd)


def objs_and_defines(bdir):
    txt = open(os.path.join(bdir, 'build.ninja'), encoding='utf-8', errors='replace').read()
    objs = {m.group(1) for m in OBJ.finditer(txt)}
    defs = {}
    for m in re.finditer(r'^build CMakeFiles/wwe2k14\.dir/(src/[^ ]+?)\.obj:.*?\n((?:  .*\n)+)', txt, re.M):
        d = re.search(r'^  DEFINES = (.*)$', m.group(2), re.M)
        defs[m.group(1)] = d.group(1) if d else ''
    return objs, defs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--out', default='')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_dir = main_checkout(root)
    out = os.path.abspath(a.out or os.path.join(root, 'out', 'p9_cfg'))
    tree = os.path.join(out, 'tree')
    bad = []

    def need(ok, what):
        print(('ok   ' if ok else 'FAIL ') + what)
        if not ok:
            bad.append(what)
    if not os.path.isfile(os.path.join(main_dir, BUILD_DIR, 'build.ninja')):
        print('FAIL: the main checkout has no configured %s (the reference object list)' % BUILD_DIR)
        return 1
    n = link_tree(root, main_dir, tree)
    print('scratch tree: %d hard links under %s' % (n, tree))
    dropped = [ln.strip() for ln in open(os.path.join(root, 'tools', 'scene', 'keep_stream_sources.txt'), encoding='utf-8') if ln.strip().startswith('src/')]
    off_only = [ln.strip() for ln in open(os.path.join(root, 'tools', 'scene', 'keep_stream_off_sources.txt'), encoding='utf-8') if ln.strip().startswith('src/')]
    ref_objs, _ = objs_and_defines(os.path.join(main_dir, BUILD_DIR))
    mk = re.search(r'^WWE2K14_KEEP_STREAM:BOOL=(\w+)', open(os.path.join(main_dir, BUILD_DIR, 'CMakeCache.txt'), encoding='utf-8', errors='replace').read(), re.M)
    if mk and mk.group(1).upper() == 'OFF':   # S111: main's folder is the stream-FREE player build; the ON reference is its list with the OFF-only units out and the stream units in
        ref_objs = (set(ref_objs) - set(off_only)) | set(dropped)
    # S103 SPB: a new .cpp this tree's CMakeLists.txt lists that the main checkout's configured build.ninja does not know yet (it re-configures at its next build; same rule as keep_stream_off_link.py's
    # "new unit" pass) belongs to the reference list: the check is about the OPTION, not about whether main has re-configured.
    cm = open(os.path.join(root, 'CMakeLists.txt'), encoding='utf-8').read()
    blk = re.search(r'set\(WWE2K14_SOURCES(.*?)\n\s*\)', cm, re.S)
    ref_objs = set(ref_objs)
    for rel in re.findall(r'^\s*(src/\S+?\.cpp)\s*(?:#.*)?$', blk.group(1) if blk else '', re.M):
        if rel not in ref_objs and os.path.isfile(os.path.join(root, rel)):
            print('new unit (not in the main build.ninja yet): ' + rel)
            ref_objs.add(rel)

    bare = configure(tree, os.path.join(out, 'off_bare'), main_dir, ['-DWWE2K14_KEEP_STREAM=OFF'])
    need(bare.returncode != 0 and 'needs -DWWE2K14_KEEP_STREAM_RUN=ON' in (bare.stdout + bare.stderr), 'OFF without KEEP_STREAM_RUN or KEEP_STREAM_LINK_TEST fails to configure with the needs-a-flag message')

    r = configure(tree, os.path.join(out, 'off'), main_dir, ['-DWWE2K14_KEEP_STREAM=OFF', '-DWWE2K14_KEEP_STREAM_LINK_TEST=ON'])
    need(r.returncode == 0, 'OFF + link-test configures' + ('' if r.returncode == 0 else '\n' + (r.stdout + r.stderr)[-1500:]))
    if r.returncode == 0:
        objs, defs = objs_and_defines(os.path.join(out, 'off'))
        present = [d for d in dropped if d in objs]
        need(not present, 'OFF: none of the %d dropped units has an object (present: %s)' % (len(dropped), ', '.join(present) or 'none'))
        need(all(o in objs for o in off_only), 'OFF: %s is in the link' % ', '.join(off_only))
        wrong = [s for s, d in defs.items() if '-DWWE2K14_KEEP_STREAM=0' not in d or '-DWWE2K14_KEEP_STREAM_LINK_TEST=1' not in d]
        need(bool(defs) and not wrong, 'OFF: every wwe2k14 compile line (%d) carries -DWWE2K14_KEEP_STREAM=0 and the link-test define (wrong: %s)' % (len(defs), ', '.join(wrong[:5]) or 'none'))
        need(objs == (ref_objs - set(dropped)) | set(off_only), 'OFF: the object list (%d) is the main build\'s minus the dropped units plus the OFF-only units' % len(objs))
    r = configure(tree, os.path.join(out, 'on'), main_dir, ['-DWWE2K14_KEEP_STREAM=ON'])   # explicit: the ON variant's own identity
    need(r.returncode == 0, 'ON (default) configures' + ('' if r.returncode == 0 else '\n' + (r.stdout + r.stderr)[-1500:]))
    if r.returncode == 0:
        objs, defs = objs_and_defines(os.path.join(out, 'on'))
        wrong = [s for s, d in defs.items() if '-DWWE2K14_KEEP_STREAM=1' not in d or 'LINK_TEST' in d]
        need(bool(defs) and not wrong, 'ON: every wwe2k14 compile line (%d) carries -DWWE2K14_KEEP_STREAM=1 and no link-test define (wrong: %s)' % (len(defs), ', '.join(wrong[:5]) or 'none'))
        need(not any('owner_cvars_off' in o for o in objs), 'ON: owner_cvars_off.cpp is not built')
        need(objs == ref_objs, 'ON: the object list (%d) equals the main checkout\'s configured build (extra here: %s; missing here: %s)' % (len(objs), sorted(objs - ref_objs)[:5], sorted(ref_objs - objs)[:5]))
    print('FAIL: %d' % len(bad) if bad else 'PASS: the option configures as designed in both settings')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
