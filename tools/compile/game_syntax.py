#!/usr/bin/env python3
"""Compile check: the game exe's translation units still compile (S102 T11 follow-up; G12 named a struct field `small`, a macro of the Windows RPC headers the GAME build
includes: fview and rplay compiled, the game exe did not, and nobody saw it until the build).

  py -I tools\\compile\\game_syntax.py [--root DIR] [--files src/a.cpp,src/b.cpp] [--no-twin]
  py -I tools\\compile\\game_syntax.py --changed [BASE] [--build off|on|both] [--root DIR]     (S103 GSB: the main session runs it after every merge)

S111 COMPILEOFF: the default build is `off`, the stream-free build the players run (WWE2K14_KEEP_STREAM=OFF + KEEP_STREAM_RUN=ON, preset win-amd64-release-off). The stream build (`on`) is only
compiled when asked for by name (`--build on` / `--build both`): the code stays for history, nobody compiles it by default (Jarrod, 2026-11-27: "we should not be compiling into it").
--changed: every src\\*.cpp unit the change touched (the file itself, or a unit that includes a changed header, found by a quoted-#include scan of THIS tree's src) is syntax-checked in the requested
build (default off), with that build's own flags (read from the tree's own configure; the fallback reads main's out\\build\\win-amd64-release-off for off); a unit a build
does not compile is skipped for that build only. BASE default: HEAD^1 when HEAD is a merge commit, else the merge-base with the main checkout's current branch (the rule of only_base below); the changed set also holds uncommitted edits. Without --changed the old fixed list runs (the off build since S111 COMPILEOFF; it was the stream build).
S105 PCH: --changed parses every plain game unit with a precompiled header (src\\pch_units.h: the STL headers the real build force-includes into every plain unit (S106 G4-4), no Windows headers; tools\\compile\\syntax_pch.py has the rules) built once per build + tree root into
<store>\\pch; units with other flags, main.cpp and units that #define before their first #include skip it (printed). --no-cache / GAME_SYNTAX_NO_CACHE=1 = no store and no PCH.
Not covered: Android guards, the AVX2 (off-v3) build, tool builds (a note only); --files / --no-twin are ignored with --changed. On the main checkout the default base is the last sha this tool PASSED
on the branch (out\\game_syntax\\last_pass_<branch>.txt) when an ancestor of HEAD, else HEAD^1; in any other tree (a builder worktree) it is the merge-base with the main checkout's branch. 0 units = `NOTHING CHECKED`
(exit 0, not a PASS); a failed git call = FAIL.
S106 GSMAIN: the flags come from the CHECKED TREE'S OWN configure-only CMake run (tools\\compile\\syntax_cfg.py: a placeholder source folder, main's cache entries, `ninja -t commands`; ~5 s the first time per
tree-config, a hit afterwards), NOT from main's out\\build, so a tree whose CMakeLists.txt differs from main's is checked with ITS flags; a tree that does not configure = FAIL (cmake's last lines). Exit 4
(NEEDS-MAIN: the units compile but main's configured build does not describe the tree) happens only on the FALLBACK, when cmake / ninja / main's cache are missing, and the output says so.
Not seen: tool / viewer builds (they list their own sources in tools\\**\\build*.ps1): a changed file one of those names prints a note to build that tool.

clang -fsyntax-only of src\\main.cpp and src\\renderer_adapter\\live.cpp (and any unit --files names; each must be one the main build compiles), with the flags the MAIN
checkout's own build uses (`ninja -t commands` in out\\build\\win-amd64-release: defines, standard, system includes), the tree's src (--root) put first so a worktree's
edits are what gets compiled; includes the main checkout still owns (generated\\default, thirdparty) come after.
The planted twin (unless --no-twin): src\\main.cpp followed by `struct PlantSmall { int small; };` in a scratch unit under out\\game_syntax must FAIL to compile (the macro
is live in that unit); if it compiles, the check itself is blind and FAILs. A missing ninja / build dir / flags line fails loudly (it never falls back to guessed flags)."""
import argparse
import ctypes
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import syntax_store as _store   # S105 CC: per-key result files shared by every worktree
import syntax_cfg as _cfg   # S106 GSMAIN: the flags come from the checked tree's own configure-only CMake run
import syntax_pch as _pch   # S105 PCH: precompiled STL + fx_registry.h for the per-unit check (key text = the parse's text, see syntax_pch.py)
import proc as _proc   # QTF: every child runs under a Windows Job Object + hard timeout (a 10-hour hang happened with none)
sys.path.append(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'admit'))
import admit_py as _admit   # CQW: the machine-wide compile queue (nothing runs at import)

T_GIT, T_NINJA = 60, 300
T_PP = int(os.environ.get('GAME_SYNTAX_PP_TIMEOUT', '300'))
T_UNIT = int(os.environ.get('GAME_SYNTAX_UNIT_TIMEOUT', '900'))

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
DEFAULT_FILES = ['src/main.cpp', 'src/renderer_adapter/live.cpp']
def _free_ram_gb():
    try:
        class MS(ctypes.Structure):
            _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong), ('ullTotalPhys', ctypes.c_ulonglong),
                        ('ullAvailPhys', ctypes.c_ulonglong), ('ullTotalPageFile', ctypes.c_ulonglong), ('ullAvailPageFile', ctypes.c_ulonglong),
                        ('ullTotalVirtual', ctypes.c_ulonglong), ('ullAvailVirtual', ctypes.c_ulonglong), ('ullAvailExtendedVirtual', ctypes.c_ulonglong)]
        m = MS()
        m.dwLength = ctypes.sizeof(MS)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
        return m.ullAvailPhys / 2**30
    except Exception:
        return None


def _syntax_workers():
    """Parallel clang parses: at most 8, one per 2.5 GB of FREE RAM above a 6 GB reserve, at least 1 (S103 11:38 RAM 0.0 GB)."""
    free = _free_ram_gb()
    by_ram = 8 if free is None else int((free - 6) // 2.5)
    return max(1, min(8, by_ram))


BUILD_DIR = os.path.join('out', 'build', 'win-amd64-release')
BUILD_DIRS = {'on': BUILD_DIR, 'off': os.path.join('out', 'build', 'win-amd64-release-off')}


def main_checkout(root):
    out = _proc.run(['git', '-C', root, 'rev-parse', '--git-common-dir'], timeout=T_GIT)
    if out.returncode:
        return None
    return os.path.dirname(os.path.abspath(os.path.join(root, out.stdout.strip())))


def find_ninja():
    n = shutil.which('ninja')
    if n:
        return n
    for pat in (r'C:\Program Files\Microsoft Visual Studio\2022\*\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe',
                r'C:\Program Files (x86)\Microsoft Visual Studio\2022\*\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'):
        hits = glob.glob(pat)
        if hits:
            return hits[0]
    return None


def build_commands(main, build_rel=BUILD_DIR):
    ninja = find_ninja()
    if not ninja:
        raise RuntimeError('ninja not found (PATH or the Visual Studio copy)')
    bd = os.path.join(main, build_rel)
    if not os.path.isfile(os.path.join(bd, 'build.ninja')):
        raise RuntimeError('no build.ninja in ' + bd + ' (the main checkout has never configured this build)')
    r = _proc.run([ninja, '-C', bd, '-t', 'commands'], timeout=T_NINJA)
    if r.returncode:
        raise RuntimeError('ninja -t commands failed: ' + r.stderr[:500])
    return [ln for ln in r.stdout.splitlines() if ' -c ' in ln]


def build_lines_for(root, main_dir, b, why=None):
    """S106 GSMAIN: the compile command lines of build `b` ('on' | 'off') for the tree `root`: its own configure (syntax_cfg), the root rewritten to main's. ConfigureFailed (the tree does not
    configure) propagates: a real FAIL, never stale flags. Only when cmake / ninja / main's cache are missing does it fall back to main's out\\build (loudly) and append cmake_state's reasons to `why`."""
    try:
        return _cfg.commands(root, main_dir, b)
    except _cfg.Unavailable as e:
        print('note: syntax_cfg unavailable (%s): FALLING BACK to the main checkout\'s out\\build flags for the %s build (they may not describe this tree: exit 4 NEEDS-MAIN when they differ)' % (e, b))
        if why is not None:
            why.extend(w for w in cmake_state(root, main_dir, [b]) if w not in why)
        _check_folder_variant(main_dir, b)
        return build_commands(main_dir, BUILD_DIRS[b])


def _check_folder_variant(main_dir, b):
    """S111: the fallback reads main's folder as build `b`; refuse when that folder's own cache says it is the other variant (out\\build\\win-amd64-release is the stream-FREE player build since 2026-11-26,
    so it is no 'on' reference). RuntimeError = the same channel as a missing build.ninja."""
    cache = _cfg.parse_cache(os.path.join(main_dir, BUILD_DIRS[b], 'CMakeCache.txt'))
    got = (cache or {}).get('WWE2K14_KEEP_STREAM', (None, None))[1]
    want = _cfg.VARIANT[b]['WWE2K14_KEEP_STREAM']
    if got is not None and got != want:
        raise RuntimeError('main\'s %s is configured WWE2K14_KEEP_STREAM=%s, not the %s build\'s %s: it is no reference for the %s build' % (BUILD_DIRS[b], got, b, want, b))


def flags_for(lines, main, tu_rel):
    """The main build's flags for one unit, without the object / dependency / pch parts."""
    want = (main + '/' + tu_rel).replace('\\', '/').lower()
    for ln in lines:
        m = re.search(r' -c (\S+)\s*$', ln)
        if not m or m.group(1).replace('\\', '/').lower() != want:
            continue
        toks = ln.replace('\\"', '"').split()[1:]
        out, i = [], 0
        while i < len(toks):
            t = toks[i]
            if t in ('-MT', '-MF', '-o', '-c', '-include', '-Xclang'):
                i += 2 if t != '-c' else 2
                continue
            if t in ('-MD', '-MMD', '-Winvalid-pch') or t == m.group(1):
                i += 1
                continue
            out.append(t)
            i += 1
        return out
    raise RuntimeError('the main build has no compile command for %s: only units it compiles can be checked' % tu_rel)


def tree_args(root, main, flags):
    """The main build's flags for one unit, pointed at the tree `root`: its own roots first, the main checkout's after (generated code, thirdparty, ../thirdparty relative includes).
    Only the roots the unit's own flags name: the launcher target has no -I<repo root>, and adding one made <version> (via nlohmann/json.hpp) resolve to a stray repo-root file
    "version" (S103 23:4x). Shared with tools/handback/cvar_pp.py (LNK: the cvar judge preprocesses with exactly these command lines)."""
    wt = os.path.abspath(root).replace('\\', '/')
    m = main.replace('\\', '/')
    had = set()
    kept = []
    for f in flags:
        p = f[2:].replace('\\', '/').rstrip('/') if f.startswith('-I') else None
        if p in (m, m + '/src'):
            had.add(p[len(m):])   # '' or '/src': replaced by the tree's own root below
            continue
        kept.append(f)
    order = [s for s in ('', '/src') if s in had]
    return kept + ['-I' + wt + s for s in order] + ['-I' + m + s for s in order]


def with_pch(root, unit, flags):
    """main.cpp gets the tree's own pch_main.h first (the build's PCH pair is dropped from the flags by flags_for; the G12 macro-order class: the PCH's headers come before main.cpp's
    own). Shared with tools/handback/cvar_pp.py."""
    if unit == 'src/main.cpp':
        return flags + ['-include', os.path.abspath(os.path.join(root, 'src', 'pch_main.h')).replace('\\', '/')]
    return flags


def _roots(root, main):
    return ((os.path.abspath(root).replace('\\', '/'), '<ROOT>'), (main.replace('\\', '/'), '<MAIN>'))


def compile_unit(root, main, flags, unit_path, label, cache_fail=False):
    rc, text, key, hit = syntax_check(tree_args(root, main, flags), unit_path, roots=_roots(root, main), cache_fail=cache_fail)
    return label, rc, text, key, hit


def run_jobs(jobs, root, main, workers=None, pch_refs=None):
    """[(label, flags, unit_path)] -> [(label, rc, text, key, hit)] in order. S105 CC: phase 1 looks every unit up in the store (a preprocess each, no compile-queue admission, at most 4 at once);
    only the misses go through the admitted parse pool, held only when there is one: a pool of all hits never waits for the queue."""
    res = [None] * len(jobs)
    args = [_clean(tree_args(root, main, j[1])) for j in jobs]
    roots = _roots(root, main)
    pch = assign_pch(jobs, args, pch_refs)
    if jobs:
        with ThreadPoolExecutor(max_workers=min(4, len(jobs))) as ex:
            looked = list(ex.map(lambda i: syntax_lookup(args[i], jobs[i][2], roots, pch[i]), range(len(jobs))))
    else:
        looked = []
    todo = []
    for i, (key, hit) in enumerate(looked):
        if hit is not None:
            res[i] = (jobs[i][0],) + hit
        else:
            todo.append(i)
    if todo:
        with _admit.hold('syntax.worker', workers or _syntax_workers()) as nw, ThreadPoolExecutor(max_workers=nw) as ex:
            for i, r in zip(todo, ex.map(lambda i: syntax_run(args[i], jobs[i][2], roots, looked[i][0], pch[i]), todo)):
                res[i] = (jobs[i][0],) + r
    return res


def norm_args(args, roots):
    """The flag list with every tree path replaced by a placeholder (longest path first: a worktree lives inside the main checkout), so two worktrees with the same text share cache keys: the -E text
    already records how every include resolved, the paths in the flags only say where this tree is."""
    out = []
    for arg in args:
        for path, ph in sorted(roots, key=lambda r: -len(r[0])):
            for v in {path, path.replace('/', '\\'), path.replace('\\', '/')}:
                arg = arg.replace(v, ph)
        out.append(arg)
    return out


NO_MAIN_PCH = False   # S110 SYNTAXPCH: --no-pch
KEY_SALT = 'syntax-v2:-w'   # S105 CC: the parse now runs with -w (not -Wno-everything, which also hid clang's default-error diagnostics such as -Wc++11-narrowing): old stored passes must not be served
FAIL_RX = re.compile(r':\d+:\d+: (?:fatal )?error:')
CRASH_RX = re.compile(r'PLEASE submit a bug report|Stack dump|LLVM ERROR|out of memory|bad_alloc|Assertion|Segmentation|frontend command failed|error in backend|timed out')


def storable(rc, text):
    """A pass, or a failure that is a real source diagnostic: exit 1 with a `file:L:C: error:` line and no crash / OOM / timeout marker (a killed or crashed clang says nothing about the source)."""
    if rc == 0:
        return True
    return rc == 1 and bool(FAIL_RX.search(text or '')) and not CRASH_RX.search(text or '')


def pp_key(args, unit_path, roots):
    """Key of one unit: hash of `clang -E -P` of it with the full flag list + the placeholder-normalised flags + the clang + KEY_SALT. None when the preprocess fails or the cache is off.
    S105 CC measured one unit (src/start_settings.cpp, stream build): preprocess 0.35 s (-fdirectives-only 0.26 s, -M deps 0.50 s) vs the parse 3.2 s, so keying on the preprocessed text
    is the cheaper safe key (a -MD header-hash scheme costs the same preprocess plus re-hashing the headers); the preprocess is the only unconditional cost of a hit."""
    if os.environ.get('GAME_SYNTAX_NO_CACHE'):
        return None
    pp = _proc.run([CLANG] + args + ['-E', '-P', '-w', unit_path], timeout=T_PP, text=False)
    if pp.returncode != 0:
        return None
    d = hashlib.sha256(pp.stdout)
    d.update(('\0'.join(norm_args(args, roots)) + '\0' + CLANG_ID + '\0' + KEY_SALT).encode())
    return d.hexdigest()


def _clean(args):
    """-Wno-everything in a caller's flags (keep_stream_off_link --syntax) would hide the default errors again; the parse runs with -w instead."""
    out, i = [], 0
    while i < len(args):
        a = args[i]
        if a == '-include' and i + 1 < len(args) and args[i + 1].replace('\\', '/').endswith('src/pch_units.h'):
            i += 2   # S106 G4-4: the build's force-include (src/pch_units.h) is re-added from THIS tree by _kargs (the PCH, or the plain text), so the same parse results whatever build.ninja holds
            continue
        if a != '-Wno-everything':
            out.append(a)
        i += 1
    return out


FORCE_INC = None   # S106 G4-4: <tree>/src/pch_units.h while --changed runs on a tree that has it (the real build force-includes it into every plain game unit); None = old behaviour
FORCE_UNITS = set()   # S106 G4-4F (review B1): the units that force-include it = the game target's units (_norm(abs path)); the launcher target (LAUNCHER_PREFIXES) and main.cpp (pch_main.h) have none in the real build
LAUNCHER_PREFIXES = ('src/launcher/', 'src/importer/', 'src/audio_import/audio_import.cpp')


def force_units(root, rels):
    """The set of _norm(abs path) of the game-target units among `rels` (repo-relative): every unit but src/main.cpp and the launcher target's (LAUNCHER_PREFIXES). Shared with keep_stream_off_link --syntax."""
    return set(_norm(os.path.join(root, u)) for u in rels if u != 'src/main.cpp' and not u.replace(chr(92), '/').startswith(LAUNCHER_PREFIXES))


def set_force(root, rels):
    """Parse the game units in `rels` with this tree's src/pch_units.h force-included, as the build does (a tree without the file: unchanged behaviour)."""
    global FORCE_INC, FORCE_UNITS
    h = _pch.header_path(root)
    FORCE_INC = h if os.path.isfile(h) else None
    FORCE_UNITS = force_units(root, rels) if FORCE_INC else set()


def _forced(unit_path):
    return bool(FORCE_INC) and _norm(unit_path) in FORCE_UNITS


def _kargs(args, pch, unit_path=None):
    """The flag list a unit is keyed and parsed with: the PCH's own -include (the text the PCH holds), else the build's force-include of src/pch_units.h as plain text (a unit that skips the PCH
    still parses what the build parses), else the flags as they are (main.cpp has pch_main.h; tool callers have no force-include)."""
    if pch:
        return args + ['-include', pch['header']]
    if unit_path is not None and _forced(unit_path) and '-include' not in args and '-include-pch' not in args:
        return args + ['-include', FORCE_INC]
    return args


PCH_SKIPS = []   # (unit, reason) of units that were PCH candidates (a reference exists for the run) but skipped it
PCH_STATS = {'used': 0, 'fallback': 0, 'rebuilt': 0}


def _strip_main_inc(args, header):
    """`args` without the `-include <header>` pair with_pch added (main.cpp's plain-text pch_main.h), or None when it holds none."""
    h = header.replace('\\', '/').lower()
    for i in range(len(args) - 1):
        if args[i] == '-include' and args[i + 1].replace('\\', '/').lower() == h:
            return args[:i] + args[i + 2:]
    return None


def main_pch_ref(root, main, flags):
    """S110 SYNTAXPCH: main.cpp's PCH reference for one build = src/pch_main.h (the file the real build precompiles) built with main.cpp's own flags + -fpch-instantiate-templates (as CMake's
    PCH_INSTANTIATE_TEMPLATES does). One per build: clang refuses to share one across WWE2K14_KEEP_STREAM values. Same key rule as every PCH (syntax_pch._key): the expanded closure of the header
    (-E -P -dD) + the flags + the build-only extra flag + clang + tree root; clang's own input check (mtime / size) rejects a touched one and the existing rebuild-once / loud-fallback rules apply."""
    ref = _pch.make_ref(root, _clean(tree_args(root, main, flags)), header=os.path.abspath(os.path.join(root, 'src', 'pch_main.h')).replace('\\', '/'))
    ref['extra'] = ['-fpch-instantiate-templates']
    ref['main'] = True
    return ref


def assign_pch(jobs, args, pch_refs):
    """[ref or None] per job: the PCH reference whose flag list the unit's flags equal and whose unit text allows it. A unit that cannot use one is recorded in PCH_SKIPS (quiet reasons are not).
    A main.cpp reference matches a job whose flags carry the plain `-include pch_main.h` (with_pch) and equal the reference's without it; that pair is then removed from args[i] (the PCH supplies it)."""
    out = [None] * len(jobs)
    if not pch_refs or os.environ.get('GAME_SYNTAX_NO_CACHE'):
        return out
    for i, j in enumerate(jobs):
        reasons = []
        for ref in pch_refs:
            a = args[i]
            if ref.get('main'):
                a = _strip_main_inc(a, ref['header'])
                if a is None:
                    continue
            good, why = _pch.unit_ok(a, ref['args'], j[2], force=_forced(j[2]))
            if good:
                out[i] = ref
                args[i] = a
                break
            reasons.append(why)
        loud = [r for r in reasons if not r.startswith('-')]
        if out[i] is None and loud:
            PCH_SKIPS.append((j[0], loud[-1]))
    return out


def syntax_lookup(args, unit_path, roots=(), pch=None):
    """(key, result): result = (rc, text, key, True) when the store holds the unit's verdict, else None. A cached failure's text names the bypass."""
    args = _clean(args)
    key = pp_key(_kargs(args, pch, unit_path), unit_path, roots)
    if key is not None:
        c = _store.get(key)
        if c is not None:
            STATS['hit'] += 1
            rc, text = c
            return key, (rc, (('pp-cache: cached failure replayed (same preprocessed text and flags as an earlier run; bypass: --no-cache or GAME_SYNTAX_NO_CACHE=1)\n' + text) if rc else 'pp-cache hit (cached pass)'), key, True)
    return key, None


def _pch_path(pch, roots, force=False, seen_gen=None):
    return _pch.ensure(pch, _store.store_dir(), _proc.run, CLANG, CLANG_ID, norm_args, roots, T_UNIT, force=force, seen_gen=seen_gen)


def _pch_parse(args, unit_path, path):
    return _proc.run([CLANG] + args + ['-Xclang', '-include-pch', '-Xclang', path, '-fsyntax-only', '-w', unit_path], timeout=T_UNIT)


def _pch_bad(r):
    return bool(r.returncode) and bool(_pch.BAD_RX.search((r.stdout or '') + (r.stderr or '')))


def syntax_run(args, unit_path, roots, key, pch=None):
    """The parse of a unit the store did not hold: (rc, text, key, False). Stores the verdict (storable() only, and only when the unit preprocesses to the same key again).
    pch = a syntax_pch reference: the parse uses the .pch (built here the first time, under the pool's admission), the key is of the text with the PCH's -include. A PCH that cannot be built,
    or that clang rejects after one rebuild, makes this unit fall back to a plain parse under its plain key (a loud PCH FALLBACK line); a failure is never a pass."""
    args = _clean(args)
    if pch is not None:
        path, gen = _pch_path(pch, roots)   # S106 G4-4F (m1): gen = which build of the .pch this parse used, read under the ref's lock with the path; a rejection rebuilds only if nobody rebuilt it since
        r = _pch_parse(args, unit_path, path) if path else None
        if r is not None and _pch_bad(r):   # a touched header: clang rejects the .pch -> rebuild once
            PCH_STATS['rebuilt'] += 1
            path, _g = _pch_path(pch, roots, force=True, seen_gen=gen)
            r = _pch_parse(args, unit_path, path) if path else None
            if r is not None and _pch_bad(r):
                pch['dead'] = True   # a rebuilt PCH is rejected too: later units parse plainly without rebuilding again
                pch['note'] = 'clang rejects the rebuilt PCH: ' + ((r.stdout or '') + (r.stderr or ''))[:300].replace('\n', ' | ')
                r = None
        if r is None:
            PCH_STATS['fallback'] += 1
            print('PCH FALLBACK: %s parsed without the PCH (%s)' % (unit_path, pch['note'] or 'the PCH was evicted by another run or lost a build race'), flush=True)
            if pch.get('main'):
                args = args + ['-include', pch['header']]   # the plain parse of today: pch_main.h as text (a main.cpp PCH never drops the header from the parse)
            k2, hit = syntax_lookup(args, unit_path, roots)
            if hit is not None:
                return hit
            return syntax_run(args, unit_path, roots, k2)
        PCH_STATS['used'] += 1
        STATS['miss'] += 1
        return _finish(args, unit_path, roots, key, r, pch)
    STATS['miss'] += 1
    r = _proc.run([CLANG] + _kargs(args, None, unit_path) + ['-fsyntax-only', '-w', unit_path], timeout=T_UNIT)   # S106 G4-4: a plain parse carries the build's force-include too
    return _finish(args, unit_path, roots, key, r, None)


def _finish(args, unit_path, roots, key, r, pch):
    if r.timed_out:
        r.stderr = (r.stderr or '') + '\nerror: game_syntax: clang timed out after %d s on %s (process tree killed)' % (T_UNIT, unit_path)
    text = r.stdout + r.stderr
    if key is not None and not r.timed_out and storable(r.returncode, text):
        if pp_key(_kargs(args, pch, unit_path), unit_path, roots) == key:
            _store.put(key, r.returncode, text)
        else:
            NOT_STORED.append(unit_path)
    return r.returncode, text, key, False


def syntax_check(args, unit_path, roots=(), cache_fail=False, pch=None):
    """clang -fsyntax-only of one unit with the full flag list `args`, served by the preprocessed-text store (tools\\compile\\syntax_store.py). Returns (rc, text, key, hit).
    A pass AND a source failure are stored (S105 CC; `cache_fail` is kept for old callers and ignored), each the moment its unit ends; a hit replays the verdict and the diagnostics
    (a cached failure is a failure with its text, never a pass). A crash, an OOM or a timeout is never stored. Shared with tools/scene/keep_stream_off_link.py --syntax.
    S103 H3: the key is the hash of `clang -E -P` of the same unit with the same flags + the clang: identical preprocessed text + identical flags + the same clang = the same verdict.
    The preprocess always runs, so an edited header can never hit. Only an exit-0 preprocess is keyed.
    GSP2 review bug 6: the preprocess and the parse are two reads of the tree; after the parse the unit is preprocessed AGAIN and the verdict is stored only when that key equals the
    first (a changed key = not stored, counted in NOT_STORED). The flags' tree paths are placeholders in the key (roots), so worktrees share hits.
    Not in the key: the contents of a response file named in the flags (`@$DYNDEP_MODULE_MAP_FILE`; theoretical, noted)."""
    if pch is not None and (os.environ.get('GAME_SYNTAX_NO_CACHE') or not _pch.unit_ok(_clean(args), pch['args'], unit_path, force=_forced(unit_path))[0]):
        pch = None   # S105 PCH: a unit whose flags differ / that defines before its includes skips it; --no-cache means no PCH either
    key, hit = syntax_lookup(args, unit_path, roots, pch)
    if hit is not None:
        return hit
    return syntax_run(args, unit_path, roots, key, pch)


def load_cache(main_dir):
    """Kept for callers: the store is per-key files under %USERPROFILE%\\.cache\\wwe2k14-syntax now (not main\\out); returns the store folder."""
    return _store.store_dir()


def save_cache(cache_file):
    """Kept for callers: entries are written as each unit ends; this only trims the store to its size cap."""
    _store.prune()


def _clang_id():
    try:
        st = os.stat(CLANG)
        return '%d:%d' % (st.st_size, st.st_mtime_ns)
    except OSError:
        return '?'


CLANG_ID = _clang_id()
NOT_STORED = []   # units whose verdict was not stored because the tree changed between the first preprocess and the parse (bug 6)
STATS = {'hit': 0, 'miss': 0}   # this process's store hits / parses (S105 CC)


def git_lines(root, *args):
    """stdout lines of one git command, or None when it failed / timed out (a failure is never "no output")."""
    r = _proc.run(['git', '-C', root] + list(args), timeout=T_GIT)
    if r.returncode or getattr(r, 'timed_out', False):
        return None
    return r.stdout.strip().splitlines()


def _same_dir(a, b):
    try:
        return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))
    except OSError:
        return False


def _branch_key(root):
    br = git_lines(root, 'rev-parse', '--abbrev-ref', 'HEAD')
    return re.sub(r'[^A-Za-z0-9._-]', '_', br[0] if br else 'detached')


def last_pass_file(main_dir, root):
    return os.path.join(main_dir, 'out', 'game_syntax', 'last_pass_%s.txt' % _branch_key(root))


def default_base(root, main_dir):
    """The commit --changed diffs against. The one definition of this rule:
    - a tree that is NOT the main checkout (a builder worktree, a separate checkout) -> merge-base(HEAD, the main checkout's branch): HEAD^1 of a merge there is only what the session brought in
      (`git merge <session branch>`), never the builder's own edits;
    - the main checkout itself -> the last sha this tool PASSED on this branch (out\\game_syntax\\last_pass_<branch>.txt) when it is an ancestor of HEAD, else HEAD^1 (a merge's first parent;
      a non-merge HEAD, e.g. the STATUS commit after a merge, a hand fix or a fast-forward, would otherwise check nothing).
    Returns None when git cannot say."""
    br = git_lines(main_dir, 'rev-parse', '--abbrev-ref', 'HEAD')
    if not _same_dir(root, main_dir):
        mb = git_lines(root, 'merge-base', 'HEAD', br[0] if br else 'main')
        return mb[0] if mb else None
    try:
        sha = open(last_pass_file(main_dir, root)).read().strip()
    except OSError:
        sha = ''
    if sha and git_lines(root, 'cat-file', '-e', sha + '^{commit}') is not None and \
            _proc.run(['git', '-C', root, 'merge-base', '--is-ancestor', sha, 'HEAD'], timeout=T_GIT).returncode == 0:
        return sha
    return 'HEAD^1' if git_lines(root, 'rev-parse', '--verify', '-q', 'HEAD^1') else 'HEAD'


def changed_files(root, base):
    """Repo-relative changed paths (base -> working tree, renames split into a delete + an add, plus untracked), or None when a git call failed."""
    d = git_lines(root, 'diff', '--name-only', '--no-renames', base)
    u = git_lines(root, 'ls-files', '--others', '--exclude-standard')
    if d is None or u is None:
        return None
    return sorted(set(c.replace('\\', '/') for c in d + u))


INC_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)


def _norm(p):
    return os.path.normpath(p).replace('\\', '/').lower()


def units_for_changed(root, changed):
    """src/**/*.cpp that is a changed file or includes one (transitively; quoted includes resolved against the includer's dir, src, then the root). An include whose target no longer exists
    (a deleted / renamed header) still maps its includer-relative and src-relative candidates, so the old path reaches its includers. src/pch_main.h stands for src/main.cpp (the PCH)."""
    src = os.path.join(root, 'src')
    rev, files = {}, []
    for dp, _dn, fn in os.walk(src):
        for f in fn:
            if f.endswith(('.cpp', '.h', '.hpp', '.inc', '.inl')):
                files.append(os.path.join(dp, f))
    for p in files:
        try:
            txt = open(p, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        for inc in INC_RE.findall(txt):
            cands = [os.path.join(b, inc) for b in (os.path.dirname(p), src, root)]
            hit = [q for q in cands if os.path.isfile(q)]
            for q in (hit[:1] if hit else cands[:2]):
                rev.setdefault(_norm(q), []).append(p)
    main_cpp = os.path.join(src, 'main.cpp')
    rev.setdefault(_norm(os.path.join(src, 'pch_main.h')), []).append(main_cpp)
    rev.setdefault(_norm(os.path.join(src, 'pch_units.h')), []).extend(p for p in files if p.endswith('.cpp') and _norm(p) != _norm(main_cpp))   # S106 G4-4: the build force-includes it into every plain unit
    seen = set(_norm(os.path.join(root, c)) for c in changed)
    todo = list(seen)
    while todo:
        k = todo.pop()
        for p in rev.get(k, []):
            if _norm(p) not in seen:
                seen.add(_norm(p))
                todo.append(_norm(p))
    return sorted(os.path.relpath(p, root).replace('\\', '/') for p in files if p.endswith('.cpp') and _norm(p) in seen)


UNIT_RE = re.compile(r' -c (\S+)\s*$')
CMAKE_UNITS_CAP = 60   # S106 GSN N7: at most this many units are added for a CMake-input change (see cmake_flag_units)
CMAKE_UNITS_PER_SIG = 8   # ... and at most this many per distinct flag change (a dropped include dir moves every unit the same way: eight of them show it, not 600)


def _flag_map(lines, main):
    """unit rel path (src/...) -> its flags list, for every src unit of the compile lines (root = main's root, as build_lines_for returns them)."""
    pre = main.replace('\\', '/').rstrip('/').lower() + '/'
    out = {}
    for ln in lines:
        m = UNIT_RE.search(ln)
        if not m:
            continue
        p = m.group(1).replace('\\', '/')
        if not p.lower().startswith(pre + 'src/'):
            continue
        rel = p[len(pre):]
        try:
            out[rel] = [f for f in flags_for([ln], main, rel) if not (f.startswith('-I') and f.replace('\\', '/').rstrip('/').endswith('/stamp'))]   # the binary-dir stamp folder differs per key by name only
        except RuntimeError:
            pass
    return out


def cmake_flag_units(tree_lines, base_lines, main, have=()):
    """S106 GSN N7: the src units whose flag LIST differs between two configures (the checked tree's and the base's), both from syntax_cfg: what a CMake-input change (include dir, define, option)
    moves. Returns (units to add, total differing, note). Cap: CMAKE_UNITS_PER_SIG per distinct difference (the tokens added / removed) and CMAKE_UNITS_CAP overall, sorted, units already in `have` first
    (they are checked anyway and count toward neither). A unit only one side compiles is not a flag difference (its source is in the changed set when it is new)."""
    a, b = _flag_map(tree_lines, main), _flag_map(base_lines, main)
    sigs = {}
    for u in sorted(set(a) & set(b)):
        if a[u] != b[u]:
            sa, sb = set(a[u]), set(b[u])
            sigs.setdefault((tuple(sorted(sa - sb)), tuple(sorted(sb - sa))), []).append(u)
    total = sum(len(v) for v in sigs.values())
    pick = []
    for _s, us in sorted(sigs.items()):
        rest = [u for u in us if u not in have]
        pick.extend(rest[:CMAKE_UNITS_PER_SIG] if len(us) == len(rest) or rest else [])
    pick = sorted(set(pick))[:CMAKE_UNITS_CAP]
    return pick, total, '%d units have different flags from the base; %d signature(s); checking %d (cap %d per signature, %d overall)' % (
        total, len(sigs), len(pick), CMAKE_UNITS_PER_SIG, CMAKE_UNITS_CAP)


def tool_build_notes(root, changed):
    """Changed src files that a tools\\**\\build*.ps1 names: tool / viewer builds compile them without the game's flags, which game_syntax cannot see."""
    texts = []
    for dp, _d, fn in os.walk(os.path.join(root, 'tools')):
        for f in fn:
            if f.startswith('build') and f.endswith('.ps1'):
                p = os.path.join(dp, f)
                try:
                    texts.append((p, open(p, encoding='utf-8', errors='replace').read().replace('\\', '/')))
                except OSError:
                    pass
    hits = {}
    for c in changed:
        if c.startswith('src/'):
            for p, t in texts:
                if c in t or c[4:] in t:
                    hits.setdefault(c, []).append(os.path.relpath(p, root).replace('\\', '/'))
    return hits


def check_tool_builds(root, main_dir, units, unit_flags, notes):
    """S106 G4-4F (review F2): the tools build scripts (tools/**/build*.ps1) compile some src units WITHOUT the build's force-include of src/pch_units.h (fview, pass2d, case, crowd / arena tests, the viewer).
    A changed game unit that `notes` (tool_build_notes) names is parsed once more with no force-include; a failure there is returned as (unit, 'also compiled by <scripts> without pch_units.h', clang text)."""
    bad = []
    global FORCE_INC
    saved = FORCE_INC
    FORCE_INC = None
    try:
        for u in units:
            if u not in notes or u not in unit_flags or u == 'src/main.cpp' or u.startswith(LAUNCHER_PREFIXES) or not u.endswith('.cpp'):
                continue
            label, rc, text, _k, _h = compile_unit(root, main_dir, unit_flags[u], os.path.join(root, u), u)
            if rc:
                bad.append((u, 'also compiled by ' + ', '.join(notes[u][:3]) + ' without pch_units.h', text))
    finally:
        FORCE_INC = saved
    return bad


def _read(p):
    try:
        return open(p, 'rb').read()
    except OSError:
        return None


def cmake_content_unchanged_since(main_dir, t, rel='CMakeLists.txt'):
    """S105 E (S106 GSN N3: any `rel`, default CMakeLists.txt): True when main's `rel` holds the same CONTENT it had when build.ninja was written at time t (a touched / re-checked-out file has a newer mtime and the same bytes:
    that was a false NEEDS-MAIN alarm). Stamp = the checkout the ninja was configured from: the newest `git reflog HEAD` entry made at or before t (whole seconds); the working file must equal that
    commit's. (Not `git log -1 -- file`: it skips merge commits, so a branch change committed before the configure and merged after it read "unchanged".) No reflog entry that old, or any git
    error = False (the alarm stays)."""
    def g(*a):
        r = subprocess.run(['git', '-C', main_dir] + list(a), capture_output=True, text=True)
        return r.returncode, r.stdout
    try:
        rc, out = g('reflog', 'show', '--date=unix', '--format=%H %gd', 'HEAD')
        if rc:
            return False
        for ln in out.splitlines():   # newest first
            m = re.match(r'^([0-9a-f]{40}) HEAD@\{(\d+)\}$', ln.strip())
            if m and int(m.group(2)) <= int(t):
                rc, _ = g('diff', '--quiet', m.group(1), '--', rel)
                return rc == 0
        return False
    except (OSError, ValueError):
        return False


def cmake_state(root, main_dir, builds):
    """Why the main checkout's configured builds may not describe this tree: any file configure reads (syntax_cfg.REAL_INPUTS) differs from main's, or a build.ninja is older than ANY of them
    in main (S106 GSN note 3: it used to test CMakeLists.txt's mtime only; a newer VERSION / preset / keep_stream list / rexglue.cmake is the same stale build)."""
    why = []
    for f in _cfg.REAL_INPUTS:   # S106 GSMAINF N6: every file configure reads (not just CMakeLists.txt / CMakePresets.json)
        if _read(os.path.join(root, f)) != _read(os.path.join(main_dir, f)):
            why.append('%s differs from the main checkout\'s' % f)
    for b in builds:
        ninja = os.path.join(main_dir, BUILD_DIRS[b], 'build.ninja')
        if not os.path.isfile(ninja):
            continue
        for f in _cfg.REAL_INPUTS:
            fp = os.path.join(main_dir, f)
            if os.path.isfile(fp) and os.path.getmtime(ninja) < os.path.getmtime(fp) and not cmake_content_unchanged_since(main_dir, os.path.getmtime(ninja), f):
                why.append('%s build.ninja is older than %s (reconfigure)' % (b, f))
                break
    return why


def fixed_list_verdict(why, n_files, twin=True):
    """The fixed-list mode's last line. On the fallback to main's out\\build with reasons it does not describe the tree: PASS (units) but NEEDS-MAIN, exit 4 (S106 GSMAINF N6), like --changed."""
    if why:
        print('PASS (units) but NEEDS-MAIN: %s: the main checkout\'s configured builds do not describe this tree; the main session re-runs this after the merge on the reconfigured build' % '; '.join(why))
        return 4
    print('PASS: %d game translation units compile (-fsyntax-only, the main build\'s flags, this tree\'s src first)%s' % (n_files, '; the planted twin is caught' if twin else ''))
    return 0


def run_changed(a, root, main_dir):
    own_main = _same_dir(root, main_dir)
    base = a.changed if a.changed != 'auto' else default_base(root, main_dir)
    if not base:
        print('FAIL: no base for --changed (not a git tree, or no merge-base)')
        return 1
    ch = changed_files(root, base)
    if ch is None:
        print('FAIL: git diff / ls-files failed for base %s: the changed set is unknown (never treated as "no changes")' % base)
        return 1
    if a.files or a.no_twin:
        print('note: --files / --no-twin are ignored with --changed')
    t0 = time.time()
    units = units_for_changed(root, [c for c in ch if c.startswith('src/')])
    print('base %s: %d changed files -> %d src units' % (base, len(ch), len(units)))
    for c, ps in sorted(tool_build_notes(root, ch).items()):
        print('note: %s is also compiled by tool builds (%s): build those tools (game_syntax cannot see them)' % (c, ', '.join(ps[:3])))
    builds = ['on', 'off'] if a.build == 'both' else [a.build]
    why = []   # S106 GSMAIN: filled by build_lines_for only on the fallback to main's out\build (cmake_state's reasons)
    cmake_txt = (_read(os.path.join(root, 'CMakeLists.txt')) or b'').decode('utf-8', 'replace').replace('\\', '/')
    cache_file = load_cache(main_dir)
    jobs, skipped, per_unit = [], [], {u: 0 for u in units}
    unit_flags = {}   # S106 G4-4F: unit -> its flags (first build), for the tool-build second parse
    build_lines = {}
    for b in builds:
        try:
            build_lines[b] = build_lines_for(root, main_dir, b, why)   # S106 GSMAIN: the tree's own configure (was: build_commands(main_dir, BUILD_DIRS[b]))
        except RuntimeError as e:   # ConfigureFailed (the tree does not configure) or the fallback's own failure
            print('FAIL: [%s] %s' % (b, e))
            return 1
    print(_cfg.note_line())
    # S106 GSN N7: a CMake input (CMakeLists.txt, presets, VERSION, the keep_stream lists, ...) that changed since the base moves flags, not sources: add the units whose flag list differs between
    # this tree's configure and the base's (both syntax_cfg), capped; a comment-only edit moves no flag and adds none.
    cm_changed = [f for f in _cfg.REAL_INPUTS if f in ch]
    if cm_changed:
        added = set()
        for b in builds:
            try:
                base_lines = _cfg.commands(_cfg.RevTree(root, base), main_dir, b)
            except (_cfg.Unavailable, _cfg.ConfigureFailed) as e:
                msg = 'the base\'s own configure (%s) is not available (%s): the units a CMake change moves are unknown' % (b, e)
                print('note: ' + msg)
                why.append(msg)
                continue
            pick, total, note = cmake_flag_units(build_lines[b], base_lines, main_dir, units)
            print('note: [%s] %s changed since %s: %s' % (b, ', '.join(cm_changed), base, note))
            added.update(pick)
        extra = sorted(added - set(units))
        if extra:
            print('note: %d unit(s) added because their flags changed: %s' % (len(extra), ', '.join(extra[:12]) + (' ...' if len(extra) > 12 else '')))
            units = sorted(set(units) | set(extra))
            per_unit.update({u: 0 for u in extra})
    for b in builds:
        want, other = ('-DWWE2K14_KEEP_STREAM=1', '-DWWE2K14_KEEP_STREAM=0') if b == 'on' else ('-DWWE2K14_KEEP_STREAM=0', '-DWWE2K14_KEEP_STREAM=1')
        for u in units:
            try:
                fl = flags_for(build_lines[b], main_dir.replace('\\', '/'), u)
                per_unit[u] += 1
            except RuntimeError:
                skipped.append((u, b))
                continue
            # S103: a launcher unit (src/launcher/*) is the wwe2k14_launcher target: its own flags from the same build.ninja carry no KEEP_STREAM define (it is not the game), so the
            # wrong-build check below is for game units only (S103 DLF: also src/audio_import/audio_import.cpp, launcher-only per CMakeLists; its sibling lookup.cpp is a game unit and stays checked); the launcher unit is compiled with its target's own flags (one job per build: the same flags in both).
            if not u.startswith(LAUNCHER_PREFIXES) and (want not in fl or other in fl):
                print('FAIL: the %s build\'s flags for %s do not carry %s (or carry %s): the wrong build was read' % (b, u, want, other))
                return 1
            fl = with_pch(root, u, fl)
            unit_flags.setdefault(u, fl)
            jobs.append(('%s [%s]' % (u, b), fl, os.path.join(root, u)))
    # a unit NO requested build compiles: if the tree's CMakeLists.txt names it, borrow src/start_settings.cpp's flags (all units but main.cpp / build_stamp.cpp share them) and
    # compile it in every requested build; one the CMake file does not name is tool-only (fview, viewers): left to its own lane, listed below
    notdone = []
    # S111 COMPILEOFF follow-up: a STREAM-ONLY unit (tools/scene/keep_stream_sources.txt, the list CMakeLists.txt drops from the off build)
    # is not compiled unless the stream build is asked for by name (Jarrod 2026-11-27: "we should not be compiling into it"); borrowing the
    # off build's flags for it compiled it in the wrong build and failed on stream-only names.
    stream_only = set()
    if 'on' not in builds:
        for ln in (_read(os.path.join(root, 'tools', 'scene', 'keep_stream_sources.txt')) or b'').decode('utf-8', 'replace').splitlines():
            ln = ln.strip().replace('\\', '/')
            if ln.startswith('src/'):
                stream_only.add(ln)
    stream_skipped = []
    for u in units:
        if per_unit[u]:
            continue
        if u in stream_only:
            stream_skipped.append(u)
            continue
        if u in cmake_txt:
            for b in builds:
                fl = flags_for(build_lines[b], main_dir.replace('\\', '/'), 'src/start_settings.cpp')
                jobs.append(('%s [%s] (not in the configured build: start_settings.cpp\'s flags borrowed)' % (u, b), fl, os.path.join(root, u)))
        else:
            notdone.append(u)
    shown = [s for s in skipped if per_unit[s[0]]]
    if shown:
        print('skipped (that build does not compile the unit): ' + ', '.join('%s [%s]' % s for s in shown))
    if notdone:
        print('not compiled by any game build (tool-only; run its own lane): ' + ', '.join(notdone))
    if stream_skipped:
        print('stream-only (not compiled: ask for --build on by name): ' + ', '.join(stream_skipped))
    bad = 0
    refs = []
    set_force(root, [u for u in units if u != 'src/main.cpp'])   # S106 G4-4 / 4F: every GAME unit (not the launcher target, not main.cpp) parses with the build's force-include (PCH or plain text)
    if jobs and not NO_MAIN_PCH and not os.environ.get('GAME_SYNTAX_NO_CACHE') and os.path.isfile(os.path.join(root, 'src', 'pch_main.h')):
        for b in builds:   # S110 SYNTAXPCH: main.cpp parses through a PCH of src/pch_main.h (one per build), like the real build; --no-pch = the plain-text parse
            try:
                refs.append(main_pch_ref(root, main_dir, flags_for(build_lines[b], main_dir.replace('\\', '/'), 'src/main.cpp')))
            except RuntimeError:
                pass   # a build that does not compile main.cpp
    if jobs and not os.environ.get('GAME_SYNTAX_NO_CACHE') and os.path.isfile(_pch.header_path(root)):
        for b in builds:   # S105 PCH: one reference per build = start_settings.cpp's flags (every plain game unit has them); launcher / importer units and main.cpp differ and skip the PCH
            refs.append(_pch.make_ref(root, _clean(tree_args(root, main_dir, flags_for(build_lines[b], main_dir.replace('\\', '/'), 'src/start_settings.cpp')))))
    if jobs:
        # S103 2026-11-19 11:38: free RAM hit 0.0 GB with several game_syntax runs (main session + builders) of 8 clang parses each at once.
        # Size the pool by FREE RAM like q's build_ours (2.5 GB a parse, 6 GB left over), at most 8, at least 1.
        # CQW: and by the machine-wide compile queue (tools\admit\admit_py.py; the formula above stays as the want and the fall-open value), held until the pool ends.
        res = run_jobs(jobs, root, main_dir, pch_refs=refs)   # S105 CC: store lookups first (no queue), the admitted pool only for the misses
        for label, rc, text, key, hit in res:
            print('%s %s%s' % ('ok  ' if rc == 0 else 'FAIL', label, ' (cached)' if hit and rc == 0 else (' (cached failure)' if hit else '')))
            if rc:
                bad += 1
                print(text[:2000])
        save_cache(cache_file)
        if refs:
            print('PCH: %d units parsed with it, %d rebuilt after a clang rejection, %d fell back, %d PCH builds (%s)' % (
                PCH_STATS['used'], PCH_STATS['rebuilt'], PCH_STATS['fallback'], sum(r['builds'] for r in refs), '+'.join(builds)))
            for r in (r for r in refs if r.get('main')):
                print('main.cpp PCH (pch_main.h, %s build): %s' % ('on' if '-DWWE2K14_KEEP_STREAM=1' in r['args'] else 'off',
                      ('built in %.1f s' % r['build_s']) if r['builds'] else ('reused' if r['path'] else 'not needed (store hit) or not built')))
            for r in refs:
                if r['note']:
                    print('PCH NOTE: %s' % r['note'])
            if PCH_SKIPS:
                print('PCH skipped for %d units (plain parse): %s' % (len(PCH_SKIPS), '; '.join('%s: %s' % s for s in PCH_SKIPS[:6]) + (' ...' if len(PCH_SKIPS) > 6 else '')))
    if not bad and jobs:   # S106 G4-4F (review F2): the second parse of units a tool build also compiles, without the force-include
        for u, tool, text in check_tool_builds(root, main_dir, units, unit_flags, tool_build_notes(root, ch)):
            print('FAIL: %s: %s (the tool build has no force-include: add the STL header the unit needs)' % (u, tool))
            print(text[:1200])
            bad += 1
    dt = time.time() - t0
    if bad:
        print('FAIL: %d of %d unit-builds (%s), %.0f s' % (bad, len(jobs), '+'.join(builds), dt))
        return 1
    if not jobs:
        print('NOTHING CHECKED: 0 game units touched since %s (exit 0, but this is not a PASS: %d changed files, none reach a game unit)' % (base, len(ch)))
        return 0
    if why:
        print('PASS (units) but NEEDS-MAIN: %s: the main checkout\'s configured builds do not describe this tree; the main session re-runs this after the merge on the reconfigured build' % '; '.join(why))
        print('%d unit-builds (%s), %.0f s (store: %d hits, %d parsed)' % (len(jobs), '+'.join(builds), dt, STATS['hit'], STATS['miss']))
        return 4
    if own_main and a.changed == 'auto':
        hd = git_lines(root, 'rev-parse', 'HEAD')
        if hd:
            try:
                os.makedirs(os.path.dirname(last_pass_file(main_dir, root)), exist_ok=True)
                open(last_pass_file(main_dir, root), 'w').write(hd[0] + '\n')
            except OSError as e:
                print('note: last-pass sha not saved (%s)' % e)
    print('PASS: %d unit-builds (%s), %.0f s (store: %d hits, %d parsed)' % (len(jobs), '+'.join(builds), dt, STATS['hit'], STATS['miss']))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--changed', nargs='?', const='auto', default=None, metavar='BASE')
    ap.add_argument('--build', choices=('on', 'off', 'both'), default='off', help="default off = the stream-free build the players run; on = the stream build, both = on + off (only by name)")
    ap.add_argument('--root', default='.')
    ap.add_argument('--files', default='')
    ap.add_argument('--no-twin', action='store_true')
    ap.add_argument('--no-cache', action='store_true', help='neither read nor write the result store: every unit is parsed')
    ap.add_argument('--no-pch', action='store_true', help='main.cpp is parsed with pch_main.h as plain text (no PCH for it), as before S110 SYNTAXPCH')
    a = ap.parse_args()
    global NO_MAIN_PCH
    NO_MAIN_PCH = a.no_pch
    if a.no_cache:
        os.environ['GAME_SYNTAX_NO_CACHE'] = '1'
    root = os.path.abspath(a.root)
    if not os.path.isfile(CLANG):
        print('FAIL: %s not found' % CLANG)
        return 1
    main_dir = main_checkout(root)
    if not main_dir or not os.path.isdir(os.path.join(main_dir, 'generated', 'default')):
        print('FAIL: the main checkout with generated\\default was not found (%s): the game units cannot be checked' % main_dir)
        return 1
    if a.changed:
        return run_changed(a, root, main_dir)
    cache_file = load_cache(main_dir)
    files = DEFAULT_FILES + [f.strip().replace('\\', '/') for f in a.files.split(',') if f.strip()]
    why = []   # S106 GSMAINF N6: filled on the fallback to main's out\\build, like the --changed mode
    try:
        lines = build_lines_for(root, main_dir, 'off', why)   # S106 GSMAIN: the tree's own configure (was: build_commands(main_dir)); S111 COMPILEOFF: the stream-free build, not the stream one
    except RuntimeError as e:
        print('FAIL: ' + str(e))
        return 1
    # S103 DIB2 S4: a unit that cannot be checked (a --files entry that is missing or that the main build does not compile) FAILS THAT UNIT ONLY: every other unit
    # still compiles and prints its own line, so the shared run's other checks keep their verdicts (it used to exit before compiling anything)
    flags, jobs, bad_units = {}, [], 0
    for f in files:
        p = os.path.join(root, f)
        try:
            flags[f] = flags_for(lines, main_dir.replace('\\', '/'), f)
        except RuntimeError as e:
            print('FAIL %s\nerror: %s' % (f, e))
            bad_units += 1
            continue
        if not os.path.isfile(p):
            print('FAIL %s\nerror: missing %s' % (f, p))
            bad_units += 1
            continue
        jobs.append((f, flags[f], p, False))
    if 'src/main.cpp' not in flags:
        print('FAIL: src/main.cpp has no compile command: the planted twin cannot run')
        return 1
    if not a.no_twin:
        d = os.path.join(root, 'out', 'game_syntax')
        os.makedirs(d, exist_ok=True)
        twin = os.path.join(d, 'plant_small.cpp')
        open(twin, 'w').write('#include "%s/src/main.cpp"\nstruct PlantSmall { int small; };   // planted: `small` is a macro of the Windows RPC headers main.cpp pulls in\n' % root.replace('\\', '/'))
        jobs.append(('planted twin (a member named small after main.cpp)', flags['src/main.cpp'], twin, True))
    with _admit.hold('syntax.worker', 4) as nw, ThreadPoolExecutor(max_workers=nw) as ex:   # CQW: the fixed-list path's pool (4 = the want and the fall-open value)
        res = list(ex.map(lambda j: (compile_unit(root, main_dir, j[1], j[2], j[0], cache_fail=j[3]), j[3]), jobs))
    bad = bad_units
    hits = sum(1 for (r, _t) in res if r[4])
    for (label, rc, text, key, hit), is_twin in res:
        if is_twin:
            if rc == 0:
                print('FAIL: the planted twin compiled: `small` is not a live macro in main.cpp\'s unit, so this check cannot see the G12 class of fault')
                bad += 1
            else:
                print('ok   %s: failed to compile, as planted (%s)' % (label, (re.findall(r'error: [^\n]*', text) or ['?'])[0][:120]))
        else:
            print('%s %s' % ('ok  ' if rc == 0 else 'FAIL', label))
            if rc:
                bad += 1
                print(text[:2000])
    save_cache(cache_file)
    if bad:
        return 1
    print('pp-cache: %d of %d units served by the preprocess hash (no parse)' % (hits, len(res)))
    return fixed_list_verdict(why, len(files), not a.no_twin)


if __name__ == '__main__':
    sys.exit(main())
