#!/usr/bin/env python3
"""S103 P9 lint: with WWE2K14_KEEP_STREAM OFF, no translation unit of the game exe includes a header of the command stream.

  py -I tools\\scene\\keep_stream_lint.py [--root DIR] [--plant reader_h] [--from-deps FILE]

Checks (all read the tree, nothing is run):
  1. include closure: tools\\scene\\keep_stream_off_link.py --deps (clang -MM with -DWWE2K14_KEEP_STREAM=0, the main build's flags, this tree's src first) gives every compiled unit's
     closure (the units of tools\\scene\\keep_stream_sources.txt are left out, as the OFF build leaves them out); none may contain a path of tools\\scene\\keep_stream_headers.txt.
     Paths are normalised (normcase + normpath, one slash style) before they are matched. deps.json is deleted before every pass and every return code is checked.
     Known open items (the B4a / B4b splits, tools\\scene\\keep_stream_known_open.txt, one `unit` per line with its reason) are reported by name as KNOWN OPEN, never silently
     passed; a unit NOT on that list that reaches the stream FAILS, and a listed unit that no longer reaches it FAILS too (remove it from the list).
  2. text: no raw `#if WWE2K14_KEEP_STREAM` / `#ifdef` / `defined()` of the macro anywhere in src\\ (use `#if WWE2K14_STREAM()`, defined only in keep_stream.h: a file that does not
     include it gets a hard preprocessor error, which the off_syntax / game_syntax checks catch).
  3. removed switches (L3): every REXCVAR_GET / REXCVAR_SET of a name in renderer_game\\removed_switches.h (and the pipeline store folder) in a file of the OFF closure carries an
     `OFF:` comment on its own line or one of the 3 lines above, stating what the switch means when the stream is left out (the stub reads constant false / empty).
  4. exclusivity (L6): see tools\\renderer\\scene_skip_unit.py (the cvar definitions, the two source lists).
Planted twins (--plant reader_h; its own check): scratch files under src\\renderer_scene\\ are added to the OFF scan and MUST be reported: (a) a unit that includes
"../renderer_adapter/reader.h", (b) a unit that includes a kept header which includes renderer_adapter/d3_arena.h (transitive). The files are written, scanned and always deleted.
Exit 0 = clean (and, with --plant, both twins were caught)."""
import argparse
import json
import os
import re
import subprocess
import sys


def lines(path):
    return [ln.strip() for ln in open(path, encoding='utf-8') if ln.strip() and not ln.strip().startswith('#')]


def norm(p):
    return os.path.normcase(os.path.normpath(p)).replace('\\', '/')


def hit_headers(deps_list, forbidden_norm):
    """forbidden_norm: {normalised '/src/<rel>' suffix: rel}"""
    found = set()
    for d in deps_list:
        n = norm(d)
        for suf, rel in forbidden_norm.items():
            if n.endswith(suf):
                found.add(rel)
    return sorted(found)


def on_only_lines(ls):
    """Indices of the lines inside a `#if WWE2K14_STREAM()` branch that exists only when the stream is kept (not compiled in the OFF build)."""
    stack, skip = [], set()   # entries: [is_stream_cond, currently_on_only_branch]
    for i, ln in enumerate(ls):
        m = re.match(r'#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)', ln.strip())
        if m:
            d, rest = m.group(1), m.group(2).strip()
            if d in ('if', 'ifdef', 'ifndef'):
                cond = re.sub(r'\s|//.*|/\*.*', '', rest)
                if d == 'if' and cond.startswith('WWE2K14_STREAM()'):
                    stack.append([True, True])
                elif d == 'if' and cond.startswith('!WWE2K14_STREAM()'):
                    stack.append([True, False])
                else:
                    stack.append([False, False])
            elif d in ('else', 'elif') and stack and stack[-1][0]:
                stack[-1][1] = not stack[-1][1]
            elif d == 'endif' and stack:
                stack.pop()
        if any(e[1] for e in stack):
            skip.add(i)
    return skip


def run_deps(root, out, extra):
    deps_file = os.path.join(out, 'deps.json')
    if os.path.exists(deps_file):
        os.remove(deps_file)   # a stale file from an earlier pass must never be read as this pass's result
    os.makedirs(out, exist_ok=True)
    cmd = [sys.executable, '-I', os.path.join(root, 'tools', 'scene', 'keep_stream_off_link.py'), '--root', root, '--keep', '0', '--deps', '--out', out]
    if extra:
        cmd += ['--extra', ','.join(extra), '--only', 'src/main.cpp']
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        return None, 'the include-closure pass failed (exit %d)\n%s' % (r.returncode, (r.stdout + r.stderr)[-3000:])
    if not os.path.isfile(deps_file):
        return None, 'the include-closure pass wrote no deps.json'
    return json.load(open(deps_file)), ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--plant', default='')
    ap.add_argument('--from-deps', default='', help='use an existing deps.json (tests)')
    a = ap.parse_args()
    root = os.path.abspath(a.root).replace('\\', '/')
    forbidden = lines(os.path.join(root, 'tools', 'scene', 'keep_stream_headers.txt'))
    for h in forbidden:
        if not os.path.isfile(os.path.join(root, 'src', h)):
            print('FAIL: keep_stream_headers.txt names %s which does not exist' % h)
            return 1
    fnorm = {norm('/src/' + h): h for h in forbidden}
    for lst in ('keep_stream_sources.txt', 'keep_stream_off_sources.txt'):
        for s in lines(os.path.join(root, 'tools', 'scene', lst)):
            if not os.path.isfile(os.path.join(root, s)):
                print('FAIL: %s names %s which does not exist' % (lst, s))
                return 1
    known = {}
    kp = os.path.join(root, 'tools', 'scene', 'keep_stream_known_open.txt')
    if os.path.isfile(kp):
        for ln in lines(kp):
            unit, _, why = ln.partition('|')
            known[norm(unit.strip())] = why.strip()
    out = os.path.join(root, 'out', 'p9_lint' + ('_plant' if a.plant else ''), 'deps')

    if a.plant:
        pdir = os.path.join(root, 'src', 'renderer_scene')
        twin_a = os.path.join(pdir, 'zz_plant_keep_stream_a.cpp')
        hdr_b = os.path.join(pdir, 'zz_plant_keep_stream_b.h')
        twin_b = os.path.join(pdir, 'zz_plant_keep_stream_b.cpp')
        try:
            open(twin_a, 'w').write('// planted: a unit of the OFF build that includes the stream reader\n#include "../renderer_adapter/reader.h"\n')
            open(hdr_b, 'w').write('// planted: a kept header that reaches the stream\n#pragma once\n#include "../renderer_adapter/d3_arena.h"\n')
            open(twin_b, 'w').write('// planted: a unit that reaches the stream only through a kept header\n#include "zz_plant_keep_stream_b.h"\n')
            deps, err = run_deps(root, out, [twin_a, twin_b])
        finally:
            for f in (twin_a, hdr_b, twin_b):
                if os.path.exists(f):
                    os.remove(f)
        if deps is None:
            print('FAIL: ' + err)
            return 1
        got = {}
        for unit, dl in deps.items():
            h = hit_headers(dl, fnorm)
            if h:
                got[norm(unit)] = h
        ok = True
        for twin, need_h in ((twin_a, 'renderer_adapter/reader.h'), (twin_b, 'renderer_adapter/d3_arena.h')):
            hits = [v for k, v in got.items() if k.endswith(norm('/' + os.path.basename(twin)))]
            if hits and need_h in hits[0]:
                print('ok   planted twin %s: reported (%s)' % (os.path.basename(twin), ', '.join(hits[0])))
            else:
                print('FAIL: the planted twin %s (reaches %s) was NOT reported: the lint is blind (%d units scanned)' % (os.path.basename(twin), need_h, len(deps)))
                ok = False
        return 0 if ok else 1

    if a.from_deps:
        deps = json.load(open(a.from_deps))
    else:
        deps, err = run_deps(root, out, [])
        if deps is None:
            print('FAIL: ' + err)
            return 1
    bad, still_known = [], set()
    closure = set()
    for unit, dl in sorted(deps.items()):
        for d in dl:
            closure.add(norm(d))
        hit = hit_headers(dl, fnorm)
        if hit:
            u = norm(unit)
            if any(u.endswith(k) for k in known):
                still_known.add(next(k for k in known if u.endswith(k)))
                print('KNOWN OPEN: %s reaches the stream (%s): %s' % (unit, ', '.join(hit), known[next(k for k in known if u.endswith(k))]))
            else:
                bad.append((unit, hit))
    probs = []
    for k in sorted(set(known) - still_known):
        probs.append('keep_stream_known_open.txt lists %s but it no longer reaches the stream: remove it from the list' % k)
    # 2. raw macro in conditionals
    srcdir = os.path.join(root, 'src')
    raw = re.compile(r'#\s*(?:if|elif)\s+.*\bWWE2K14_KEEP_STREAM\b|#\s*ifn?def\s+WWE2K14_KEEP_STREAM\b')
    for dp, _dn, fn in os.walk(srcdir):
        for f in fn:
            if not f.endswith(('.h', '.cpp', '.inc')) or f == 'keep_stream.h':
                continue
            p = os.path.join(dp, f)
            for i, ln in enumerate(open(p, encoding='utf-8', errors='replace').read().split('\n'), 1):
                if raw.search(ln):
                    probs.append('%s:%d uses the raw WWE2K14_KEEP_STREAM in a conditional (use `#if WWE2K14_STREAM()` from keep_stream.h)' % (os.path.relpath(p, root).replace('\\', '/'), i))
    # 3. removed switches read in the OFF closure
    rs = open(os.path.join(srcdir, 'renderer_game', 'removed_switches.h'), encoding='utf-8').read()
    names = re.findall(r'WWE2K14_REMOVED_SWITCH_FALSE\((\w+)\)', rs) + re.findall(r'FLAGS_(\w+)_storage_\(\)', rs)
    names = sorted({n for n in names if n != '##name##'} - {'name'})
    use = re.compile(r'REXCVAR_(?:GET|SET)\(\s*(%s)\b' % '|'.join(map(re.escape, names)))
    n_checked = 0
    for c in sorted(closure):
        if not c.startswith(norm(srcdir) + '/') or not os.path.isfile(c):
            continue
        if c.endswith('/renderer_game/removed_switches.h'):
            continue
        ls = open(c, encoding='utf-8', errors='replace').read().split('\n')
        on_only = on_only_lines(ls)
        for i, ln in enumerate(ls):
            if use.search(ln) and i not in on_only:
                n_checked += 1
                if not any('OFF:' in x for x in ls[max(0, i - 3):i + 1]):
                    probs.append('%s:%d reads a removed switch (%s) without an `OFF:` comment (same line or the 3 above) saying what it means when the stream is left out' % (os.path.relpath(c, root).replace('\\', '/'), i + 1, use.search(ln).group(1)))
    for p in probs:
        print('FAIL: ' + p)
    for unit, hit in bad:
        print('FAIL: %s reaches the stream with WWE2K14_KEEP_STREAM=0: %s' % (unit, ', '.join(hit)))
    if bad or probs:
        return 1
    print('PASS: %d units of the OFF build; none outside the %d listed known-open units reaches any of the %d stream headers; no raw macro tests; %d removed-switch reads all annotated OFF:' % (len(deps), len(still_known), len(forbidden), n_checked))
    return 0


if __name__ == '__main__':
    sys.exit(main())
