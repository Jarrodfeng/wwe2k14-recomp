r"""The honest proof cache's key builder (S103 lane PS2). Library for tools\gate\gate.py (through fastgate.cache_key_ex); selftest: tools\gate\sched_selftest.py.

A PASS may be served from the cache only when EVERY input the check reads is in its key, so a changed input can never ride on an old PASS. This file
enumerates the inputs a lane json does not spell out, and REFUSES (returns why) when it cannot:
  - content, not mtime, of every `needs` path and every existing file / folder the command names (a capture or case folder: each file's relative path,
    size and FULL content, never a sample); big-file hashes are remembered across gates by (path, size, mtime_ns) -> content hash, so a rewrite that keeps
    the bytes keeps the key and an edit never does; a folder under out/build is its build files only;
  - the check's own script and its local import closure (every .py it imports under tools\ or next to it, and every .py named by a string literal);
    an import that is not stdlib, not installed and not under tools\ makes the check uncacheable (we cannot see what it runs);
  - for a planted-fault twin (`--plant X`, `--expect-fail T`): the key of the real check it pairs with (same lane, same command without the plant flags).
    A twin is cached only together with its pair, under the same key; a twin with no pair is never cached.
Selected tool exes, programs and the toolchain are added by fastgate.cache_key_ex (the file bytes of the exe the check runs: a rebuilt exe with other bytes is a miss).
"""
import ast, hashlib, importlib.util, json, os, re, sys, threading
from pathlib import Path

BIG = 4 << 20   # files above this go through the persistent memo (path, size, mtime_ns -> content hash): a one-time cost per file, never a sample
_MEMO_FILE = None
_MEMO = {}
_DIR_STATE = {}
_DIR_LOCKS = {}
_LOCKS_MU = threading.Lock()
_PY_INDEX = {}
_STDLIB = set(getattr(sys, 'stdlib_module_names', ())) | {'__future__'}


def set_memo(path):
    global _MEMO_FILE
    _MEMO_FILE = Path(path)
    try:
        _MEMO.update(json.loads(_MEMO_FILE.read_text(encoding='utf-8')))
    except Exception:
        pass


def save_memo():
    if _MEMO_FILE is None:
        return
    try:
        cur = {}
        try:
            cur = json.loads(_MEMO_FILE.read_text(encoding='utf-8'))
        except Exception:
            pass
        cur.update(_MEMO)
        tmp = Path(str(_MEMO_FILE) + f'.{os.getpid()}.tmp')
        tmp.write_text(json.dumps(cur), encoding='utf-8')
        os.replace(tmp, _MEMO_FILE)
    except OSError:
        pass


def content_hash(path):
    """blake2b of the file's WHOLE bytes; 'missing' for no file. Files above BIG are remembered by (path, size, mtime_ns): the value is a content hash, so a
    copy / tidy that keeps the bytes keeps the key, and a rewrite (new mtime) is read again in full: no sampling, a middle-of-file edit is always seen."""
    p = str(path)
    try:
        st = os.stat(p)
    except OSError:
        return 'missing'
    big = st.st_size > BIG
    mk = f'{p}|{st.st_size}|{st.st_mtime_ns}'
    if big and mk in _MEMO:
        return _MEMO[mk]
    d = hashlib.blake2b(digest_size=16)
    with open(p, 'rb') as f:
        for blk in iter(lambda: f.read(1 << 16), b''):
            d.update(blk)
    h = d.hexdigest()
    if big:
        _MEMO[mk] = h
    return h


def invalidate(paths):
    """Forget the memoised folder states that overlap any of these absolute paths (a folder a check of this gate may write: its state is read again under
    the check's locks, never taken from an earlier look)."""
    ps = [os.path.normcase(os.path.abspath(str(p))) for p in paths]
    with _LOCKS_MU:
        for k in list(_DIR_STATE):
            nk = os.path.normcase(k[:-2] if k.endswith('|L') else k)   # GKL fix: a listing memo (key + '|L') is the same folder
            if any(nk == q or nk.startswith(q + os.sep) or q.startswith(nk + os.sep) for q in ps):
                del _DIR_STATE[k]


BUILD_FILES = ('build.ninja', 'CMakeCache.txt', 'compile_commands.json', 'rules.ninja')


def _is_build_tree(p):
    return bool(re.search(r'(^|/)out/build(/|$)', str(os.path.abspath(str(p))).replace('\\', '/')))


def content_state(p, fresh=False, listing=False):
    """A file by content; a folder by every file's relative path + size + content; 'missing'. A folder under out/build is only its build files (build.ninja,
    CMakeCache.txt, compile_commands.json, rules.ninja): the game's objects are an output, not a check input. Memoised per folder for the life of the process
    unless fresh=True (see invalidate).
    listing=True (GKL): a folder is keyed by a LISTING (relative path, size, mtime_ns of every file), no file is opened. For the folders a script names in its own code
    (out\\renderer, out\\deps, out\\s103\\...: scratch outputs, often 10^4 files / GB) that nobody declared as an input: L10.draw's key read 18,260 of them in full, 136 s cold /
    9 s warm, under the check's path locks, and again after every slot wait (the stall of 2026-11-19 05:53-06:03). A rewrite changes mtime_ns, an edit size or mtime: the listing
    differs exactly when the content could. Declared inputs (`needs`, command arguments) and every file keep the full content hash."""
    p = Path(p)
    if p.is_file():
        return 'f:' + content_hash(p)
    if not p.is_dir():
        return 'missing'
    k = str(p) + ('|L' if listing else '')
    if not fresh and k in _DIR_STATE:
        return _DIR_STATE[k]
    with _LOCKS_MU:
        lk = _DIR_LOCKS.setdefault(k, threading.Lock())
    with lk:   # parallel checks name the same folder (game_data/extracted: 8 GB): hash it once, the others wait for that one result
        if not fresh and k in _DIR_STATE:
            return _DIR_STATE[k]
        return _dir_state(p, k, listing)


def _dir_state(p, k, listing=False):
    files = []
    if _is_build_tree(p):
        for dp, dn, fn in os.walk(p):
            if os.path.relpath(dp, p).count(os.sep) >= 3:
                dn[:] = []
            for f in sorted(fn):
                if f in BUILD_FILES:
                    fp = os.path.join(dp, f)
                    files.append((os.path.relpath(fp, p).replace('\\', '/'), fp))
        files.sort()
    else:
        for dp, dn, fn in os.walk(p):
            dn.sort()
            for f in sorted(fn):
                fp = os.path.join(dp, f)
                files.append((os.path.relpath(fp, p).replace('\\', '/'), fp))
    d = hashlib.sha256()
    for rel, fp in files:
        try:
            st = os.stat(fp)
            sz = st.st_size
        except OSError:
            continue
        if listing and not _is_build_tree(p):
            if not fp.endswith('.lock'):
                d.update(f'{rel}\0{sz}\0{st.st_mtime_ns}\n'.encode())
            continue
        if fp.endswith('.lock'):
            continue                  # QX4 fix2: a *.lock file (ours.db.lock) is held by a running `q ours update` / build: reading it raises PermissionError and stalled every gate that hashes out/harness/index; a lock file carries no input
        try:
            h = content_hash(fp)
        except PermissionError:
            h = f'locked:{sz}'        # another process holds the file exclusively (Windows): its size stands in for its content
        d.update(f'{rel}\0{sz}\0'.encode() + h.encode() + b'\n')
    _DIR_STATE[k] = 'd:' + d.hexdigest()
    return _DIR_STATE[k]


def _py_index(root):
    k = str(root)
    if k not in _PY_INDEX:
        idx = {}
        for dp, dn, fn in os.walk(Path(root) / 'tools'):
            dn[:] = [d for d in dn if d not in ('__pycache__', 'lanes')]
            for f in fn:
                if f.endswith('.py'):
                    idx.setdefault(f[:-3], []).append(os.path.join(dp, f))
        _PY_INDEX[k] = idx
    return _PY_INDEX[k]


_PY_LIT = re.compile(r"""['"]((?:[\w.-]+[\\/])*[\w.-]+\.py)['"]""")


_CLOSURE = {}


def script_closure(root, script, limit=300):
    k = (str(root), str(script))
    if k not in _CLOSURE:
        _CLOSURE[k] = _script_closure(root, script, limit)
    return _CLOSURE[k]


_FILE_INFO = {}
_SPEC = {}


def _file_info(f):
    """(text, [import-name lists] | None when it does not parse) of one .py, parsed once per (path, mtime, size): the closures of hundreds of checks share it."""
    try:
        st = os.stat(f)
    except OSError:
        return None
    k = str(f)
    hit = _FILE_INFO.get(k)
    if hit and hit[0] == (st.st_mtime_ns, st.st_size):
        return hit[1]
    try:
        text = Path(f).read_text(encoding='utf-8', errors='replace')
    except OSError:
        return None
    try:
        tree = ast.parse(text)
    except SyntaxError:
        res = (text, None)
    else:
        imps = []
        for node in ast.walk(tree):
            names = []
            if isinstance(node, ast.Import):
                names = [a.name.split('.')[0] for a in node.names]
            elif isinstance(node, ast.ImportFrom):
                if node.level == 0 and node.module:
                    names = [node.module.split('.')[0]]
                elif node.level > 0:
                    names = [node.module.split('.')[0]] if node.module else [a.name for a in node.names]
            if names:
                imps.append(names)
        res = (text, imps)
    _FILE_INFO[k] = ((st.st_mtime_ns, st.st_size), res)
    return res


def _script_closure(root, script, limit=300):
    """-> (sorted absolute paths of the script, its local imports and every .py named by a literal; problems). A problem = an input we cannot enumerate."""
    root = Path(root)
    seen, probs = {}, []
    todo = [Path(script)]
    idx = _py_index(root)
    while todo and len(seen) < limit:
        f = todo.pop()
        f = f if f.is_absolute() else root / f
        k = str(f)
        if k in seen or not f.is_file():
            continue
        info = _file_info(f)
        if info is None:
            continue
        text, imports = info
        seen[k] = True
        if imports is None:
            probs.append(f'{f.name} does not parse')
            continue
        for names in imports:
            for n in names:
                if not n or n in _STDLIB:
                    continue
                cands = list(idx.get(n, []))
                loc = f.parent / (n + '.py')
                if loc.is_file():
                    cands.append(str(loc))
                if cands:
                    todo.extend(Path(c) for c in cands)
                    continue
                if n not in _SPEC:
                    try:
                        _SPEC[n] = importlib.util.find_spec(n) is not None
                    except (ImportError, ValueError, AttributeError):
                        _SPEC[n] = False
                if not _SPEC[n]:
                    probs.append(f'{f.name} imports {n}: not stdlib, not installed, not under tools')
        for m in _PY_LIT.finditer(text):
            lit = m.group(1).replace('\\', '/')
            for cand in (root / lit, f.parent / lit):
                if cand.is_file():
                    todo.append(cand)
                    break
    return sorted(seen), probs


_TOK = re.compile(r"""(?<![\w./\\-])((?:[A-Za-z]:)?[\\/]*(?:\.\.?[\\/])*(?:out|game_data|generated)[\\/][^\s'"`;|)(,<>]+)""")


def named_inputs(root, main, cmd):
    """Existing files and folders a command names under out\\ / game_data\\ / generated\\ (whole arguments, or tokens inside a longer one such as a -Command
    body), resolved under root, else under the main checkout. -> sorted [(posix display path, real path)]. Folders under src / tools are not listed: a
    source folder is the business of `affects`."""
    found = {}
    for a in cmd:
        if not isinstance(a, str):
            continue
        base = a.split(':')[0] if not re.match(r'^[A-Za-z]:[\\/]', a) else a
        cands = [base] + [m.group(1) for m in _TOK.finditer(a)]
        for c in cands:
            c = c.strip('\'"').rstrip('\\/')
            if not c or c.startswith('-'):
                continue
            norm = c.replace('\\', '/')
            for b in (Path(root), Path(main), None):
                cand = Path(c) if b is None else b / c
                try:
                    if cand.exists():
                        if cand.is_dir() and not re.search(r'(^|/)(out|game_data|generated)(/|$)', norm):
                            break   # a source folder
                        found.setdefault(os.path.normcase(str(cand.resolve())), (norm, str(cand)))
                        break
                except OSError:
                    pass
    return sorted(found.values())


def strip_plant(cmd):
    """The command of a planted-fault twin with its plant flags removed (= the real check's command), or None when the command is not a plant."""
    cmd = [c for c in cmd]
    j = ' '.join(c for c in cmd if isinstance(c, str))
    if not re.search(r'--plant\b|--expect-fail\b', j):
        return None
    if cmd and cmd[0] in ('pwsh', 'powershell') and cmd:
        m = re.search(r"^(\S+?\.exe)\s+(?:--\w[\w-]*\s+\S+\s+)*--plant\s+\S+\s*;\s*if\s*\(\s*\$LASTEXITCODE\s*-eq\s*0\s*\)\s*\{\s*exit 1\s*\}\s*else\s*\{\s*exit 0\s*\}\s*$", str(cmd[-1]))
        if m:
            return [m.group(1)]
    out, i = [], 0
    while i < len(cmd):
        c = cmd[i]
        if c in ('--plant', '--expect-fail') and i + 1 < len(cmd):
            i += 2
            continue
        if isinstance(c, str) and c.startswith(('--plant=', '--expect-fail=')):
            i += 1
            continue
        out.append(c)
        i += 1
    return out


def is_plant(check):
    return strip_plant(check.get('cmd', [])) is not None


def pair_real(check, lane_checks):
    """The real check a planted-fault twin pairs with: same lane, same command without the plant flags, not a plant itself, not retired. None when none."""
    base = strip_plant(check.get('cmd', []))
    if base is None:
        return None
    for o in lane_checks:
        if o is check or o.get('retired') or strip_plant(o.get('cmd', [])) is not None:
            continue
        if o.get('cmd') == base:
            return o
    return None
