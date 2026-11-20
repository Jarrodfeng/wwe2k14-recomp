r"""Dependency manifests and derived dependencies for the gate (Session 103, lane DIB; design docs\research\s103\DI_index_check_selection.md).

Part 1 (this file's first half): the per-build manifest the compiler itself writes. tools\cxx_cache.ps1 compiles with -MMD and, at every link, writes
`<exe>.deps.txt` (the exact sources and headers the exe's objects were compiled from) and `out\deps\<build script>.deps.txt` (the union for one script
run). The tool cache (toolcache.py) and the proof key (fastgate.py) key on the CONTENT of those files instead of "all of src\ + tools\".

Manifest format: first line `# wwe2k14-deps v1`; then one path per line: a repo-relative posix path, `thirdparty/...` (resolved under the checkout, else
under the main checkout), or `abs:<path>`. A first line `# wwe2k14-deps INVALID: <why>` means "the build could not name its inputs": every reader treats
it as NO manifest (the caller falls back to the old whole-tree key / refuses to cache).

Why a key from a manifest is sound (it is read BEFORE the build it keys, so it comes from an earlier build): the key holds the content of every file
of that earlier closure, plus the set of directory names under src\ / tools\ and the file names inside the directories the closure lives in. A change
to what the build compiles is one of: an edit to a file of the old closure (key changes); a new `#include` (an edit to a file of the old closure, same);
a new file that shadows or is picked up by a source-list directory walk (a name appears: key changes); a changed build script / flag (the script and
its helpers are in the closure). A header that only a different flag set reaches is the open case (documented in the lane's handback).
"""
import hashlib, os, re, sys, threading, time
from pathlib import Path

HEAD = '# wwe2k14-deps v1'
INVALID = '# wwe2k14-deps INVALID'
CPP_EXT = ('.cpp', '.cc', '.c', '.h', '.hpp', '.inl', '.vert', '.frag', '.comp', '.glsl', '.hlsl', '.fx.json')   # (shader sources too: a generator build reads them)
SKIP_DIRS = {'__pycache__', '.git', 'out', 'lanes'}
_MU = threading.Lock()
_FH = {}
_DIRS = {}


def flag_tag(flags):
    """The build arguments of one flavour of a build script (-Release, -Surface ...) as the tag its manifest name carries: the lower-case names of the arguments
    that start with '-', sorted, joined by '+' (tools/cxx_cache.ps1 builds the same tag from the caller's bound parameters). '' for no flags."""
    names = sorted({str(f).split(':')[0].lstrip('-').lower() for f in (flags or []) if str(f).startswith('-') and len(str(f)) > 1})
    return '+'.join(names)


def script_id(script_rel, flags=None):
    """The name `out/deps/<id>.deps.txt` uses for a build script (cxx_cache.ps1 writes the same: the repo-relative path with slashes as _). S103 DIB review
    bug 5: the build arguments are part of the id (`@release`, `@release+surface`): a dev and a -Release build of one script are different builds with different
    inputs and no longer overwrite one manifest."""
    base = re.sub(r'[\\/]', '_', str(script_rel).replace('\\', '/').lstrip('./'))
    tag = flag_tag(flags)
    return base + ('@' + tag if tag else '')


def _sha1(p):
    """sha1 of a file's bytes (memoised by path, mtime, size): the hash a manifest records per entry. 'gone' for no file."""
    try:
        st = os.stat(p)
    except OSError:
        return 'gone'
    k = ('sha1', str(p), st.st_mtime_ns, st.st_size)
    with _MU:
        h = _FH.get(k)
    if h is None:
        h = hashlib.sha1(Path(p).read_bytes()).hexdigest()
        with _MU:
            _FH[k] = h
    return h


def read_manifest_full(path):
    """-> {entry: recorded sha1 or None}, or None (missing, unreadable, INVALID, wrong header). A line is `entry` or `entry<TAB>sha1` (the build records the
    content hash of every file it compiled from)."""
    try:
        text = Path(path).read_text(encoding='utf-8')
    except OSError:
        return None
    lines = text.splitlines()
    if not lines or lines[0].strip() != HEAD:
        return None
    out = {}
    for l in lines[1:]:
        if not l.strip() or l.startswith('#'):
            continue
        e, _t, h = l.partition('\t')
        out[e.strip()] = h.strip() or None
    return out or None


def read_manifest(path, root=None, main=None):
    """-> sorted list of entries, or None (missing, unreadable, INVALID, wrong header). With root: also None when the manifest does not describe this tree
    (S103 DIB review bug 5): an entry whose recorded content hash differs from the file now in the checkout is a build of ANOTHER tree state (a manifest from main
    or the shared store); a manifest without hashes (an old one) is accepted."""
    full = read_manifest_full(path)
    if full is None:
        return None
    if root is not None and mismatch(full, root, main):
        return None
    return sorted(full)


def mismatch(full, root, main=None):
    """The first entry whose recorded hash differs from the file in this checkout, or None. abs: entries and entries without a hash are not compared."""
    for e, h in full.items():
        if h and not e.startswith('abs:') and _sha1(resolve(e, root, main)) != h:
            return e
    return None


def invalid_reason(path):
    try:
        l = Path(path).read_text(encoding='utf-8').splitlines()
    except OSError:
        return None
    return l[0] if l and l[0].startswith(INVALID) else None


def write_manifest(path, entries, root=None, main=None):
    """Atomic write of a manifest (tmp + replace). entries: a list of entry names, or {entry: sha1}; with root the sha1 of each file is recorded.
    Not rewritten when the content is the same (S103 DIB S2)."""
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    items = dict(entries) if isinstance(entries, dict) else {e: None for e in set(entries)}
    if root is not None:
        for e in items:
            if items[e] is None and not e.startswith('abs:'):
                h = _sha1(resolve(e, root, main))
                items[e] = None if h == 'gone' else h
    text = HEAD + '\n' + ''.join(e + ('\t' + items[e] if items[e] else '') + '\n' for e in sorted(items))
    try:
        if p.read_text(encoding='utf-8') == text:
            return
    except OSError:
        pass
    tmp = p.with_name(p.name + f'.{os.getpid()}.tmp')
    tmp.write_text(text, encoding='utf-8')
    os.replace(tmp, p)


def union(*lists):
    s = set()
    for l in lists:
        if l:
            s.update(l)
    return sorted(s)


def resolve(entry, root, main=None):
    """An entry -> an absolute Path."""
    root = Path(root)
    if entry.startswith('abs:'):
        return Path(entry[4:])
    if entry.startswith('thirdparty/'):
        c = root / entry
        if not c.exists() and main is not None and (Path(main) / entry).exists():
            return Path(main) / entry
        return c
    return root / entry


def _hash_file(p):
    try:
        st = os.stat(p)
    except OSError:
        return 'gone'
    k = (str(p), st.st_mtime_ns, st.st_size)
    with _MU:
        h = _FH.get(k)
    if h is None:
        d = hashlib.blake2b(digest_size=16)
        with open(p, 'rb') as f:
            for blk in iter(lambda: f.read(1 << 16), b''):
                d.update(blk)
        h = d.hexdigest()
        with _MU:
            _FH[k] = h
    return h


def _tree_dirs(root):
    """{relative dir (posix): sorted C++-family file names in it} for src\\ and tools\\ (memoised per root and process)."""
    k = str(root)
    with _MU:
        if k in _DIRS:
            return _DIRS[k]
    res = {}
    for t in ('src', 'tools'):
        for dp, dn, fn in os.walk(Path(root) / t):
            dn[:] = sorted(d for d in dn if d not in SKIP_DIRS)
            rel = os.path.relpath(dp, root).replace('\\', '/')
            res[rel] = sorted(f for f in fn if f.lower().endswith(CPP_EXT))
    with _MU:
        _DIRS[k] = res
    return res


def forget(root=None):
    """Drop the memoised directory listing (a test that edits a tree, or a gate that built something)."""
    with _MU:
        if root is None:
            _DIRS.clear()
        else:
            _DIRS.pop(str(root), None)


def digest(entries, root, main=None):
    """sha256 over (entry, content hash) of every manifest file, the set of directories under src\\ and tools\\, and the C++-family file NAMES of every
    directory a manifest file lives in (a new file that shadows an include, or that a source-list directory walk would pick up, changes it)."""
    root = Path(root)
    d = hashlib.sha256()
    dirs_used = set()
    for e in sorted(entries):
        p = resolve(e, root, main)
        d.update(e.encode() + b'\0' + _hash_file(p).encode() + b'\n')
        if not e.startswith(('abs:', 'thirdparty/')):
            dirs_used.add(os.path.dirname(e))
    tree = _tree_dirs(root)
    d.update(('DIRS\0' + '\n'.join(sorted(tree))).encode())
    for dd in sorted(dirs_used):
        d.update(('NAMES\0' + dd + '\0' + '\0'.join(tree.get(dd, ['<unlisted>']))).encode() + b'\n')
    return d.hexdigest()


def script_manifest(root, main, script_rel, store=None, flags=None):
    """The manifest of the latest build of a build script: this checkout's out\\deps, else the main checkout's, else the tool cache's store. -> entries or None.
    flags=None: the union of every flavour of the script (dev, -Release, ...: the caller cannot say which one a python check runs); flags=[...]: that flavour.
    A manifest whose recorded hashes do not match THIS checkout's files is a build of another tree state and is skipped (S103 DIB review bug 5)."""
    base_id = script_id(script_rel)
    for base in (Path(root) / 'out' / 'deps', Path(main) / 'out' / 'deps' if main else None, Path(store) if store else None):
        if base is None:
            continue
        if flags is not None:
            names = [script_id(script_rel, flags) + '.deps.txt']
        else:
            try:
                names = sorted(p.name for p in base.glob(glob_escape(base_id) + '*.deps.txt') if p.name == base_id + '.deps.txt' or p.name.startswith(base_id + '@'))
            except OSError:
                names = []
        if any(invalid_reason(base / n) for n in names):
            return None   # one flavour could not name its inputs: the union is not trustworthy
        ms = [read_manifest(base / n, root, main) for n in names]
        ms = [m for m in ms if m]
        if ms:
            return union(*ms)
    return None


def glob_escape(s):
    return re.sub(r'([\[\]*?])', r'[\1]', s)


# =====================================================================================================================================================
# Part 2: DERIVED dependencies per check (selection + narrow keys). Design: docs\research\s103\DI_index_check_selection.md sections 1, 4.
#
# derive(check) -> {tier, exact, files, dirs, idx_files, notes, closure}
#   tier A  compile-derived: the TUs / headers the check compiles or runs the exe of (a build's dependency manifest; game_syntax's units from the ours.db index)
#   tier B  script-derived: the script, its imports and every repo path its code names; the script enumerates nothing
#   tier C  SCAN: a script in the closure enumerates a tree (os.walk, glob, git ls-files, Get-ChildItem ...): its dependency is "a directory"; stays on `affects`
#   tier D  UNKNOWN: a .ps1 that is not a manifest build, an exe no manifest names, a missing / INVALID manifest: stays on `affects`
# exact = tier A or B with nothing partial. Only exact derived deps may let a check SKIP; any other tier never narrows what `affects` / always-run selects.
# Selection (fastgate.selected) = union(affects, derived). Stale index: a closure file (not in the diff) whose sha differs from the index's makes the check inexact.
# =====================================================================================================================================================
import json, sqlite3, time

SCAN_RE = re.compile(r"os\.walk|\.rglob\(|\.glob\(|glob\.glob|iterdir\(|os\.listdir|scandir\(|ls-files|Get-ChildItem|-Recurse|fnmatch|['\"]git['\"]|\bgit\s+(?:diff|ls-files|log|grep|status)|iglob|fwalk|\.walk\(|zipfile|tarfile|unpack_archive|importlib|runpy|__import__|\bexec\(|\beval\(|['\"](?:clang\+*|clang-cl|cmake|ninja|glslangValidator|glslang|ctest)(?:\.exe)?['\"]")
LIT_RE = re.compile(r"""['"]([\w./\\ -]+\.(?:h|hpp|cpp|inl|py|ps1|json|txt|glsl|hlsl|comp|vert|frag|toml|cmake|md|csv|def|rc|cc))['"]""")
UNRES_RE = re.compile(r"""['"]([\w./\\ -]+\.(?:bin|png|zip|rec|yaml|yml|inc|spv|dat|xml|jpg|jpeg|bmp|tga|dds|ini|cfg|csv|pdb|wav|ogg|mp4|sqlite|db|pak|xex|rsp|ninja|tsv))['"]""")
FSTR_RE = re.compile(r"""\bf['"][^'"\n]*\b(?:src|tools|probes|config|db|docs|assets|android|patches)[\\/][^'"\n]*\{""")
JOIN_RE = re.compile(r"""['"](src|tools|probes|config|db|docs|assets|android|patches)['"]((?:\s*,\s*['"][\w.-]+['"])*)""")
DIR_LIT = re.compile(r"""['"]((?:src|tools|probes|config|db|docs|assets|android|patches)(?:[\\/][\w.-]+)*)[\\/]?['"]""")
EXE_ARG = re.compile(r"""([\w./\\-]*[\w.+-]+\.exe)\b""")
HELPERS = ('tools/env.ps1', 'tools/cxx_cache.ps1', 'tools/cxx_cc.ps1', 'tools/wrestler/atomic_build.ps1')
EXE_DIRS = ('out/tools', 'out/renderer/dev', 'out/renderer/release', 'out/release/build')
WRITES_RE = re.compile(r"open\([^)]*['\"][wax]b?\+?['\"]|write_text|write_bytes|makedirs|mkdir|shutil\.(?:copy|copyfile|copytree|move|rmtree)|os\.(?:replace|remove|rename|unlink)"
                       r"|\.unlink\(|savefig|imwrite|\.save\(|Set-Content|Out-File|New-Item|Copy-Item|Move-Item|Add-Content|--save|json\.dump\(|sqlite3\.connect")
SYSTEM_EXES = {'py.exe', 'python.exe', 'pythonw.exe', 'pwsh.exe', 'powershell.exe', 'cmd.exe', 'git.exe', 'taskkill.exe', 'tasklist.exe', 'where.exe', 'robocopy.exe', 'xcopy.exe',
               'curl.exe', 'tar.exe', 'ssh.exe', 'scp.exe', 'adb.exe', 'ping.exe', 'sc.exe', 'reg.exe', 'wmic.exe', 'nvidia-smi.exe', 'notepad.exe', 'explorer.exe'}
DERIVE_VERSION = "dib3"


def _is_runtime_path(lit):
    """A literal that names something created or supplied at run time (an out / captures / game path, an absolute path, a template): not a repo file."""
    l = lit.strip().lstrip('./')
    return (l.startswith(('out/', 'game_data/', 'generated/', 'thirdparty/', '~', '/')) or bool(re.match(r'^[A-Za-z]:', l)) or any(c in l for c in '{}%*$')
            or lit.startswith(('-', '.')) and '/' not in lit)


def _code(text, kind):
    import sched as gs
    return gs._code_only(text)


def _sha1_file(p):
    import hashlib as _h
    try:
        return _h.sha1(Path(p).read_bytes()).hexdigest()
    except OSError:
        return None


class Index:
    """A read-only view of the `q ours` index (out\\harness\\index\\ours.db: TUs, include edges, file shas)."""

    def __init__(self, path, timeout=None):
        self.path = Path(path)
        self._db = None
        self.stamp = ''
        self._tu = {}
        self._sha = {}
        self.timeout = timeout   # QAF: a reader that must not wait on a writer (q ours affects) passes a short one; None = sqlite's default 5 s
        self._open()

    def _open(self):
        kw = {} if self.timeout is None else {'timeout': self.timeout}
        self._db = sqlite3.connect(f'file:{self.path.as_posix()}?mode=ro', uri=True, check_same_thread=False, **kw)
        self.stamp = f'{self.path.stat().st_mtime_ns}'   # informational only: no cache key uses it (DPC)
        self._tu, self._sha = {}, {}

    @property
    def db(self):
        """QTF round 3: the connection is opened again on demand after close() (a gate closes it once lane selection is done so it does not sit on the
        index for the whole run; the stamp and the row caches are re-read with it, so a swapped-in newer index is seen whole, never half and half)."""
        if self._db is None:
            self._open()
        return self._db

    def tus(self, rel):
        """[(id, errors)] of every parsed unit of this source file (it is built more than once: ON / OFF / tool)."""
        if rel not in self._tu:
            self._tu[rel] = self.db.execute('select id, errors from tus where file=?', (rel,)).fetchall()
        return self._tu[rel]

    def closure(self, rel):
        """-> (set of repo-relative files the unit(s) include, partial, found). partial = a unit parsed with errors (its include list may miss headers)."""
        rows = self.tus(rel)
        files, partial = {rel}, False
        for tid, err in rows:
            partial = partial or bool(err)
            files.update(r[0] for r in self.db.execute('select file from incs where tu=?', (tid,)))
        return files, partial, bool(rows)

    def close(self):
        db, self._db = self._db, None
        try:
            if db is not None:
                db.close()
        except sqlite3.Error:
            pass

    def sha(self, rel):
        if rel not in self._sha:
            r = self.db.execute('select sha from files where path=?', (rel,)).fetchone()
            self._sha[rel] = r[0] if r else None
        return self._sha[rel]


def open_index(root, main=None):
    for b in (Path(root), Path(main) if main else None):
        if b is None:
            continue
        p = b / 'out' / 'harness' / 'index' / 'ours.db'
        if p.is_file():
            try:
                return Index(p)
            except sqlite3.Error:
                continue
    return None


def _rel(root, p):
    try:
        return os.path.relpath(str(p), str(root)).replace('\\', '/')
    except ValueError:
        return str(p).replace('\\', '/')


def _exe_manifest(root, main, arg):
    """A built exe named in a command or a script -> its dependency manifest entries (or None). arg: a path or a bare file name (looked up in the tool dirs)."""
    a = str(arg).replace('\\', '/').strip('\'"')
    a = re.sub(r'^(?:\.\./)+', '', a)
    a = a[2:] if a.startswith('./') else a
    cands = [a] if '/' in a else [d + '/' + a for d in EXE_DIRS]
    for c in cands:
        if os.path.isabs(c):
            continue
        for b in (root, main):
            if b is None:
                continue
            m = read_manifest(Path(b) / (c + '.deps.txt'), root, main)
            if m:
                return m
    return None


def derive(check, root, main, idx=None):
    """The dependencies of one check (see the part-2 header). Never raises on a odd check: an unreadable one is tier D."""
    import gs_share
    import sched as gs
    import proofkey as pk
    root = Path(root)
    store = Path(main) / 'out' / 'toolcache' / '_scripts' if main else None
    cmd = [str(a) for a in check.get('cmd', []) if isinstance(a, str)]
    files, dirs, idx_files, notes, closure = set(), set(), set(), [], []
    lits, unresolved = set(), []
    exact, tier, partial = True, 'B', False
    gsp = gs_share.parse(cmd)
    if gsp is not None and idx is not None:
        # the game_syntax unit(s): main.cpp, live.cpp + --files, each with the include closure of the index (a TU parsed with errors = partial)
        for tu in gs_share.DEFAULT_FILES + [f for f in gsp[0] if f not in gs_share.DEFAULT_FILES]:
            cl, part, found = idx.closure(tu)
            if not found:
                exact = False
                notes.append(f'{tu}: not in the index')
            partial = partial or part
            files.update(cl)
            idx_files.update(cl)
            if part:
                notes.append(f'{tu}: the index parsed it with errors (partial include list)')
        files.update(['tools/gate/game_syntax.py', 'tools/gate/gs_share.py'])
        if partial:
            exact = False
        return {'tier': 'A' if exact else 'D', 'exact': exact, 'files': sorted(files), 'dirs': ['src'] if partial else [], 'idx_files': sorted(idx_files),
                'notes': notes + ['game_syntax: units + include closure from the ours index'], 'closure': []}
    if gsp is not None:
        return {'tier': 'D', 'exact': False, 'files': [], 'dirs': [], 'idx_files': [], 'notes': ['game_syntax without the ours index'], 'closure': []}
    scripts = []
    for a in cmd:
        if a.lower().endswith(('.py', '.ps1')):
            scripts.append(a.replace('\\', '/'))
        elif len(a) > 24:
            scripts += [m.group(0).replace('\\', '/') for m in gs._SCRIPT_LIT.finditer(a)]
    for a in cmd:   # repo files / folders named on the command line (a script reads its arguments)
        c = a.replace(chr(92), '/').lstrip('./') if not a.startswith('-') else ''
        if c and len(c) < 200:
            lits.add(c)   # DPC F3: a path looked up and not found must change the signature when it appears
        if c and len(c) < 200 and (root / c).is_file():
            files.add(c)
        elif c and len(c) < 200 and c.split('/')[0] in ('src', 'tools', 'probes', 'config', 'db', 'docs', 'assets') and (root / c).is_dir():
            dirs.add(c)
    all_text = []
    builders, others = [], []
    for nm in dict.fromkeys(scripts):
        for p, kind, text, _top in gs.closure_files(root, nm):
            rel = _rel(root, p)
            closure.append(rel)
            files.add(rel)
            all_text.append((rel, kind, text))
    manifest_used = False
    scan_files = []
    for rel, kind, text in all_text:
        if kind == 'ps1':
            if rel in HELPERS:
                continue
            if re.search(r'cxx_cache\.ps1', text):
                ents = script_manifest(root, main, rel, store)
                if ents:
                    files.update(e for e in ents if not e.startswith(('abs:',)))
                    manifest_used = True
                else:
                    exact = False
                    notes.append(f'{rel}: no (valid) dependency manifest')
            else:
                exact = False
                tier = 'D'
                notes.append(f'{rel}: a .ps1 that writes no dependency manifest')
            continue
        code = _code(text, kind)
        if SCAN_RE.search(code) and not rel.endswith(('tools/gate/game_syntax.py',)):
            scan_files.append(rel)
        # repo paths the code names
        sdir = os.path.dirname(rel)
        for m in LIT_RE.finditer(code):
            lit = m.group(1).replace('\\', '/')
            cands = [lit, lit.lstrip('./')] + ([sdir + '/' + lit.lstrip('./')] if sdir else [])   # a script-relative name (HERE / 'x.json') resolves beside the script
            lits.add(cands[1])
            hit = False
            for c in cands:
                c = os.path.normpath(c).replace('\\', '/')
                lits.add(c)
                if (root / c).is_file():
                    files.add(c)
                    hit = True
                    break
            if not hit and not _is_runtime_path(lit):
                unresolved.append(f'{rel}: names {lit}, which is not a file in the checkout')
        for m in UNRES_RE.finditer(code):   # data / binary names the repo-file pattern above does not cover
            lit = m.group(1).replace('\\', '/')
            if _is_runtime_path(lit) or LIT_RE.fullmatch("'" + lit + "'"):
                continue
            cands = [os.path.normpath(x).replace('\\', '/') for x in (lit, lit.lstrip('./'), (sdir + '/' + lit.lstrip('./')) if sdir else lit)]
            lits.update(cands)
            for c in cands:
                if (root / c).is_file():
                    files.add(c)
                    break
            else:
                unresolved.append(f'{rel}: names {lit}, which is not a file in the checkout')
        if FSTR_RE.search(code):
            unresolved.append(f'{rel}: builds a repo path in an f-string (cannot be enumerated)')
        for m in JOIN_RE.finditer(code):
            parts = [m.group(1)] + re.findall(r"['\"]([\w.-]+)['\"]", m.group(2))
            c = '/'.join(parts)
            lits.add(c)   # DPC F3
            if (root / c).is_file():
                files.add(c)
            elif (root / c).is_dir():
                dirs.add(c)
        for m in DIR_LIT.finditer(code):
            c = m.group(1).replace('\\', '/')
            lits.add(c)   # DPC F3
            if (root / c).is_dir():
                dirs.add(c)
        if kind == 'py':
            for em in gs.EXE_LIT.finditer(code):
                mf = _exe_manifest(root, main, em.group(1))
                if mf:
                    files.update(e for e in mf if not e.startswith('abs:'))
                    manifest_used = True
                elif em.group(1).lower() not in SYSTEM_EXES and em.group(1).lower().endswith('.exe'):
                    exact = False
                    notes.append(f'{rel}: runs {em.group(1)}, which no dependency manifest names')
            for tool in ('fview', 'rplay'):   # the renderer tools a script runs by name (infer() in exes.py does the same)
                if re.search(r'\b' + tool + r'(\.exe)?\b', code):
                    got = False
                    for dd in ('out/renderer/dev', 'out/renderer/release'):
                        mf = _exe_manifest(root, main, f'{dd}/{tool}.exe')
                        if mf:
                            files.update(e for e in mf if not e.startswith('abs:'))
                            manifest_used = got = True
                    if not got:
                        exact = False
                        notes.append(f'{rel}: runs {tool}, which no dependency manifest names (build it once through tools\\{"fview" if tool == "fview" else "renderer"}\\build.ps1)')
    for a in cmd:   # a unit exe run directly (cmd[0] / a -Command body)
        for m in EXE_ARG.finditer(a):
            mf = _exe_manifest(root, main, m.group(1))
            if mf:
                files.update(e for e in mf if not e.startswith('abs:'))
                manifest_used = True
            elif re.match(r'(?:\./|\.\./)*(?:out/)', m.group(1).replace('\\', '/')):
                exact = False
                notes.append(f'{m.group(1)}: no dependency manifest (built some other way)')
    if not closure and not manifest_used:   # no script file and no exe with a manifest: an inline `-c` body or a program we know nothing about
        exact = False
        notes.append('the command names no script file and no built exe with a manifest: opaque')
    if scan_files:
        tier, exact = 'C', False
        notes.append('SCAN: ' + ', '.join(sorted(set(scan_files))[:4]) + (' ...' if len(set(scan_files)) > 4 else ''))
    if unresolved and not scan_files:   # S103 DIB review bug 7: a name the script reads that we cannot resolve = deps we cannot name = not exact
        exact = False
        notes.append('unresolved: ' + unresolved[0][:90] + (f' (+{len(unresolved) - 1} more)' if len(unresolved) > 1 else ''))
    scan_dirs = []
    decl = check.get('deps')
    if isinstance(decl, str) and decl.startswith('scan:'):   # S103 DIB review bug 6: a declared scan is a tier C with exactly those directories
        scan_dirs = [d.strip().strip('/').replace('\\', '/') for d in re.split(r'[,;\s]+', decl[5:]) if d.strip()]
        tier, exact = 'C', False
        notes.append('declared "deps": ' + decl)
    if tier != 'C':
        tier = 'D' if not exact else ('A' if manifest_used else 'B')
    # does the check WRITE anything (a producer other checks may read)? Then a skip could starve a consumer, so it never skips on derived deps alone.
    writes = any(WRITES_RE.search(_code(t, k)) for _r, k, t in all_text) or any(a in ('-o', '--out', '--output') or a.startswith('--out') for a in cmd)
    return {'tier': tier, 'exact': exact and tier in ('A', 'B'), 'files': sorted(files), 'dirs': sorted(dirs), 'idx_files': sorted(idx_files), 'notes': notes,
            'closure': sorted(set(closure)), 'writes': writes, 'scan_dirs': scan_dirs, 'lits': sorted(lits), 'manifest': manifest_used,   # QAF: files came (in part) from a build manifest: such an entry is not reused once the manifests moved on
            'uncacheable': check['uncacheable'] if isinstance(check.get('uncacheable'), str) and check['uncacheable'].strip() else ''}


def _under(p, dirs):
    return any(p == d or p.startswith(d + '/') for d in dirs)


def stale_index(derived, changed, idx, root):
    """Files of a derived (index-based) closure whose tree content differs from the index and are NOT in the diff: the index does not describe the base. -> [paths]."""
    if idx is None or not derived.get('idx_files'):
        return []
    ch = set(changed)
    out = []
    for f in derived['idx_files']:
        if f in ch:
            continue
        s = idx.sha(f)
        if s is None:
            out.append(f + ' (not in the index)')
            continue
        if _sha1_file(Path(root) / f) != s:
            out.append(f)
    return out


def effective_dirs(derived):
    """The directories a derived set adds to selection AND to the key (S103 DIB S1): only an EXACT set (tier A / B) contributes the directories its scripts name;
    a tier C / D set contributes FILES only (a blanket src + tools made ~500 checks select on every edit); a declared `"deps": "scan:<dirs>"` always counts."""
    return list(derived.get('scan_dirs', [])) + (list(derived.get('dirs', [])) if derived.get('exact') else [])


def deps_hit(derived, changed):
    """The first changed path that is in the derived file set or under an effective derived directory, or None."""
    fs = set(derived.get('files', []))
    ed = effective_dirs(derived)
    for p in changed:
        if p in fs or _under(p, ed):
            return p
    return None


def _sig(check, root, derived_closure, idx, lits=(), idx_files=(), memo=None):
    """DPC: an entry follows the CONTENT of the files it was derived from. The index contributes the sha of each closure file it describes (`idx_files`), not
    ours.db's mtime: an index rebuild that changed none of those files keeps every entry warm (the old mtime key made each rebuild a cold start for every root).
    QAF: plain os.path calls instead of pathlib (57k stats through Path took 2.4 s of a warm `q ours affects`); the digest is byte-for-byte the same, so every
    saved entry stays valid. memo = a dict a SHORT-LIVED reader (q) shares across checks: each file is statted / hashed once per process, not once per check."""
    import hashlib as _h
    d = _h.sha256()
    d.update(DERIVE_VERSION.encode())
    d.update(str(root).encode())   # S103 DIB review bug 10: an entry is for one checkout
    d.update(repr((check.get('cmd'), check.get('deps'), check.get('uncacheable'))).encode())
    rs = str(root)
    for rel in derived_closure:
        p = os.path.join(rs, rel)
        if memo is None:
            h = _hash_file(p)
        else:
            h = memo.get(('h', p))
            if h is None:
                h = memo[('h', p)] = _hash_file(p)
        d.update(rel.encode() + b'\0' + h.encode())
    for lit in lits:   # which of the literals the derivation looked up exist now: a file that appears or goes changes the derived set
        p = os.path.join(rs, lit)   # (QAF: os.path.exists('a.txt/') is False where Path('a.txt/').exists() was True for a FILE: such a lit costs one recompute, nothing else)
        if memo is None:
            ex = os.path.exists(p)
        else:
            ex = memo.get(('e', p))
            if ex is None:
                ex = memo[('e', p)] = os.path.exists(p)
        d.update(lit.encode() + (b'\0Y' if ex else b'\0N'))
    if idx is None:
        d.update(b'no-index')
    else:
        for rel in idx_files:
            d.update(rel.encode() + b'\0' + (idx.sha(rel) or 'none').encode())
    return d.hexdigest()


def _is_game_syntax(check):
    import gs_share
    try:
        return gs_share.parse([str(a) for a in check.get('cmd', []) if isinstance(a, str)]) is not None
    except Exception:
        return False


TMP_MAX_AGE_S = 15 * 60


def sweep_tmp(base, max_age_s=TMP_MAX_AGE_S, now=None):
    """DPC: remove temp files a killed / failed save left next to the table (`<name>*.tmp`) that are older than max_age_s. -> number removed."""
    import time as _t
    n, now = 0, _t.time() if now is None else now
    base = Path(base)
    try:
        for p in base.parent.glob(base.stem + '*.tmp'):
            try:
                if now - p.stat().st_mtime > max_age_s:
                    p.unlink()
                    n += 1
            except OSError:
                pass
    except OSError:
        pass
    return n


class Table:
    """derive() for every check, cached per checkout in <main>\\out\\gate\\deps.<root key>.json (DPC: one small shard per checkout root, so a load reads and a save
    rewrites only this root's entries; a roots index records each root and a save drops the shards of roots that no longer exist). An entry is keyed by check name
    and a signature of the files its derivation read, the existence of the literals it looked up, the manifests' CONTENT and the sha of its index closure files; a
    stale entry is recomputed, never used."""

    def __init__(self, root, main, cache_path=None, log=print, idx=None):
        self.root, self.main = Path(root), Path(main) if main else Path(root)
        self.idx = idx if idx is not None else open_index(self.root, self.main)   # QAF: idx = a reader's own (snapshot / short-timeout) view
        # QAF (q ours affects): quick = a short-lived reader: the files of one process are statted / hashed once, not once per check;
        # derive_budget_s = stop deriving after this many seconds of derive() work: a cold table (every new checkout starts empty, ~100 s) answers
        # in bounded time with the checks it has not derived yet counted as tier D (deps unknown); `skipped` says how many.
        self.quick = False
        self.derive_budget_s = None
        self.derive_spent = 0.0
        self._skipped = set()   # names, so several files asked in one call count a check once
        self._stale = set()     # names answered from an entry whose build-manifest stamp is older (files unchanged), over the budget
        self._qmemo = {}
        self.base = Path(cache_path) if cache_path else self.main / 'out' / 'gate' / 'deps.json'
        self._rk = hashlib.sha1(str(self.root).encode()).hexdigest()[:10]
        self.path = self.base.with_name(f'{self.base.stem}.{self._rk}{self.base.suffix}')   # this checkout's shard
        self.roots_path = self.base.with_name(f'{self.base.stem}.roots{self.base.suffix}')
        sweep_tmp(self.base)
        self.cache = self._read(self.path)
        self.new = {}
        self.dirty = False
        self.hits = self.misses = 0
        self.log = log
        self._mf = {}

    @staticmethod
    def _read(path):
        try:
            d = json.loads(Path(path).read_text(encoding='utf-8'))
            e = d.get('entries') if isinstance(d, dict) else None
            return e if isinstance(e, dict) else {}
        except (OSError, ValueError):
            return {}

    def close(self):
        if self.idx is not None:
            self.idx.close()

    def underived(self, name):
        """QAF: True when the budget left this check without a derived set (get() answered a tier-D stub)"""
        return name in self._skipped

    @property
    def stale_used(self):
        """QAF: how many checks were answered from an entry older than the latest build manifests (each counted once)"""
        return len(self._stale)

    @property
    def skipped(self):
        """QAF: how many checks the derive budget left underived (each counted once)"""
        return len(self._skipped)

    def refresh(self):
        """Forget the manifest stamp (a build in this gate rewrote manifests): the next get() re-reads it."""
        self._mf.clear()

    def _manifest_stamp(self):
        """A digest of the CONTENT of every manifest in the checkout's / main's out\\deps and the tool cache store (S103 DIB S2: not their mtimes: a rewrite with
        the same content, or a copy, no longer invalidates every entry)."""
        d = hashlib.sha1()
        for b in (self.root / 'out' / 'deps', self.main / 'out' / 'deps', self.main / 'out' / 'toolcache' / '_scripts'):
            try:
                for p in sorted(b.glob('*.deps.txt')):
                    d.update(p.name.encode() + b'\0' + _hash_file(p).encode() + b'\n')
            except OSError:
                pass
        return d.hexdigest()

    def get(self, check):
        name = check['name']
        if _is_game_syntax(check):
            self.misses += 1
            return derive(check, self.root, self.main, self.idx)
        ent = self.cache.get(name)
        mstamp = self._mf.get('s')
        if mstamp is None:
            mstamp = self._mf['s'] = self._manifest_stamp()
        memo = self._qmemo if self.quick else None
        sig_ok = bool(ent) and ent.get('sig') == _sig(check, self.root, ent['d'].get('closure', []), self.idx, ent['d'].get('lits', []), ent['d'].get('idx_files', []), memo)
        if sig_ok and ent.get('mstamp') == mstamp:
            self.hits += 1
            return ent['d']
        if self.derive_budget_s is not None and self.derive_spent >= self.derive_budget_s:
            # QAF: over the budget. The signature covers the script files a check runs, NOT its build manifests, so an entry whose file list came from a manifest
            # (`manifest` true; a tier A entry, or a C / D one that read one) is never reused once the stamp moved on: a source that gained an #include is a new file in it.
            # An entry without the flag (saved before it existed) is treated as one that used a manifest. Only a manifest-free entry (tier B and the like) is reused, counted.
            if sig_ok and ent['d'].get('manifest') is False:
                self._stale.add(name)
                return ent['d']
            self._skipped.add(name)   # QAF: over the budget: not derived, not stored; the caller reports `skipped`
            return {'tier': 'D', 'exact': False, 'files': [], 'dirs': [], 'idx_files': [], 'notes': ['deps not derived yet (derive budget)'], 'closure': []}
        self.misses += 1
        t0 = time.monotonic()
        try:
            d = derive(check, self.root, self.main, self.idx)
        except Exception as e:   # an odd check is tier D, never a crash of the gate
            # DPC F1: a failed derivation is returned, never stored (a transient error must not stay in the table as the check's answer)
            return {'tier': 'D', 'exact': False, 'files': [], 'dirs': [], 'idx_files': [], 'notes': [f'derivation failed: {type(e).__name__}: {e}'], 'closure': []}
        finally:
            self.derive_spent += time.monotonic() - t0
        if _is_game_syntax(check):
            return d   # DPC F2: a game_syntax derivation is a few SQL queries on the index: never cached (its key would have to follow the index's include edges)
        ent = {'sig': _sig(check, self.root, d.get('closure', []), self.idx, d.get('lits', []), d.get('idx_files', []), memo), 'mstamp': mstamp, 'd': d}
        self.cache[name] = self.new[name] = ent
        self.dirty = True
        return d

    def _prune_roots(self):
        """Record this root in the roots index and delete the shards of roots whose folder is gone (a removed worktree)."""
        try:
            roots = json.loads(self.roots_path.read_text(encoding='utf-8'))
            if not isinstance(roots, dict):
                roots = {}
        except (OSError, ValueError):
            roots = {}
        roots[self._rk] = str(self.root)
        for rk, rt in list(roots.items()):
            if rk != self._rk and not Path(rt).exists():
                try:
                    self.base.with_name(f'{self.base.stem}.{rk}{self.base.suffix}').unlink(missing_ok=True)
                except OSError:
                    continue
                del roots[rk]
        # DPC F4: also drop a shard whose OWN recorded root is gone (roots.json can be stale / lost): the root is the shard's first key, read from its first bytes
        try:
            for sp in self.base.parent.glob(f'{self.base.stem}.*{self.base.suffix}'):
                if sp == self.path or sp == self.roots_path:
                    continue
                try:
                    with open(sp, 'rb') as fh:
                        head = fh.read(2048).decode('utf-8', 'replace')
                    m = re.match(r'\{"root": ("(?:[^"\\]|\\.)*")', head)
                    if m and not Path(json.loads(m.group(1))).exists():
                        sp.unlink(missing_ok=True)
                except (OSError, ValueError):
                    continue
        except OSError:
            pass
        tmp = self.roots_path.with_name(self.roots_path.name + f'.{os.getpid()}.tmp')
        try:
            tmp.write_text(json.dumps(roots), encoding='utf-8')
            os.replace(tmp, self.roots_path)
        finally:
            tmp.unlink(missing_ok=True)

    def save(self):
        """Rewrite only this root's shard (merged with what another process saved meanwhile). The temp file is removed whatever happens."""
        if not self.dirty:
            return
        tmp = None
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            cur = self._read(self.path)
            cur.update(self.new)
            tmp = self.path.with_name(self.path.name + f'.{os.getpid()}.tmp')
            tmp.write_text(json.dumps({'root': str(self.root), 'entries': cur}), encoding='utf-8')
            os.replace(tmp, self.path)
            tmp = None
            self.dirty = False
            self.new = {}
            try:
                self._prune_roots()
                self.base.unlink(missing_ok=True)   # the old single table (100s of MB): superseded by the shards
            except OSError:
                pass
        except Exception as e:
            self.log(f'[gate] deps table not saved ({type(e).__name__}: {e}); the gate goes on')
        finally:
            if tmp is not None:
                try:
                    tmp.unlink(missing_ok=True)
                except OSError:
                    pass


def explain(check, derived, changed, aff_hit, idx, root):
    """Lines for `gate.py --why CHECK`."""
    out = [f"check {check['name']}: tier {derived['tier']} ({'exact' if derived['exact'] else 'NOT exact: stays on affects / always-run'}), "
           f"{len(derived['files'])} derived file(s), {len(derived['dirs'])} derived dir(s)"]
    out += ['  note: ' + n for n in derived['notes']]
    aff = check.get('affects')
    out.append('  affects: ' + (json.dumps(aff) if aff is not None else '(none: always runs unless the derived deps are exact)'))
    if aff_hit:
        out.append(f'  affects match: {aff_hit}')
    h = deps_hit(derived, changed)
    out.append(f'  derived match: {h}' if h else '  derived match: none of the changed paths is in the derived deps')
    st = stale_index(derived, changed, idx, root)
    if st:
        out.append(f'  STALE INDEX for {len(st)} file(s) (e.g. {st[0]}): the derived closure is not trusted; a no-affects check runs')
    return out


if __name__ == '__main__':
    print('library for tools\\gate\\gate.py; self test: py -I tools\\gate\\deps_selftest.py')
    sys.exit(2)
