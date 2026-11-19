#!/usr/bin/env python3
"""Gate check: the game exe's translation units still compile (S102 T11 follow-up; G12 named a struct field `small`, a macro of the Windows RPC headers the GAME build
includes: fview and rplay compiled, the game exe did not, and nobody saw it until the build).

  py -I tools\\gate\\game_syntax.py [--root DIR] [--files src/a.cpp,src/b.cpp] [--no-twin]
  py -I tools\\gate\\game_syntax.py --changed [BASE] [--build on|off|both] [--root DIR]     (S103 GSB: the main session runs it after every merge)

--changed: every src\\*.cpp unit the change touched (the file itself, or a unit that includes a changed header, found by a quoted-#include scan of THIS tree's src) is syntax-checked in BOTH
game builds, each with that build's own flags (the stream build out\\build\\win-amd64-release and the stream-free build out\\build\\win-amd64-release-off, read from the main checkout); a unit a build
does not compile is skipped for that build only. BASE default: HEAD^1 when HEAD is a merge commit, else the merge-base with the main checkout's current branch (the rule of
tools\\gate\\fastgate.py only_base, kept local here so this file imports no unmerged change); the changed set also holds uncommitted edits. Without --changed the old fixed list (stream build only) runs as before.
Not covered: Android guards, the AVX2 (off-v3) build, tool builds (a note only); --files / --no-twin are ignored with --changed. On the main checkout the default base is the last sha this tool PASSED
on the branch (out\\game_syntax\\last_pass_<branch>.txt) when an ancestor of HEAD, else HEAD^1; in any other tree (a builder worktree) it is the merge-base with the main checkout's branch. 0 units = `NOTHING CHECKED`
(exit 0, not a PASS); a failed git call = FAIL; a tree whose CMakeLists.txt differs from main's (or a build.ninja older than it) = exit 4 NEEDS-MAIN after the units compile.
Not seen: tool / viewer builds (they list their own sources in tools\\**\\build*.ps1): a changed file one of those names prints a note to run the lanes that build it.

clang -fsyntax-only of src\\main.cpp and src\\renderer_adapter\\live.cpp (and any unit --files names; each must be one the main build compiles), with the flags the MAIN
checkout's own build uses (`ninja -t commands` in out\\build\\win-amd64-release: defines, standard, system includes), the tree's src (--root) put first so a worktree's
edits are what gets compiled; includes the main checkout still owns (generated\\default, thirdparty) come after.
Add it to any lane with:   {"name": "<lane>.game_syntax", "cmd": ["py", "-I", "tools/gate/game_syntax.py", "--root", "."], "timeout_s": 900,
                            "needs": ["generated/default", "thirdparty/rexglue-sdk-vulkan/include"], "affects": [<the headers the lane edits>]}
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
import proc as _proc   # QTF: every child runs under a Windows Job Object + hard timeout (a 10-hour hang happened with none)

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


def compile_unit(root, main, flags, unit_path, label, cache_fail=False):
    wt = os.path.abspath(root).replace('\\', '/')
    m = main.replace('\\', '/')
    # the tree's own roots first; the main checkout's after (generated code, thirdparty, ../thirdparty relative includes)
    # Only the roots the unit's own flags name: the launcher target has no -I<repo root>, and adding one made <version> (via
    # nlohmann/json.hpp) resolve to a stray repo-root file "version" (S103 23:4x).
    had = set()
    kept = []
    for f in flags:
        p = f[2:].replace('\\', '/').rstrip('/') if f.startswith('-I') else None
        if p in (m, m + '/src'):
            had.add(p[len(m):])   # '' or '/src': replaced by the tree's own root below
            continue
        kept.append(f)
    order = [s for s in ('', '/src') if s in had]
    inc = ['-I' + wt + s for s in order] + ['-I' + m + s for s in order]
    rc, text, key, hit = syntax_check(kept + inc, unit_path, roots=((wt, '<ROOT>'), (m, '<MAIN>')), cache_fail=cache_fail)
    return label, rc, text, key, hit


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


def pp_key(args, unit_path, roots):
    """Key of one unit: hash of `clang -E -P` of it with the full flag list + the placeholder-normalised flags + the clang. None when the preprocess fails."""
    pp = _proc.run([CLANG] + args + ['-E', '-P', '-Wno-everything', unit_path], timeout=T_PP, text=False)
    if pp.returncode != 0 or os.environ.get('GAME_SYNTAX_NO_CACHE'):
        return None
    d = hashlib.sha256(pp.stdout)
    d.update(('\0'.join(norm_args(args, roots)) + '\0' + CLANG_ID).encode())
    return d.hexdigest()


def syntax_check(args, unit_path, roots=(), cache_fail=False):
    """clang -fsyntax-only of one unit with the full flag list `args`, served by the preprocessed-text cache. Returns (rc, text, key, hit). The verdict is STORED here (PP_CACHE[key]; the caller calls
    save_cache): a pass always, a failure only when cache_fail (the planted twin). Shared with tools/scene/keep_stream_off_link.py --syntax (S103 GSP: one cache, one rule).
    S103 H3 (audit 1.3 item 5): the object cache cannot serve -fsyntax-only, so key the result on the hash of `clang -E -P` of the same unit with the same
    flags (the key sccache's preprocessor mode uses): identical preprocessed text + identical flags + the same clang = the same verdict. A hit skips the
    parse (the expensive part); the preprocess always runs, so an edited header can never hit. Only an exit-0 preprocess is keyed.
    GSP2 review bug 6: the preprocess and the parse are two reads of the tree; a header fixed in between would store a PASS under the broken text's key. After the parse the unit is preprocessed AGAIN
    and the verdict is stored only when that key equals the first (a changed key = not stored, counted in NOT_STORED). The flags' tree paths are placeholders in the key (roots), so worktrees share hits.
    Not in the key: the contents of a response file named in the flags (`@$DYNDEP_MODULE_MAP_FILE`; theoretical, noted)."""
    key = pp_key(args, unit_path, roots)
    if key is not None:
        c = PP_CACHE.get(key)
        if c is not None:
            return c, "error: pp-cache hit (cached %s verdict: same preprocessed text and flags as an earlier run)" % ("failure" if c else "pass"), key, True
    r = _proc.run([CLANG] + args + ['-fsyntax-only', '-Wno-everything', unit_path], timeout=T_UNIT)
    if r.timed_out:
        r.stderr = (r.stderr or '') + '\nerror: game_syntax: clang timed out after %d s on %s (process tree killed)' % (T_UNIT, unit_path)
    if key is not None and not r.timed_out and (r.returncode == 0 or cache_fail):
        if pp_key(args, unit_path, roots) == key:
            PP_CACHE[key] = r.returncode
        else:
            NOT_STORED.append(unit_path)
    return r.returncode, (r.stdout + r.stderr), key, False


def load_cache(main_dir):
    """Reads <main>/out/game_syntax/pp_cache.json into PP_CACHE; returns the file path."""
    cache_file = os.path.join(main_dir, 'out', 'game_syntax', 'pp_cache.json')
    try:
        PP_CACHE.update(json.load(open(cache_file)))
    except Exception:
        pass
    return cache_file


def save_cache(cache_file):
    """Merge PP_CACHE into the file (write a temp, os.replace). A concurrent reader (Windows) can make the replace fail for a moment: retried before giving up."""
    old = {}
    try:
        os.makedirs(os.path.dirname(cache_file), exist_ok=True)
        tmp = cache_file + '.%d.tmp' % os.getpid()
        try:
            old = json.load(open(cache_file))
        except Exception:
            pass
        old.update(PP_CACHE)
        json.dump(old, open(tmp, 'w'))
        last = None
        for _ in range(25):
            try:
                os.replace(tmp, cache_file)
                return
            except OSError as e:
                last = e
                time.sleep(0.2)
        raise last
    except OSError as e:
        print('note: pp cache not saved (%s)' % e)


def _clang_id():
    try:
        st = os.stat(CLANG)
        return '%d:%d' % (st.st_size, st.st_mtime_ns)
    except OSError:
        return '?'


CLANG_ID = _clang_id()
NOT_STORED = []   # units whose verdict was not stored because the tree changed between the first preprocess and the parse (bug 6)
PP_CACHE = {}   # preprocess hash -> compile exit code (0 for a real unit; nonzero for the planted twin); file: <main>/out/game_syntax/pp_cache.json


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
    """The commit --changed diffs against. Same behaviour as tools\\gate\\fastgate.py only_base (GOL fixes the same rule there; keep the two identical):
    - a tree that is NOT the main checkout (a builder worktree, a gate-main checkout) -> merge-base(HEAD, the main checkout's branch): HEAD^1 of a merge there is only what the session brought in
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
    seen = set(_norm(os.path.join(root, c)) for c in changed)
    todo = list(seen)
    while todo:
        k = todo.pop()
        for p in rev.get(k, []):
            if _norm(p) not in seen:
                seen.add(_norm(p))
                todo.append(_norm(p))
    return sorted(os.path.relpath(p, root).replace('\\', '/') for p in files if p.endswith('.cpp') and _norm(p) in seen)


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


def _read(p):
    try:
        return open(p, 'rb').read()
    except OSError:
        return None


def cmake_state(root, main_dir, builds):
    """Why the main checkout's configured builds may not describe this tree: its CMakeLists.txt / CMakePresets.json differ, or a build.ninja is older than main's CMakeLists.txt."""
    why = []
    for f in ('CMakeLists.txt', 'CMakePresets.json'):
        if _read(os.path.join(root, f)) != _read(os.path.join(main_dir, f)):
            why.append('%s differs from the main checkout\'s' % f)
    cm = os.path.join(main_dir, 'CMakeLists.txt')
    for b in builds:
        ninja = os.path.join(main_dir, BUILD_DIRS[b], 'build.ninja')
        if os.path.isfile(cm) and os.path.isfile(ninja) and os.path.getmtime(ninja) < os.path.getmtime(cm):
            why.append('%s build.ninja is older than CMakeLists.txt (reconfigure)' % b)
    return why


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
        print('note: %s is also compiled by tool builds (%s): run the lanes that build tools from it (game_syntax cannot see them)' % (c, ', '.join(ps[:3])))
    builds = ['on', 'off'] if a.build == 'both' else [a.build]
    why = cmake_state(root, main_dir, builds)
    cmake_txt = (_read(os.path.join(root, 'CMakeLists.txt')) or b'').decode('utf-8', 'replace').replace('\\', '/')
    cache_file = load_cache(main_dir)
    jobs, skipped, per_unit = [], [], {u: 0 for u in units}
    build_lines = {}
    for b in builds:
        try:
            build_lines[b] = build_commands(main_dir, BUILD_DIRS[b])
        except RuntimeError as e:
            print('FAIL: [%s] %s' % (b, e))
            return 1
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
            if not u.startswith(('src/launcher/', 'src/importer/', 'src/audio_import/audio_import.cpp')) and (want not in fl or other in fl):
                print('FAIL: the %s build\'s flags for %s do not carry %s (or carry %s): the wrong build was read' % (b, u, want, other))
                return 1
            if u == 'src/main.cpp':
                # the build's PCH pair is dropped from the flags; main.cpp gets the tree's own pch_main.h first instead (the G12 macro-order class: the PCH's headers come before main.cpp's own)
                fl = fl + ['-include', os.path.abspath(os.path.join(root, 'src', 'pch_main.h')).replace('\\', '/')]
            jobs.append(('%s [%s]' % (u, b), fl, os.path.join(root, u)))
    # a unit NO requested build compiles: if the tree's CMakeLists.txt names it, borrow src/start_settings.cpp's flags (all units but main.cpp / build_stamp.cpp share them) and
    # compile it in every requested build; one the CMake file does not name is tool-only (fview, viewers): left to its own lane, listed below
    notdone = []
    for u in units:
        if per_unit[u]:
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
    bad = 0
    if jobs:
        # S103 2026-11-19 11:38: free RAM hit 0.0 GB with several game_syntax runs (main session + builders) of 8 clang parses each at once.
        # Size the pool by FREE RAM like q's build_ours (2.5 GB a parse, 6 GB left over), at most 8, at least 1.
        with ThreadPoolExecutor(max_workers=_syntax_workers()) as ex:
            res = list(ex.map(lambda j: compile_unit(root, main_dir, j[1], j[2], j[0]), jobs))
        for label, rc, text, key, hit in res:
            print('%s %s%s' % ('ok  ' if rc == 0 else 'FAIL', label, ' (cached)' if hit and rc == 0 else ''))
            if rc:
                bad += 1
                print(text[:2000])
        save_cache(cache_file)
    dt = time.time() - t0
    if bad:
        print('FAIL: %d of %d unit-builds (%s), %.0f s' % (bad, len(jobs), '+'.join(builds), dt))
        return 1
    if not jobs:
        print('NOTHING CHECKED: 0 game units touched since %s (exit 0, but this is not a PASS: %d changed files, none reach a game unit)' % (base, len(ch)))
        return 0
    if why:
        print('PASS (units) but NEEDS-MAIN: %s: the main checkout\'s configured builds do not describe this tree; the main session re-runs this after the merge on the reconfigured build' % '; '.join(why))
        print('%d unit-builds (%s), %.0f s' % (len(jobs), '+'.join(builds), dt))
        return 4
    if own_main and a.changed == 'auto':
        hd = git_lines(root, 'rev-parse', 'HEAD')
        if hd:
            try:
                os.makedirs(os.path.dirname(last_pass_file(main_dir, root)), exist_ok=True)
                open(last_pass_file(main_dir, root), 'w').write(hd[0] + '\n')
            except OSError as e:
                print('note: last-pass sha not saved (%s)' % e)
    print('PASS: %d unit-builds (%s), %.0f s' % (len(jobs), '+'.join(builds), dt))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--changed', nargs='?', const='auto', default=None, metavar='BASE')
    ap.add_argument('--build', choices=('on', 'off', 'both'), default='both')
    ap.add_argument('--root', default='.')
    ap.add_argument('--files', default='')
    ap.add_argument('--no-twin', action='store_true')
    a = ap.parse_args()
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
    try:
        lines = build_commands(main_dir)
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
    with ThreadPoolExecutor(max_workers=4) as ex:
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
    print('PASS: %d game translation units compile (-fsyntax-only, the main build\'s flags, this tree\'s src first)%s' % (len(files), '; the planted twin is caught' if not a.no_twin else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
