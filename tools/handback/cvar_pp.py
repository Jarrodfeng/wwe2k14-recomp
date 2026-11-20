#!/usr/bin/env python3
r"""cvar_pp (lane LNK round 3, 2026-11-20): what each game unit REALLY compiles of the cvars, asked of the real preprocessor, not modelled.

For each (unit, mode) of the game target the unit's own compile command (the configured build's flags, pointed at the tree: tools\gate\game_syntax.py flags_for / tree_args /
with_pch, the same command lines the syntax check uses) runs with `-E -fdirectives-only` in place of the compile: clang resolves every #if / #include exactly (values, the SDK's
macros, guard-less headers, `#include MACRO`, a unit's own #define of its mode macro) and leaves macro INVOCATIONS unexpanded, so the text still holds `REXCVAR_DEFINE_*(name`,
`REXCVAR_DECLARE(type, name)`, `REXCVAR_GET(name)` / `REXCVAR_SET(name`. Those, plus a hand-written `FLAGS_<name>_storage_() {` (an inline definition) and a call of a macro whose
body reads a literal cvar (src\replay_guard.h W3_GUARD_TEST_FRAME), are what this scans. Directive lines and string literals are not scanned (clang already drops comments).

Units: the game target per the TREE's lists (CMakeLists.txt `set(WWE2K14_SOURCES ...)` + `target_sources(wwe2k14 ...)`, minus tools\scene\keep_stream_sources.txt in the OFF
build, plus keep_stream_off_sources.txt there). A unit the configured build compiles uses its own flags; one it does not compile and the main checkout's CMakeLists.txt does not
name either (new in this tree) borrows src\start_settings.cpp's (as game_syntax does); one the main CMakeLists.txt names that the configured build skips is NOT JUDGED (a platform
branch). A unit whose preprocess fails is BLOCKED (its first error line), never clean. Mode 2 ("ON tool build"): an OFF-only game unit that a tool build compiles in ON
(tools\**\build*.ps1 / .bat, a gate lane json with --syntax naming it), preprocessed with start_settings.cpp's ON flags; only its undeclared reads are judged (S103 SCS).

Cache (scratch, <main>\out\cvar_pp\): per (unit, mode, the placeholder-normalised command line, the clang): the result and the files the output's own `# N "file"` markers name, each
with a stamp (a tree file: its content hash, so worktrees and the base tree share entries; any other file: size + mtime). A re-run preprocesses only units whose stamps changed.
Not seen by the stamps: a NEW file that would now shadow an include earlier in the search path.
"""
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'gate'))
sys.path.insert(0, os.path.join(HERE, '..', 'admit'))
import admit_py as _admit   # noqa: E402  (the machine-wide compile queue: tools\admit\admit_py.py)

SCAN_VERSION = 'lnk3-6'
T_PP = 300
KIND = 'cvar.pp'
STUB_ENV = 'HBK_CVAR_STUB_FLAGS'   # check_selftest.py only: a scratch tree's flags (-I<tree>/src -I<tree>/sdk, the mode define); never set by a real run

# the first argument is the name (GET / SET / DEFINE_*) or the type (DECLARE: `std::string`, `int32_t`, never a comma), the second the name of a DECLARE (as check.py CVAR_USE)
USE = re.compile(rb'\bREXCVAR_(GET|SET|DEFINE_[A-Z0-9_]+|DECLARE)\s*\(\s*([^,()]+?)\s*(?:,\s*([A-Za-z_]\w*))?\s*[,)]')
STORAGE = re.compile(rb'\bFLAGS_([A-Za-z_]\w*?)_storage_\s*\(\s*\)\s*(\{)?')
MARKER = re.compile(rb'^# \d+ "((?:[^"\\]|\\.)*)"', re.M)
DEF_LINE = re.compile(rb'^\s*#\s*define\s+(\w+)(\([^)]*\))?(.*)$')
STRING = re.compile(rb'"(?:[^"\\\n]|\\.)*"')


def _clang():
    for c in (r'C:\Program Files\LLVM\bin\clang++.exe',):
        if os.path.isfile(c):
            return c
    return None


CLANG = _clang()


def _clang_id():
    try:
        st = os.stat(CLANG)
        return '%d:%d' % (st.st_size, st.st_mtime_ns)
    except (OSError, TypeError):
        return '?'


def _line_at(out, pos):
    s = out.rfind(b'\n', 0, pos) + 1
    e = out.find(b'\n', pos)
    return s, (len(out) if e < 0 else e)


def _uses(text, params=()):
    """[(kind, name or ('param', index))] of the cvar uses in one line / macro body: DEFINE / DECLARE / GET / SET, FLAGS_<name>_storage_() { (INLINE) or a call (GET),
    and in a macro body FLAGS_##p##_storage_ through a parameter."""
    out = []
    pidx = {p: i for i, p in enumerate(params)}

    def nm(tok):
        if tok is None:
            return None
        tok = tok.decode()
        return ('param', pidx[tok]) if tok in pidx else tok
    for u in USE.finditer(text):
        kind = u.group(1).decode()
        name = nm(u.group(3) if kind == 'DECLARE' else u.group(2))
        if name is not None:
            out.append(('DEFINE' if kind.startswith('DEFINE') else kind, name))
    for st in re.finditer(rb'\bFLAGS_(?:##\s*(\w+)\s*##|([A-Za-z_]\w*?))_storage_\s*\(\s*\)\s*(\{)?', text):
        name = nm(st.group(1) or st.group(2))
        if name is None or (st.group(1) and not isinstance(name, tuple)):
            continue
        out.append(('INLINE' if st.group(3) else 'GET', name))
    return out


def _args(text, start):
    """The top-level arguments of a macro call whose '(' is at text[start]; None when unbalanced on this line."""
    depth, cur, args = 0, b'', []
    for i in range(start, len(text)):
        c = text[i:i + 1]
        if c == b'(':
            depth += 1
            if depth == 1:
                continue
        elif c == b')':
            depth -= 1
            if depth == 0:
                args.append(cur.strip())
                return args
        elif c == b',' and depth == 1:
            args.append(cur.strip())
            cur = b''
            continue
        cur += c
    return None


def scan(out, ours):
    """{'def': {name: n}, 'inline', 'decl', 'read', 'ext': names declared / defined by a header outside our src (the SDK's own cvars), 'deps': paths} of one preprocessed text.
    `ours(path)` says whether a line-marker path is our src."""
    marks = [(m.start(), m.group(1).decode('utf-8', 'replace').replace('\\\\', '\\')) for m in MARKER.finditer(out)]
    starts = [p for p, _f in marks]
    import bisect

    def file_at(pos):
        i = bisect.bisect_right(starts, pos) - 1
        return marks[i][1] if i >= 0 else ''
    res = {'def': {}, 'inline': set(), 'decl': set(), 'read': set(), 'ext': set()}

    def note(kind, name, pos):
        if not isinstance(name, str) or not re.match(r'^[A-Za-z_]\w*$', name):
            return
        if kind in ('DECLARE', 'DEFINE', 'INLINE') and not ours(file_at(pos)):
            res['ext'].add(name)
        if kind in ('GET', 'SET'):
            res['read'].add(name)
        elif kind == 'DECLARE':
            res['decl'].add(name)
        elif kind == 'INLINE':
            res['inline'].add(name)
        else:
            res['def'][name] = res['def'].get(name, 0) + 1
    wrappers = []   # (macro name, its uses, offset of its #define, offset of its #undef or end)
    pos = 0
    for m in re.finditer(rb'REXCVAR_|FLAGS_', out):
        if m.start() < pos:
            continue
        s, e = _line_at(out, m.start())
        pos = e
        line = out[s:e]
        if line.lstrip().startswith(b'#'):
            d = DEF_LINE.match(line)
            if d and not d.group(1).startswith(b'REXCVAR_'):
                params = [p.strip() for p in (d.group(2) or b'()')[1:-1].split(b',') if p.strip()]
                body_uses = _uses(STRING.sub(b'""', d.group(3)), [p.decode() for p in params])
                if body_uses:
                    u = re.search(rb'(?m)^\s*#\s*undef\s+' + re.escape(d.group(1)) + rb'\b', out[e:])
                    wrappers.append((d.group(1), bool(d.group(2)), body_uses, e, e + u.start() if u else len(out)))
            continue
        for kind, name in _uses(STRING.sub(b'""', line)):
            note(kind, name, s)
    for w, fnlike, body_uses, a, b in wrappers:
        i = a
        while True:
            i = out.find(w, i, b)
            if i < 0:
                break
            j = i + len(w)
            if (i and (out[i - 1:i].isalnum() or out[i - 1:i] == b'_')) or (j < len(out) and (out[j:j + 1].isalnum() or out[j:j + 1] == b'_')):
                i = j
                continue
            s, e = _line_at(out, i)
            if out[s:e].lstrip().startswith(b'#'):
                i = e
                continue
            args = None
            if fnlike:
                k = j
                while k < e and out[k:k + 1] in (b' ', b'\t'):
                    k += 1
                if out[k:k + 1] != b'(':
                    i = j
                    continue
                args = _args(out[:e], k)
            for kind, name in body_uses:
                if isinstance(name, tuple):
                    name = args[name[1]].decode('utf-8', 'replace') if args and name[1] < len(args) else None
                note(kind, name, s)
            i = e
    deps = sorted({f for _p, f in marks if not f.startswith('<')})
    return {'def': res['def'], 'inline': sorted(res['inline']), 'decl': sorted(res['decl']), 'read': sorted(res['read']), 'ext': sorted(res['ext']), 'deps': deps}


class Flags:
    """The command line of each (unit, mode). Real: the main checkout's configured builds (game_syntax). Stub (STUB_ENV, check_selftest.py only): a scratch tree."""

    def __init__(self, tree):
        self.stub = bool(os.environ.get(STUB_ENV))
        self.tree = os.path.abspath(tree).replace('\\', '/')
        self.main = None
        self.lines = {}
        self.main_cmake = ''
        if not self.stub:
            import game_syntax as gs   # noqa: E402
            self.gs = gs
            self.main = gs.main_checkout(tree)
            if not self.main:
                raise RuntimeError('not a git tree: no main checkout for the configured builds')
            self.main = self.main.replace('\\', '/')
            for mode, b in ((1, 'on'), (0, 'off')):
                self.lines[mode] = gs.build_commands(self.main, gs.BUILD_DIRS[b])   # RuntimeError: no ninja / no build.ninja (the caller: BLOCKED)
            try:
                self.main_cmake = open(os.path.join(self.main, 'CMakeLists.txt'), encoding='utf-8', errors='replace').read().replace('\\', '/')
            except OSError:
                self.main_cmake = ''

    def cache_dir(self):
        if self.stub:   # outside the scratch repo (an untracked out\ there would join the selftest's diffs)
            return os.path.join(tempfile.gettempdir(), 'cvar_pp_stub', hashlib.sha256(self.tree.lower().encode()).hexdigest()[:16])
        return os.path.join(self.main, 'out', 'cvar_pp')

    def roots(self):
        r = [(self.tree, '<ROOT>')]
        if self.main and self.main.lower() != self.tree.lower():
            r.append((self.main, '<MAIN>'))
        return r

    def args(self, unit, mode):
        """(args, None) or (None, why it is not judged). Mode 2 = an OFF-only unit as an ON tool build compiles it (start_settings.cpp's ON flags)."""
        if self.stub:
            return ['-x', 'c++', '-std=c++20', '-I' + self.tree + '/src', '-I' + self.tree + '/sdk', '-DWWE2K14_KEEP_STREAM=%d' % (1 if mode else 0)], None
        gs = self.gs
        bmode = 1 if mode else 0
        try:
            if mode == 2:
                raise RuntimeError('tool build')
            fl = gs.flags_for(self.lines[bmode], self.main, unit)
        except RuntimeError:
            if mode != 2 and re.search(r'(?<![\w/.-])' + re.escape(unit) + r'(?![\w.])', self.main_cmake):
                return None, 'the configured %s build does not compile it (a platform branch of CMakeLists.txt)' % ('ON' if mode else 'OFF')
            fl = gs.flags_for(self.lines[bmode], self.main, 'src/start_settings.cpp')   # new in this tree / a tool build: start_settings.cpp's flags (all units but main.cpp / build_stamp.cpp share them)
        want = '-DWWE2K14_KEEP_STREAM=%d' % bmode
        if want not in fl:
            return None, 'the configured %s build\'s flags for it carry no %s (the wrong build was read)' % ('ON' if mode else 'OFF', want)
        return gs.with_pch(self.tree, unit, gs.tree_args(self.tree, self.main, fl)), None


def norm(path, roots):
    p = path.replace('\\', '/')
    for r, ph in sorted(roots, key=lambda x: -len(x[0])):
        if p.lower().startswith(r.lower() + '/'):
            return ph + p[len(r):]
    return p


def norm_args(args, roots):
    """The command line with every tree path as its placeholder (longest root first: a worktree lives inside the main checkout), so two trees with the same text share keys."""
    out = []
    for a in args:
        for r, ph in sorted(roots, key=lambda x: -len(x[0])):
            for v in (r, r.replace('/', '\\')):
                a = re.sub(re.escape(v), ph.replace('\\', '\\\\'), a, flags=re.I)
        out.append(a)
    return out


def denorm(path, roots):
    for r, ph in roots:
        if path.startswith(ph + '/'):
            return r + path[len(ph):]
    return path


class Stamps:
    def __init__(self, roots, overlay):
        self.roots, self.overlay, self.memo = roots, overlay or {}, {}

    def __call__(self, npath):
        if npath in self.memo:
            return self.memo[npath]
        real = denorm(npath, self.roots)
        key = os.path.normcase(os.path.abspath(real))
        if key in self.overlay:
            v = 'h:' + hashlib.sha256(self.overlay[key].encode()).hexdigest()
        else:
            try:
                if npath.startswith('<'):
                    with open(real, 'rb') as f:
                        v = 'h:' + hashlib.sha256(f.read()).hexdigest()
                else:
                    st = os.stat(real)
                    v = 's:%d:%d' % (st.st_size, st.st_mtime_ns)
            except OSError:
                v = 'missing'
        self.memo[npath] = v
        return v


def _overlay_yaml(overlay, tmp):
    """A clang VFS overlay (-ivfsoverlay) that serves each planted text in place of its path (a planted twin; the path may not exist)."""
    roots = []
    for i, (path, text) in enumerate(sorted(overlay.items())):
        f = os.path.join(tmp, 'planted_%d.h' % i)
        open(f, 'w', encoding='utf-8', newline='\n').write(text)
        roots.append({'name': path.replace('\\', '/'), 'type': 'file', 'external-contents': f.replace('\\', '/')})
    y = os.path.join(tmp, 'overlay.yaml')
    # use-external-names false: the planted file IS the virtual path (its quoted includes resolve from that folder; the line markers name it, so it is never cached)
    json.dump({'version': 0, 'case-sensitive': 'false', 'use-external-names': 'false', 'roots': roots}, open(y, 'w'))
    return y


def preprocess(unit_path, args, overlay=None):
    """(rc, stdout bytes, stderr text) of clang -E -fdirectives-only."""
    tmp = None
    try:
        extra = []
        if overlay:
            tmp = tempfile.mkdtemp(prefix='cvar_pp_')
            extra = ['-ivfsoverlay', _overlay_yaml(overlay, tmp)]
        r = subprocess.run([CLANG] + args + extra + ['-E', '-fdirectives-only', '-Wno-everything', unit_path], capture_output=True, timeout=T_PP)
        return r.returncode, r.stdout, r.stderr.decode('utf-8', 'replace')
    except subprocess.TimeoutExpired:
        return 124, b'', 'error: cvar_pp: clang -E timed out after %d s' % T_PP
    finally:
        if tmp:
            import shutil
            shutil.rmtree(tmp, ignore_errors=True)


def _one(job):
    """One (unit, mode): preprocess and scan, in a worker process. -> (unit, mode, key, scan result or None, first error line or None, seconds)."""
    tree, u, mode, args, key, overlay, roots = job
    t = time.time()
    rc, text, err = preprocess((tree.rstrip('/\\') + '/' + u).replace('\\', '/'), args, overlay)   # one slash style: the VFS overlay matches the main file by its exact spelling
    if rc:
        first = next((ln for ln in err.splitlines() if 'error' in ln), err.strip().splitlines()[0] if err.strip() else 'exit %d' % rc)
        return u, mode, key, None, first[:300], time.time() - t

    def ours(path):
        """True for our src (the tree's or the main checkout's) and for any path this cannot place (relative): only an absolute path outside src\\ (the SDK install under
        thirdparty\\, a system header, generated\\) is the SDK's, so a path that is not understood never hides a missing definition."""
        p = norm(path, roots)
        if p.startswith('<ROOT>/src/') or p.startswith('<MAIN>/src/'):
            return True
        if p.startswith('<'):
            return False   # a tree's thirdparty\ (the SDK install) or generated\
        return not os.path.isabs(p)
    return u, mode, key, scan(text, ours), None, time.time() - t


def _run_worker(job):
    """Run _one(job) in a child `py -I cvar_pp.py --worker`; a worker that dies is the unit's BLOCKED line, never a clean result."""
    tree, u, mode, args, key, overlay, roots = job
    t = time.time()
    try:
        r = subprocess.run([sys.executable, '-I', os.path.abspath(__file__), '--worker'], input=json.dumps(job).encode(), capture_output=True, timeout=T_PP + 60)
        if r.returncode == 0:
            res = json.loads(r.stdout.decode('utf-8'))
            return tuple(res)
        err = (r.stderr.decode('utf-8', 'replace').strip().splitlines() or ['exit %d' % r.returncode])[-1]
    except (subprocess.TimeoutExpired, ValueError, OSError) as e:
        err = '%s: %s' % (type(e).__name__, e)
    return u, mode, key, None, 'error: cvar_pp worker failed: ' + err[:250], time.time() - t


def _worker_main():
    job = json.loads(sys.stdin.buffer.read().decode('utf-8'))
    job[6] = [tuple(x) for x in job[6]]
    sys.stdout.write(json.dumps(list(_one(tuple(job)))))
    return 0


def game_units(tree):
    """{mode: sorted units} the tree's lists give the game target (cmake_sources + the keep_stream lists)."""
    sys.path.insert(0, HERE)
    import check as hb   # noqa: E402  (cmake_sources)

    def rd(rel):
        try:
            return open(os.path.join(tree, rel), encoding='utf-8', errors='replace').read()
        except OSError:
            return ''
    cm = hb.cmake_sources(rd('CMakeLists.txt'))
    off_only = {x.strip() for x in rd('tools/scene/keep_stream_off_sources.txt').splitlines() if x.strip().startswith('src/')}
    on_only = {x.strip() for x in rd('tools/scene/keep_stream_sources.txt').splitlines() if x.strip().startswith('src/')}
    have = lambda u: os.path.isfile(os.path.join(tree, u))   # noqa: E731
    on = sorted(u for u in cm - off_only if have(u))
    off = sorted(u for u in (cm - on_only) | off_only if have(u))
    # TOOL (mode 2): an OFF-only game unit a tool build compiles in ON (tools\**\build*.ps1 / .bat, a gate lane with --syntax; S103 SCS: host_start.cpp through lane s103_P7H1):
    # a read there that the unit does not declare is a compile error of that tool build. (Tool-only units, e.g. the viewers, are not judged.)
    texts = []
    for dp, _dn, fn in os.walk(os.path.join(tree, 'tools')):
        if os.path.basename(dp) == 'handback':
            continue
        for f in fn:
            low = f.lower()
            if (low.startswith('build') and low.endswith(('.ps1', '.bat'))) or low.endswith('.json'):
                t = rd(os.path.relpath(os.path.join(dp, f), tree))
                if not low.endswith('.json') or '--syntax' in t:
                    texts.append(t.replace('\\', '/'))
    tool = sorted(u for u in set(off) - set(on) if any(u[4:] in t for t in texts))
    return {1: on, 0: off, 2: tool}


def judge_tree(tree, overlay=None, stats=None):
    """{mode: {unit: scan result}}, blocked [(unit, mode, first error line)], not_judged [(unit, mode, why)]. Raises RuntimeError when no judgement is possible (no clang, no flags)."""
    if not CLANG:
        raise RuntimeError('clang++ not found (C:\\Program Files\\LLVM\\bin\\clang++.exe)')
    tree = os.path.abspath(tree)   # (a relative tree made the unit's own line marker relative: every name it declared looked like the SDK's)
    fl = Flags(tree)
    roots = fl.roots()
    overlay = {os.path.normcase(os.path.abspath(k)): v for k, v in (overlay or {}).items()}
    stamps = Stamps(roots, overlay)
    cdir = fl.cache_dir()
    os.makedirs(cdir, exist_ok=True)
    units = game_units(tree)
    jobs, blocked, not_judged = [], [], []
    for mode in (1, 0, 2):
        for u in units[mode]:
            args, why = fl.args(u, mode)
            if args is None:
                not_judged.append((u, mode, why))
                continue
            nargs = norm_args(args, roots)
            key = hashlib.sha256(('\0'.join([SCAN_VERSION, _clang_id(), u, str(mode)] + nargs)).encode()).hexdigest()[:32]
            jobs.append((u, mode, args, key))
    out = {1: {}, 0: {}, 2: {}}
    t0 = time.time()
    misses = []
    for u, mode, args, key in jobs:
        hit = None
        try:
            for ent in json.load(open(os.path.join(cdir, key + '.json'))):
                if all(stamps(p) == s for p, s in ent['deps']):
                    hit = ent['res']
                    break
        except (OSError, ValueError, KeyError, TypeError):
            pass
        if hit is not None:
            out[mode][u] = hit
        else:
            misses.append((u, mode, args, key))
    t_check = time.time() - t0

    t1 = time.time()
    times = []
    if misses:
        want = max(1, min(16, (os.cpu_count() or 4) // 2))
        jobs_in = [(tree, u, mode, args, key, overlay, roots) for u, mode, args, key in misses]
        # each job in its own worker PROCESS (`py -I cvar_pp.py --worker`, the job as JSON on stdin): the scan of a 60-90 MB text is Python work, and threads made a 1 s preprocess
        # take 6 s under the GIL; a multiprocessing pool re-imports the caller's main script on Windows. The threads here only wait on their child.
        with _admit.hold(KIND, want, timeout_s=120) as n, ThreadPoolExecutor(max_workers=max(1, min(int(n), len(jobs_in)))) as ex:
            results = list(ex.map(_run_worker, jobs_in))
        for u, mode, key, res, err, dt in results:
            times.append(dt)
            if res is None:
                blocked.append((u, mode, err))
                continue
            deps = [norm(d, roots) for d in res.pop('deps')]
            out[mode][u] = res
            planted = any(os.path.normcase(os.path.abspath(denorm(d, roots))) in overlay for d in deps) or any('cvar_pp_' in d and 'planted_' in d for d in deps)
            if planted:
                continue   # a planted twin's text is never cached
            ent = {'deps': [[d, stamps(d)] for d in deps], 'res': res}
            path = os.path.join(cdir, key + '.json')
            try:
                old = json.load(open(path))
            except (OSError, ValueError):
                old = []
            tmp = path + '.%d.tmp' % os.getpid()
            try:
                json.dump(([ent] + old)[:12], open(tmp, "w"))   # 12 variants: the head, its base and the re-find trees share keys
                os.replace(tmp, path)
            except OSError:
                pass
    if stats is not None:
        stats.update({'units': len(jobs), 'hits': len(jobs) - len(misses), 'misses': len(misses), 'check_s': round(t_check, 2), 'pp_s': round(time.time() - t1, 2),
                      'per_unit_s': round(sum(times) / len(times), 2) if times else 0.0, 'max_unit_s': round(max(times), 2) if times else 0.0})
    return out, blocked, not_judged


if __name__ == "__main__":
    if sys.argv[1:] == ["--worker"]:
        sys.exit(_worker_main())
    print(__doc__)
