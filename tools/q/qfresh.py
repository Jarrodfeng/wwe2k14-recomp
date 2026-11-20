"""qfresh: make a BORROWED main-checkout index current for a worktree's edited files, in seconds, with a hard time budget (S103 QX1).

What it answers: `q ours def/refs/callers/fn ...` run inside a worktree must give the worktree's CURRENT line numbers for the files the
branch changed, without a long refresh (the clang overlay of `q ours --worktree .` takes 30-50 s the first time and is still there for exact
answers). How: the files the branch differs in (git diff against the merge base + untracked, then a content compare with the main checkout's
copy, which is what the index was built from) are diffed with `git diff --no-index -U0`; every index row of such a file gets its line (and
endline) mapped through the hunks by a SQL function over TEMP views of the base db (nothing is copied, nothing is written).
  * a row inside a deleted / replaced hunk maps to the start of the replacement (approximate there);
  * a symbol the branch ADDED is not in the index: the caller falls back to `text_hits` (a word search of the changed files) and says so;
  * over the budget (WWE_Q_FRESH_BUDGET seconds, default 8): the files not yet mapped are left as the base has them and listed as `stale`;
  * test hook: WWE_Q_FRESH_PLANT_MS=N sleeps N ms per file (a planted slow parse) so the budget path can be proven.
Everything is read-only; no index file is created.

    import qfresh; con, info = qfresh.wrap(base_con, root, base_path)   # info: {'changed','mapped','stale','new','secs','note'}
"""
import os
import re
import sqlite3
import subprocess
import time

CPP = ('.h', '.hpp', '.cpp', '.inl', '.cc', '.inc')
TOPS = ('src', 'tools', 'probes')


def _git(cwd, *a, timeout=10):
    r = subprocess.run(['git', '-C', cwd] + list(a), capture_output=True, text=True, errors='replace', timeout=max(1.0, timeout))
    return r.returncode, r.stdout


def _main_of(root):
    rc, out = _git(root, 'rev-parse', '--git-common-dir')
    if rc != 0 or not out.strip():
        return None
    cd = os.path.abspath(os.path.join(root, out.strip()))
    return os.path.dirname(cd) if os.path.basename(cd) == '.git' else None


def _norm(b):
    return b.replace(b'\r\n', b'\n')


HUNK = re.compile(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@', re.M)


def hunks_of(main_file, wt_file, timeout):
    """-> [(old_start, old_count, new_start, new_count)] of main_file -> wt_file (git diff --no-index -U0). A git error raises OSError (never 'mapped')."""
    if not (os.path.isfile(main_file) and os.path.isfile(wt_file)):
        raise OSError(f'hunks_of: missing file {main_file if not os.path.isfile(main_file) else wt_file}')
    rc, out = _git(os.path.dirname(wt_file) or '.', 'diff', '--no-index', '--no-ext-diff', '--no-color', '-U0', '--ignore-cr-at-eol', '--', main_file, wt_file,
                   timeout=timeout)
    if rc > 1 or (rc == 1 and '@@' not in out and 'Binary files' not in out and 'diff --git' in out):
        raise OSError(f'git diff --no-index failed (exit {rc}) for {wt_file}')
    hs = []
    for m in HUNK.finditer(out):
        os_, oc, ns, nc = int(m.group(1)), m.group(2), int(m.group(3)), m.group(4)
        hs.append((os_, 1 if oc is None else int(oc), ns, 1 if nc is None else int(nc)))
    return hs


def map_line(hs, line, end=False):
    """a line of the OLD file -> (line of the NEW file, inside_a_changed_hunk). Inside a hunk: the start of the replacement (end=True: its LAST line,
    for an endline), approximate."""
    if line is None:
        return None, False
    off = 0
    for os_, oc, ns, nc in hs:
        if oc == 0:                    # pure insertion after old line os_
            if line > os_:
                off += nc
            else:
                break
        elif line >= os_ + oc:         # the hunk lies wholly before this line
            off += nc - oc
        elif line >= os_:              # inside the changed / deleted region
            if end:
                return (ns + nc - 1 if nc else ns), True
            return (ns + min(line - os_, max(nc - 1, 0)) if nc else ns), True
        else:
            break
    return line + off, False


_SHA_CACHE = {'path': None, 'rows': {}, 'dirty': False}


def sha_cache_open(root):
    """S105 QC: a (path -> size, mtime_ns, sha1) cache under root/out/harness: hashing ~2000 files took 0.4 s of every worktree q call. A file whose size and
    mtime_ns are unchanged keeps its hash; anything else is re-hashed (the planted twin: a same-size edit with a new mtime must re-hash)."""
    p = os.path.join(root, 'out', 'harness', 'qfresh_sha.json')
    _SHA_CACHE.update(path=p, rows={}, dirty=False)
    try:
        import json
        with open(p, encoding='utf-8') as f:
            _SHA_CACHE['rows'] = json.load(f)
    except (OSError, ValueError):
        pass


def sha_cache_save():
    if not (_SHA_CACHE['dirty'] and _SHA_CACHE['path']):
        return
    tmp = _SHA_CACHE['path'] + f'.{os.getpid()}.tmp'
    try:
        import json
        os.makedirs(os.path.dirname(_SHA_CACHE['path']), exist_ok=True)
        with open(tmp, 'w', encoding='utf-8') as f:
            json.dump(_SHA_CACHE['rows'], f)
        os.replace(tmp, _SHA_CACHE['path'])
    except OSError:                  # the cache file is held open by another q call (Windows): the save is lost, the tmp must not stay (QC L4)
        try:
            os.remove(tmp)
        except OSError:
            pass
    _SHA_CACHE['dirty'] = False


RACY_NS = 2_000_000_000


def sha_of(path):
    """sha1 of the file. A cached hash is trusted only when (size, mtime_ns) match AND the entry was cached >= 2 s after that mtime: a file hashed in the same
    clock tick as its write (or a copy that kept an old mtime) is re-hashed until the entry ages out (git's racy-clean rule, QC M2)."""
    import hashlib
    import time
    rows = _SHA_CACHE['rows'] if _SHA_CACHE['path'] else None
    if rows is not None:
        st = os.stat(path)
        e = rows.get(path)
        try:
            if e and e[0] == st.st_size and e[1] == st.st_mtime_ns and e[3] - e[1] >= RACY_NS:
                return e[2]
        except (IndexError, TypeError):
            pass
    with open(path, 'rb') as f:
        h = hashlib.sha1(f.read()).hexdigest()
    if rows is not None:
        rows[path] = [st.st_size, st.st_mtime_ns, h, time.time_ns()]
        _SHA_CACHE['dirty'] = True
    return h


def classify(root, main, base_rows):
    """compare the worktree's C++ files with what the index was BUILT FROM (files.sha / size), not with the main checkout's current copy.
    -> (changed, new, removed, unmappable): unmappable = changed, but main's copy is no longer what the index was built from (the old text is gone:
    those files are listed stale, never mapped)."""
    changed, new, unmappable = [], [], set()
    seen = set()
    for top in TOPS:
        for dp, dn, fn in os.walk(os.path.join(root, top)):
            dn[:] = [d for d in dn if d not in ('__pycache__', 'node_modules')]
            for f in fn:
                if not f.endswith(CPP):
                    continue
                p = os.path.join(dp, f)
                rel = p[len(root):].lstrip('\\/').replace('\\', '/') if p.startswith(root) else os.path.relpath(p, root).replace('\\', '/')   # S105 QC: relpath was 0.09 s
                seen.add(rel)
                if rel not in base_rows:
                    new.append(rel)
                    continue
                sha, size = base_rows[rel]
                try:
                    if os.stat(p).st_size == size and sha_of(p) == sha:
                        continue
                    mp = os.path.join(main, rel) if main else None
                    main_ok = bool(mp) and os.path.exists(mp) and sha_of(mp) == sha
                    if main_ok:   # same text with other line endings (a checkout's autocrlf) is not a change
                        with open(p, 'rb') as a, open(mp, 'rb') as b:
                            if _norm(a.read()) == _norm(b.read()):
                                continue
                    else:
                        unmappable.add(rel)
                    changed.append(rel)
                except OSError:
                    unmappable.add(rel)
                    changed.append(rel)
    removed = sorted(r for r in base_rows if r not in seen and r.split('/')[0] in TOPS and r.endswith(CPP))
    return sorted(changed), sorted(new), removed, unmappable


def wrap(base_con, root, base_path):
    """-> (connection with the remapped views, info). base_con is only used to read the file list."""
    t0 = time.time()
    budget = float(os.environ.get('WWE_Q_FRESH_BUDGET', '8') or 8)
    deadline = t0 + budget
    plant = int(os.environ.get('WWE_Q_FRESH_PLANT_MS', '0') or 0)
    info = {'changed': [], 'mapped': [], 'stale': [], 'new': [], 'removed': [], 'approx': 0, 'secs': 0.0, 'note': ''}
    maps = {}
    main = _main_of(root)
    base_rows = {r[0]: (r[1], r[2]) for r in base_con.execute('SELECT path, sha, size FROM files')}
    unmappable = set()
    sha_cache_open(root)
    try:
        changed, new, removed, unmappable = classify(root, main, base_rows)
    except (OSError, subprocess.TimeoutExpired):
        changed, new, removed = [], [], []
        info['stale'].append('(file scan failed: no file list)')
    sha_cache_save()
    info['new'], info['removed'] = new, removed
    for rel in changed:
        info['changed'].append(rel)
        if rel in unmappable or time.time() >= deadline:
            info['stale'].append(rel)
            continue
        if plant:
            time.sleep(plant / 1000.0)
            if time.time() >= deadline:
                info['stale'].append(rel)
                continue
        try:
            maps[rel] = hunks_of(os.path.join(main, rel), os.path.join(root, rel), max(1.0, deadline - time.time()))
            info['mapped'].append(rel)
        except (subprocess.TimeoutExpired, OSError):
            info['stale'].append(rel)
    mem = sqlite3.connect(':memory:', uri=True)
    mem.row_factory = sqlite3.Row
    mem.execute('ATTACH DATABASE ? AS base', (f'file:{base_path.replace(chr(92), "/")}?mode=ro',))
    mem.execute('CREATE TEMP TABLE removed(path TEXT PRIMARY KEY)')
    mem.executemany('INSERT INTO removed VALUES(?)', [(r,) for r in removed])
    bc = sqlite3.connect(f'file:{base_path.replace(chr(92), "/")}?mode=ro', uri=True)
    names, lines_cache = {}, {}

    def name_of(usr):
        if usr not in names:
            r = bc.execute('SELECT name FROM syms WHERE usr = ?', (usr,)).fetchone()
            names[usr] = r[0] if r else None
        return names[usr]

    def wt_lines(f):
        if f not in lines_cache:
            try:
                with open(os.path.join(root, f), encoding='utf-8', errors='replace') as fh:
                    lines_cache[f] = fh.read().split('\n')
            except OSError:
                lines_cache[f] = []
        return lines_cache[f]

    def rl(f, line, usr):
        """map a start line; a row inside a changed hunk is checked: its symbol name must be on the mapped line, else the nearest line of the file that
        has the name (a function moved elsewhere), else the hunk start counted as approximate."""
        hs = maps.get(f)
        if not hs or line is None:
            return line
        new_line, inside = map_line(hs, line)
        if not inside:
            return new_line
        nm = name_of(usr)
        if not nm:
            return new_line
        rx = re.compile(r'\b' + re.escape(nm) + r'\b')
        ls = wt_lines(f)
        if 1 <= new_line <= len(ls) and rx.search(ls[new_line - 1]):
            return new_line
        best = None
        for i, t in enumerate(ls, 1):
            if rx.search(t) and (best is None or abs(i - new_line) < abs(best - new_line)):
                best = i
        if best is None:
            info['approx'] += 1
        return best if best is not None else new_line

    def rle(f, line):
        hs = maps.get(f)
        return line if not hs or line is None else map_line(hs, line, end=True)[0]
    mem.create_function('rl', 3, rl)
    mem.create_function('rle', 2, rle)
    gone = 'file NOT IN (SELECT path FROM temp.removed)'
    for name, sel, where in (('syms', 'usr, qname, name, kind, file, rl(file, line, usr) AS line, isdef, builds', gone),
                             ('refs', 'usr, kind, file, rl(file, line, usr) AS line, col, func, builds', gone),
                             ('funcs', 'usr, file, rl(file, line, usr) AS line, rle(file, endline) AS endline, builds, cvars, logs', gone),
                             ('tus', '*', '1'), ('incs', '*', '1'), ('files', '*', 'path NOT IN (SELECT path FROM temp.removed)'), ('meta', '*', '1')):
        mem.execute(f'CREATE TEMP VIEW {name} AS SELECT {sel} FROM base.{name} WHERE {where}')
    info['secs'] = round(time.time() - t0, 1)
    n = len(info['changed']) + len(info['new']) + len(info['removed'])
    if n:
        bits = [f'{len(info["mapped"])} changed files have their lines remapped to this worktree (rows inside an edited region are checked for their name; `~approx` when it is gone)']
        if info['new']:
            bits.append(f'{len(info["new"])} new files are not indexed (symbols added by this branch are not found: a text search of them is shown instead; '
                        f'exact: q ours --worktree . CMD)')
        if info['removed']:
            bits.append(f'{len(info["removed"])} files deleted / renamed in this worktree: their rows are dropped ({", ".join(info["removed"][:3])}'
                        f'{" ..." if len(info["removed"]) > 3 else ""})')
        if info['stale']:
            bits.append(f'stale: {len(info["stale"])} files keep the main checkout\'s lines (over the {budget:g} s budget, or main changed them since the index was built) '
                        f'({", ".join(info["stale"][:3])}{" ..." if len(info["stale"]) > 3 else ""})')
        info['note'] = f'q ours: worktree: borrowed main index, {"; ".join(bits)} [{info["secs"]} s]'
    return mem, info


TYPE_WORDS = {'int', 'float', 'double', 'bool', 'char', 'void', 'auto', 'const', 'unsigned', 'signed', 'long', 'short', 'static', 'constexpr', 'inline', 'size_t', 'struct',
              'class', 'enum', 'volatile', 'extern', 'mutable'}


def _type_like(prefix, sep):
    """`a * name` / `b & name` are expressions; `T* name` / `uint32_t& name` / `const Foo* name` are declarations: when the separator holds a * or &, the token before it must
    look like a type (a builtin / qualifier, `_t`, a qualified or templated name, or a CamelCase one); a plain `name name` pair (`int name`) always counts"""
    if not re.search(r'[*&]', sep):
        return True
    toks = re.findall(r'[\w:<>,]+', prefix)
    if not toks:
        return False
    t = toks[-1]
    return t in TYPE_WORDS or t.endswith('_t') or '::' in t or '<' in t or t[:1].isupper() or any(x in TYPE_WORDS for x in toks[:-1])


def text_hits(root, info, name, limit=40):
    """a word search of the branch's changed + new files for `name` (a symbol the index cannot know yet): [(rel, line, text)]. QGL: the declaration-looking lines
    (`struct NAME`, `int NAME = ..`, `NAME(` after a type) come first and are marked `[decl?]`; up to `limit` lines (was 6: a field used in 8 places showed 3)."""
    tail = re.split(r'::|\.', name.strip())[-1]
    if not tail:
        return []
    rx = re.compile(r'\b' + re.escape(tail) + r'\b')
    decl = re.compile(r'(?:\b(?:struct|class|enum|union|using|typedef)\s+' + re.escape(tail) + r'\b|#\s*define\s+' + re.escape(tail) + r'\b|'
                      r'(?<![.>\w])([A-Za-z_][\w:<>,*&\s]*?)([\s*&]+)' + re.escape(tail) + r'\s*(?:=|;|\(|\{|\[|,\s*\w+\s*(?:=|;|,)))')
    out = []
    for rel in info['changed'] + info['new']:
        try:
            with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
                for i, ln in enumerate(f, 1):
                    if rx.search(ln):
                        t = ln.strip()
                        dm = decl.search(t)
                        d = bool(dm) and not t.startswith(('//', 'return ')) and (dm.group(1) is None or _type_like(dm.group(1), dm.group(2)))
                        out.append((rel, i, ('[decl?] ' if d else '') + t[:140]))
        except OSError:
            pass
    out.sort(key=lambda h: (not h[2].startswith('[decl?]'), h[0], h[1]))
    return out[:limit]


def near_names(root, info, name, limit=12):
    """QGL: no word match: identifiers of the branch's changed + new files that contain the name ignoring case and underscores (CharE12 -> char_e12, kCharE12):
    [(identifier, rel, first_line)]. A typo or another spelling of the same thing is the usual reason a reviewer finds nothing."""
    key = re.sub(r'[_\W]', '', name.strip().split('::')[-1]).lower()
    if len(key) < 3:
        return []
    seen, out = {}, []
    idr = re.compile(r'[A-Za-z_]\w*')
    for rel in info['changed'] + info['new']:
        try:
            with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
                for i, ln in enumerate(f, 1):
                    for m in idr.finditer(ln):
                        w = m.group(0)
                        if w not in seen and key in w.replace('_', '').lower():
                            seen[w] = (rel, i)
                            out.append((w, rel, i))
        except OSError:
            pass
    return out[:limit]
