r"""The one place a tool finds fview.exe (Session 101 lane 218).

Why: a stale exe measures OLD code. Twice on 2026-11-13 a tool picked the main checkout's out\renderer\dev\fview.exe (built 02:26, before lanes 170 / 206 changed
the shaders and the wrestler core): lane 208's carry darkening read 16-43 % (really 0-7 %), K1's roster_proof said INCOMPLETE (really 20 / 20 OURS). The replay compiles its
shaders from the exe's own headers, so an exe older than the code under test is wrong, not just slow.

  from fview_exe import find_fview, FviewError          (sys.path.insert(0, <tools\fview>) first; tools run with py -I)
  exe = find_fview(prefer=None, allow_stale=False, root=None, kind='dev')   -> str path; raises FviewError (message names the file and both times)
  exe = find_or_exit(...)   the same, but prints 'FAIL  <message>' and exits 1 (what the check tools use)

  prefer      an explicit exe path (a tool's --fview). Else the env WWE_FVIEW. Else the candidates below.
  root        the checkout whose sources are compared and whose out\ is searched first (default: the checkout this file is in).
  kind        'dev' | 'release' | None (None = the newest of both). A tool that needs the release build says so.
  candidates  <root>\out\renderer\<kind>\fview.exe; only when root has none, the MAIN checkout's (git common dir, or RDR_MAIN_CHECKOUT). The newest of the root's own wins.
              (A main-checkout exe is not preferred over the root's own: it can be newer than a worktree's exe and still lack the worktree's changes.)
  refusal     the exe is older than any file it compiles: the headers its build read (the .d files next to it, out\renderer\<kind>\fview_obj; without them every .h / .cpp /
              .hpp / .inl / .inc of src\fview, src\renderer_adapter, src\renderer_scene, src\renderer without player\), every .cpp of src\fview and src\renderer, and the files
              tools\fview\session_deps.txt names. (Gate lanes: tools\gate\exes.py already builds a stale exe first; its source set is wider, so a gate-fresh exe passes here.) Message: 'fview.exe built HH:MM is older than <file> (HH:MM): rebuild with tools\fview\build.ps1'.
  allow_stale / env WWE_FVIEW_ALLOW_STALE=1   run anyway (the exe and the warning are still printed).
  Prints '[fview_exe] <path> (built YYYY-MM-DD HH:MM)' once per exe per process (quiet=True hides it).
Selftest: py -I tools\fview\fview_exe.py --selftest      Where is it:  py -I tools\fview\fview_exe.py [--root DIR] [--kind dev|release|any] [--prefer EXE] [--allow-stale]
"""
import datetime, os, re, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[2]
SRC_DIRS = ('src/fview', 'src/renderer_adapter', 'src/renderer_scene', 'src/renderer')
SKIP_DIRS = ('src/renderer/player',)
EXT = ('.h', '.cpp', '.hpp', '.inl', '.inc')
_scan_cache = {}
_printed = set()


class FviewError(Exception):
    pass


def _hm(t):
    return datetime.datetime.fromtimestamp(t).strftime('%H:%M')


def main_checkout(root=None):
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return Path(env)
    root = Path(root or HERE)
    try:
        common = subprocess.run(['git', '-C', str(root), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return Path(common).parent
    except Exception:
        pass
    return root


def _dep_paths(exe):
    """The src\\... files the build of exe read, relative to its checkout: every .d file of <exe dir>\\fview_obj (clang -MMD). None when there are none."""
    objdir = Path(exe).parent / 'fview_obj'
    try:
        names = [n for n in os.listdir(objdir) if n.endswith('.d')]
    except OSError:
        return None
    if not names:
        return None
    base = Path(exe).resolve().parents[3]
    rels = set()
    for n in names:
        try:
            text = (objdir / n).read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        text = re.sub(r'\\\r?\n', ' ', text)
        text = text[text.find(': ') + 2:] if ': ' in text else text
        for item in re.split(r'(?<!\\)\s+', text):
            item = item.replace('\\ ', ' ').strip()
            if not item:
                continue
            try:
                rel = Path(item).resolve().relative_to(base)
            except (ValueError, OSError):
                continue
            if rel.parts and rel.parts[0] == 'src':
                rels.add(rel)
    return rels or None


def newest_source(root, exe=None):
    """(mtime, path) of the newest file fview compiles under root; cached per process.
    With the exe's own depfiles (out\\renderer\\<kind>\\fview_obj\\*.d, written by build.ps1) a header counts only when the build read it (a touched header nobody includes cannot make
    the exe stale: build.ps1 would not relink); every .cpp of src\\fview, src\\renderer (not player\\) and the session_deps list counts always (a NEW source is in no depfile yet).
    Without depfiles: every .h / .cpp / .hpp / .inl / .inc under SRC_DIRS."""
    root = Path(root).resolve()
    deps = _dep_paths(exe) if exe else None
    key = (root, str(exe) if deps else None)
    if key in _scan_cache:
        return _scan_cache[key]
    skip = [os.path.normcase(str(root / s)) for s in SKIP_DIRS]
    best = [0.0, None]

    def consider(p):
        try:
            m = os.stat(p).st_mtime
        except OSError:
            return
        if m > best[0]:
            best[0], best[1] = m, str(p)

    for d in SRC_DIRS:
        build_cpp_dir = d in ('src/fview', 'src/renderer')
        for dp, dn, fn in os.walk(root / d):
            if os.path.normcase(dp) in skip:
                dn[:] = []
                continue
            for f in fn:
                if f.endswith(EXT) if deps is None else (build_cpp_dir and f.endswith('.cpp')):
                    consider(os.path.join(dp, f))
    if deps:
        for rel in deps:
            if not any(os.path.normcase(str(root / rel)).startswith(s + os.sep) for s in skip):
                consider(root / rel)
    sdeps = root / 'tools' / 'fview' / 'session_deps.txt'
    try:
        lines = [l.strip() for l in sdeps.read_text(encoding='utf-8', errors='replace').splitlines() if l.strip() and not l.startswith('#')]
    except OSError:
        lines = []
    consider(sdeps)
    for rel in lines:
        consider(root / rel.replace('\\', os.sep))
    _scan_cache[key] = (best[0], best[1])
    return _scan_cache[key]


def _candidates(root, kind):
    kinds = ('dev', 'release') if kind is None else (kind,)
    own = [Path(root) / 'out' / 'renderer' / k / 'fview.exe' for k in kinds]
    own = [c for c in own if c.exists()]
    if own:
        return own
    main = main_checkout(root)
    if os.path.normcase(str(main.resolve())) == os.path.normcase(str(Path(root).resolve())):
        return []
    return [c for c in (main / 'out' / 'renderer' / k / 'fview.exe' for k in kinds) if c.exists()]


def find_fview(prefer=None, allow_stale=False, root=None, kind='dev', quiet=False):
    root = Path(root or HERE)
    prefer = prefer or os.environ.get('WWE_FVIEW') or None
    allow_stale = allow_stale or os.environ.get('WWE_FVIEW_ALLOW_STALE', '') not in ('', '0')
    if prefer:
        exe = Path(prefer)
        if not exe.exists():
            raise FviewError(f'fview.exe not found: {exe}')
    else:
        cands = _candidates(root, kind)
        if not cands:
            raise FviewError(f'no fview.exe under {root}\\out\\renderer\\{kind or "dev|release"} (or the main checkout): build it with tools\\fview\\build.ps1' + (' -Release' if kind == 'release' else ''))
        exe = max(cands, key=lambda c: c.stat().st_mtime)
    built = exe.stat().st_mtime
    t, who = newest_source(root, exe)
    stale = who is not None and t > built
    if stale:
        msg = f'fview.exe built {_hm(built)} ({exe}) is older than {who} ({_hm(t)}): rebuild with tools\\fview\\build.ps1' + (' -Release' if exe.parent.name == 'release' else '')
        if not allow_stale:
            raise FviewError(msg + ' (or pass --allow-stale / set WWE_FVIEW_ALLOW_STALE=1)')
        print('[fview_exe] WARNING (allowed): ' + msg, flush=True)
    key = str(exe)
    if not quiet and key not in _printed:
        _printed.add(key)
        print(f'[fview_exe] {exe} (built {datetime.datetime.fromtimestamp(built):%Y-%m-%d %H:%M})', flush=True)
    return str(exe)


def find_or_exit(prefer=None, allow_stale=False, root=None, kind='dev'):
    """find_fview for a tool with no way to carry the error: prints 'FAIL  <message>' and exits 1."""
    try:
        return find_fview(prefer=prefer, allow_stale=allow_stale, root=root, kind=kind)
    except FviewError as e:
        print('FAIL  ' + str(e), flush=True)
        sys.exit(1)


def add_allow_stale(ap):
    ap.add_argument('--allow-stale', action='store_true', help='run although fview.exe is older than the renderer / fview sources (tools\\fview\\fview_exe.py)')


def _selftest():
    import tempfile
    bad = []

    def check(name, ok, detail=''):
        print(('PASS  ' if ok else 'FAIL  ') + name + (('  ' + detail) if detail and not ok else ''))
        if not ok:
            bad.append(name)

    def refuses(**kw):
        try:
            find_fview(quiet=True, **kw)
            return None
        except FviewError as e:
            return str(e)

    def mk(path, t, data=b'x'):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        os.utime(path, (t, t))
        return path

    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        os.environ['RDR_MAIN_CHECKOUT'] = str(td / 'main')
        os.environ.pop('WWE_FVIEW', None)
        os.environ.pop('WWE_FVIEW_ALLOW_STALE', None)
        T = 1_800_000_000
        for r in ('wt', 'main'):
            mk(td / r / 'src' / 'renderer' / 'a.h', T)
            mk(td / r / 'src' / 'renderer' / 'player' / 'win.cpp', T + 5000)   # skipped
            mk(td / r / 'src' / 'fview' / 'x.cpp', T)
            mk(td / r / 'src' / 'renderer_adapter' / 'b.h', T)
            mk(td / r / 'src' / 'renderer_scene' / 'c.cpp', T)
            mk(td / r / 'tools' / 'fview' / 'session_deps.txt', T, b'# c\nsrc\\assets\\d.cpp\n')
            mk(td / r / 'src' / 'assets' / 'd.cpp', T)
        wt, main = td / 'wt', td / 'main'
        _scan_cache.clear()
        mk(wt / 'out' / 'renderer' / 'dev' / 'fview.exe', T + 100)
        mk(main / 'out' / 'renderer' / 'dev' / 'fview.exe', T + 900)
        check('fresh own exe is found, player/ ignored', refuses(root=wt) is None)
        check('own exe preferred over a newer main exe', Path(find_fview(root=wt, quiet=True)).parts[-5] == 'wt')
        _scan_cache.clear()
        hdr = mk(wt / 'src' / 'renderer' / 'shaders' / 'fx_new.h', T + 200)
        m = refuses(root=wt)
        check('exe older than a shader header is refused', bool(m) and 'older than' in m and 'fx_new.h' in m and 'tools\\fview\\build.ps1' in m, str(m))
        check('message carries both times', bool(m) and f'built {_hm(T + 100)}' in m and f'({_hm(T + 200)})' in m, str(m))
        check('allow_stale runs', refuses(root=wt, allow_stale=True) is None)
        os.environ['WWE_FVIEW_ALLOW_STALE'] = '1'
        check('env WWE_FVIEW_ALLOW_STALE runs', refuses(root=wt) is None)
        os.environ.pop('WWE_FVIEW_ALLOW_STALE')
        os.remove(hdr)
        for sub, f in (('src/fview', 'n.cpp'), ('src/renderer_adapter', 'n.h'), ('src/renderer_scene', 'n.h')):
            _scan_cache.clear()
            p = mk(wt / sub / f, T + 300)
            check(f'{sub} source is watched', refuses(root=wt) is not None)
            os.remove(p)
        _scan_cache.clear()
        mk(wt / 'src' / 'assets' / 'd.cpp', T + 400)
        check('session_deps source is watched', refuses(root=wt) is not None)
        mk(wt / 'src' / 'assets' / 'd.cpp', T)
        _scan_cache.clear()
        check('fresh again after the source goes back', refuses(root=wt) is None)
        # no own exe -> main's, checked against the root's sources
        os.remove(wt / 'out' / 'renderer' / 'dev' / 'fview.exe')
        _scan_cache.clear()
        check('no own exe falls to the main checkout', Path(find_fview(root=wt, quiet=True)).parts[-5] == 'main')
        mk(wt / 'src' / 'renderer' / 'a.h', T + 2000)
        _scan_cache.clear()
        m = refuses(root=wt)
        check('a stale main exe is refused against the worktree sources (the 02:26 case)', bool(m) and 'a.h' in m, str(m))
        e = mk(td / 'other' / 'fview.exe', T + 5000)
        check('explicit prefer wins', find_fview(prefer=str(e), root=wt, quiet=True) == str(e))
        os.environ['WWE_FVIEW'] = str(e)
        check('env WWE_FVIEW is read', find_fview(root=wt, quiet=True) == str(e))
        os.environ.pop('WWE_FVIEW')
        check('missing explicit path is an error', refuses(prefer=str(td / 'nope.exe'), root=wt) is not None)
        _scan_cache.clear()
        mk(wt / 'out' / 'renderer' / 'release' / 'fview.exe', T + 6000)
        check('kind=release picks release', 'release' in find_fview(root=wt, kind='release', quiet=True))
        check('kind=None picks the newest of dev / release', 'release' in find_fview(root=wt, kind=None, quiet=True))
        # depfiles: only the headers the build read count
        os.remove(wt / 'out' / 'renderer' / 'release' / 'fview.exe')
        mk(wt / 'src' / 'renderer' / 'a.h', T)
        mk(wt / 'out' / 'renderer' / 'dev' / 'fview.exe', T + 100)
        mk(wt / 'out' / 'renderer' / 'dev' / 'fview_obj' / 'x.o.d', T + 100,
           ('x.o: %s \\\n  %s\n' % (wt / 'src' / 'fview' / 'x.cpp', wt / 'src' / 'renderer' / 'a.h')).encode())
        mk(wt / 'src' / 'renderer_scene' / 'unused.h', T + 500)
        _scan_cache.clear()
        check('depfiles: a touched header the build never read does not make the exe stale', refuses(root=wt) is None, str(refuses(root=wt)))
        mk(wt / 'src' / 'renderer' / 'a.h', T + 600)
        _scan_cache.clear()
        m = refuses(root=wt)
        check('depfiles: a header the build read does', bool(m) and 'a.h' in m, str(m))
        mk(wt / 'src' / 'renderer' / 'a.h', T)
        mk(wt / 'src' / 'renderer' / 'new_source.cpp', T + 700)
        _scan_cache.clear()
        check('depfiles: a NEW .cpp (in no depfile) does', refuses(root=wt) is not None)
        check('kind=dev never returns the release exe', 'release' not in (refuses(root=wt, kind='dev') or find_fview(root=wt, kind='dev', quiet=True)))
    print('selftest: %s' % ('FAILED ' + ', '.join(bad) if bad else 'all passed'))
    return 1 if bad else 0


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--root'); ap.add_argument('--kind', default='dev'); ap.add_argument('--prefer')
    add_allow_stale(ap)
    a = ap.parse_args()
    if a.selftest:
        sys.exit(_selftest())
    try:
        print(find_fview(prefer=a.prefer, allow_stale=a.allow_stale, root=a.root, kind=None if a.kind == 'any' else a.kind))
    except FviewError as e:
        print('FAIL  ' + str(e))
        sys.exit(1)
