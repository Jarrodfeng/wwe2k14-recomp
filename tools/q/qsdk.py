"""qsdk: `q sdk` = a READ-ONLY second index over the ReXGlue SDK source (thirdparty\\rexglue-sdk-src\\src + \\include) AND the third-party code the SDK carries that our runtime
runs (SDL3, Dear ImGui, VMA, spdlog, fmt, CLI11, toml++, crypto, disasm, pe), S103 QX5 / QSD. Every hit is tagged with its part ([sdk], [sdl3], [imgui], ...); --part NAME keeps one part.
AUTOMATIC: a query builds the index when it is missing (seconds) and re-reads any file whose size / mtime changed (a stat walk of ~2600 files, well under a second when nothing
changed). In a worktree the index lives in the MAIN checkout's out\\harness\\index (the SDK tree only exists there), one db, one writer lock. At most 4 workers, fewer when free RAM
is low. Every answer's header line says how old the index is and how many files were re-read just now.

    py tools/q/q.py sdk update [-j N] [--sdk PATH]   force a refresh (rarely needed: EVERY query refreshes by itself, see below)
    py tools/q/q.py sdk def NAME             where a function / method / class / struct / enum / enumerator / variable / macro / cvar is declared and defined (file:line)
    py tools/q/q.py sdk refs NAME            every mention, grouped by file and enclosing function, with the kind (call / write / ref / str)
    py tools/q/q.py sdk callers NAME         the call sites only (NAME followed by '(') with the function each sits in
    py tools/q/q.py sdk info                 what the index holds and when it was built
NAME = a tail (OnCreateDialogs) or a qualified suffix (rex::Foo::Bar, ImGuiDrawer::Draw); a cvar name (REXCVAR_DEFINE_* in the SDK) is a definition too.

Not libclang: a lexer-level index (comments / string contents blanked, brace-scope stack for namespaces / classes / functions, one row per
(name, file, function, kind) with its lines), so it is fast and needs no compile flags; an overload set shows as several rows. The SDK source is never
modified and never copied: the db holds names, files and lines only (it lives in out\\, scratch). From a worktree the main checkout's SDK source is read.
"""
import os
import re
import sqlite3
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import qx5  # noqa: E402

EXTS = ('.h', '.hpp', '.cpp', '.inl', '.cc', '.mm', '.c', '.m')
# (root relative to the SDK tree, recurse). The SDK itself, then the third-party trees our runtime's code paths use. Skipped on purpose (agents never read them, or they are
# generated / vendored and huge): tracy (profiler, 300 files), simde (1030 SIMD-wrapper files), glslang / spirv-* / dxc / dxbc (shader compilers, build time only), vulkan-headers
# (generated registry headers), vulkan-loader, FFmpeg, catch2, moltenvk, libmspack, xxHash, utfcpp, stb, o1heap, renderdoc, tiny-aes-c / aes_128, inja, disruptorplus.
# Under the third-party roots the SKIP_DIRS (tests, docs, examples, bench) are not walked.
ROOTS = [('src', True), ('include', True),
         ('thirdparty/sdl3/src', True), ('thirdparty/sdl3/include', True),
         ('thirdparty/imgui', False), ('thirdparty/imgui/backends', False),
         ('thirdparty/vulkan-memory-allocator/include', True), ('thirdparty/vulkan-memory-allocator/src', True),
         ('thirdparty/spdlog/include', True), ('thirdparty/fmt/include', True), ('thirdparty/fmt/src', True),
         ('thirdparty/cli11/include', True), ('thirdparty/tomlplusplus/include', True),
         ('thirdparty/crypto', True), ('thirdparty/disasm', False), ('thirdparty/pe', False)]
SKIP_DIRS = ('__pycache__', '.git', 'test', 'tests', 'docs', 'doc', 'example', 'examples', 'bench', 'fuzz', 'build-scripts')
MAX_JOBS = 4
SCHEMA = 6      # also bump when the lexer's rules change: an index built by older rules is refused, not trusted
KEYWORDS = set('''alignas alignof and asm auto bool break case catch char class const consteval constexpr constinit const_cast continue decltype default delete do double
dynamic_cast else enum explicit export extern false final float for friend goto if inline int long mutable namespace new noexcept not nullptr operator or override
private protected public register reinterpret_cast return short signed sizeof static static_assert static_cast struct switch template this thread_local throw true try
typedef typeid typename union unsigned using virtual void volatile while xor uint8_t uint16_t uint32_t uint64_t int8_t int16_t int32_t int64_t size_t std'''.split())
TOKEN = re.compile(r"\n|[A-Za-z_]\w*|::|==|!=|<=|>=|\+\+|--|->|<<=|>>=|[+\-*/%&|^]=|\d[\w.']*|[{}()\[\];=,~<>:]|\S")
DEFINE = re.compile(r'^\s*#\s*define\s+(\w+)')
CVAR_DEF = re.compile(r'\bREXCVAR_DEFINE_(\w+)\s*\(\s*(\w+)\s*,')
STR_ID = re.compile(r'"([A-Za-z_][A-Za-z0-9_]{3,})"')
MACRO_LIKE = re.compile(r'[A-Z][A-Z0-9_]{2,}')


def db_path(root, sdk_arg=None):
    """the default SDK (thirdparty/rexglue-sdk-src) is out/harness/index/sdk.s<SCHEMA>.db; --sdk PATH (a thirdparty/sdk-wt/<name> tree) has its own sdk-<name>.s<SCHEMA>.db.
    S105 D (F1): the schema is in the FILE NAME: a q of another schema uses ANOTHER file and never opens, wipes or rebuilds this one (the design of qasset.db_name)."""
    if os.environ.get('WWE_QSDK_DB'):      # S105 D (F-3): another file (how a builder / a gate lane works without touching the shared index)
        return os.environ['WWE_QSDK_DB']
    name = ('sdk' if not sdk_arg else 'sdk-' + re.sub(r'[^\w.-]', '_', os.path.basename(os.path.normpath(sdk_arg)))) + '.s%d.db' % SCHEMA
    if not sdk_arg and not os.path.isdir(os.path.join(root, 'thirdparty', 'rexglue-sdk-src')):
        sdk = qx5.sdk_root(root)           # a worktree: the SDK tree is the main checkout's, so its index lives there too
        if sdk:
            root = os.path.dirname(os.path.dirname(sdk))
    return os.path.join(root, 'out', 'harness', 'index', name)


def other_index_files(path):
    """index files of OTHER schemas of the same SDK beside ours (the old sdk.db, sdk.s5.db ...): left alone by q, named in the header so the tidy / a person can clear them"""
    me = os.path.basename(path)
    base = re.sub(r'\.s\d+\.db$', '', me)
    try:
        return sorted(f for f in os.listdir(os.path.dirname(path)) if re.match(r'^' + re.escape(base) + r'(\.s\d+)?\.db$', f) and f != me)
    except OSError:
        return []


def part_of(rel):
    """'sdk' for src / include, else the third-party folder name (thirdparty/sdl3/... -> sdl3)"""
    return rel.split('/')[1] if rel.startswith('thirdparty/') and rel.count('/') >= 2 else 'sdk'


def scan(sdk):
    """{rel path: (size, mtime_ns)} of every indexed source file"""
    files = {}
    for top, rec in ROOTS:
        base = os.path.join(sdk, top)
        if not os.path.isdir(base):
            continue
        for dp, dn, fn in os.walk(base):
            if not rec:
                dn[:] = []
            else:
                dn[:] = [d for d in dn if d not in SKIP_DIRS] if top.startswith('thirdparty') else [d for d in dn if d not in ('__pycache__', '.git')]
            for f in fn:
                if f.endswith(EXTS):
                    fp = os.path.join(dp, f)
                    try:
                        st = os.stat(fp)
                    except OSError:
                        continue
                    files[qx5.rel_of(sdk, fp)] = (st.st_size, st.st_mtime_ns)
    return files


def safe_jobs(want):
    """at most MAX_JOBS workers, fewer when free RAM is low (the build_ours rule; a lexer worker is ~0.3 GB, so 1 GB each with 4 GB left over)"""
    try:
        import build_ours
        free = build_ours._free_ram_gb()
    except Exception:
        free = None
    by_ram = MAX_JOBS if free is None else int((free - 4) // 1)
    return max(1, min(want or MAX_JOBS, MAX_JOBS, by_ram))


def sdk_head(sdk):
    """git HEAD of the SDK tree (None when it is not a git checkout / git fails)"""
    import subprocess
    try:
        r = subprocess.run(['git', '-C', sdk, 'rev-parse', 'HEAD'], capture_output=True, text=True, timeout=20)
        return (r.stdout.strip() or None) if r.returncode == 0 else None
    except (OSError, subprocess.SubprocessError):
        return None


PP_IF = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif|elifdef|elifndef|else|endif)\b(.*)$')


def strip_pp(text):
    """-> (text with every preprocessor line blanked (continuations too) and every `#if 0` region blanked (its #else / #elif branch stays live), [(macro name, line)])"""
    out, defs, cont = [], [], False
    stack = []        # one [branch is dead, enclosing code is dead] per open #if
    for i, ln in enumerate(text.split('\n'), 1):
        dead = stack[-1][0] if stack else False
        if cont or ln.lstrip().startswith('#'):
            if not cont:
                m = PP_IF.match(ln)
                if m:
                    k, rest = m.group(1), re.sub(r'(//.*|/\*.*)$', '', m.group(2)).strip()
                    if k in ('if', 'ifdef', 'ifndef'):
                        stack.append([dead or (k == 'if' and rest in ('0', '(0)')), dead])
                    elif k in ('elif', 'elifdef', 'elifndef', 'else') and stack:
                        stack[-1][0] = stack[-1][1]
                    elif k == 'endif' and stack:
                        stack.pop()
                else:
                    m = DEFINE.match(ln)
                    if m and not dead:
                        defs.append((m.group(1), i))
            cont = ln.rstrip().endswith('\\')
            out.append('')
        else:
            out.append('' if dead else ln)
    return '\n'.join(out), defs


def skip_angles_back(buf, j):
    """buf[j] is '>': the index of its matching '<' (-1 when unbalanced)"""
    depth = 0
    while j >= 0:
        if buf[j] == '>':
            depth += 1
        elif buf[j] == '<':
            depth -= 1
            if depth == 0:
                return j
        j -= 1
    return -1


def fn_name(buf):
    """the (qualified name chain, index of the token after its parameter list's '(') of a function-like statement buffer, or (None, -1)"""
    for i, t in enumerate(buf):
        if t != '(' or i == 0:
            continue
        j = i - 1
        prev = buf[j]
        if prev == 'operator':                    # operator()(...)
            return 'operator()', i + 2
        if j >= 1 and buf[j - 1] == 'operator':   # operator==(...), operator<<(...)
            return 'operator' + prev, i
        if prev == '>':                           # a template-id before the '(': Foo<int>(...)
            k = skip_angles_back(buf, j)
            if k <= 0:
                continue
            j = k - 1
            prev = buf[j]
        if not (prev[0].isalpha() or prev[0] == '_') or prev in KEYWORDS:
            continue
        parts = [prev]
        while j >= 2 and buf[j - 1] == '::':      # Ns::Wrap<int>::Run: the template arguments are skipped, the names kept
            tj = j - 2
            if buf[tj] == '>':
                k = skip_angles_back(buf, tj)
                if k <= 0:
                    break
                tj = k - 1
            if not (buf[tj][0].isalpha() or buf[tj][0] == '_'):
                break
            parts.insert(0, buf[tj])
            j = tj
        if j >= 1 and buf[j - 1] == '~':
            parts[-1] = '~' + parts[-1]
        return '::'.join(parts), i
    return None, -1


def lex_file(text):
    """-> (defs [(name, qname, kind, line)], refs {(name, qual, func, kind): [lines]}) for one source text"""
    code, macros = strip_pp(qx5.mask_all(text))
    defs = [(n, n, 'macro', ln) for n, ln in macros]
    refs = {}
    stack = [('file', '')]           # (kind, qualified name): file / ns / class / enum / func / block
    buf, bufl = [], []
    line = 1
    toks = [(m.group(0), m.start()) for m in TOKEN.finditer(code)]
    n = len(toks)
    i = 0

    def scope_q():
        for k, q in reversed(stack):
            if k in ('ns', 'class', 'enum') and q:
                return q
            if k == 'func':
                return ''
        return ''

    def cur_func():
        for k, q in reversed(stack):
            if k == 'func':
                return q
        return ''

    def qualify(nm):
        pre = scope_q()
        return (pre + '::' + nm) if pre else nm

    def add_ref(name, qual, kind, ln):
        f = cur_func()
        if not f:
            sq = scope_q()
            f = f'<in {sq}>' if sq else ''
            if kind == 'call':
                kind = 'decl'          # NAME( at file / namespace / class scope is a declaration, not a call
        refs.setdefault((name, qual, f, kind), []).append(ln)

    def decl_from(buf_, bufl_, terminator):
        """a statement ended by ';' or '{' at file / ns / class / enum scope: record its definition / declaration"""
        if not buf_:
            return None
        k = stack[-1][0]
        first = buf_[0]
        if first in ('using', 'typedef', 'friend', 'template', 'static_assert', 'extern') and first != 'extern':
            if first in ('using', 'typedef') and terminator == ';':
                if first == 'using' and len(buf_) > 2 and buf_[2] == '=':
                    defs.append((buf_[1], qualify(buf_[1]), 'alias', bufl_[1]))
                elif first == 'typedef':
                    ids = [t for t in buf_ if t[0].isalpha() or t[0] == '_']
                    if ids:
                        defs.append((ids[-1], qualify(ids[-1]), 'alias', bufl_[buf_.index(ids[-1])]))
            if first != 'template':
                return None
        if k == 'enum':
            ids = [t for t in buf_ if t[0].isalpha() or t[0] == '_']
            if ids:
                defs.append((ids[0], qualify(ids[0]), 'enumerator', bufl_[buf_.index(ids[0])]))
            return None
        nm, pi = fn_name(buf_)
        eq = buf_.index('=') if '=' in buf_ else -1
        if nm and (eq < 0 or eq > pi):
            last = nm.split('::')[-1]
            if MACRO_LIKE.fullmatch(last):
                return None
            if '::' not in nm and buf_[0] == last and (k in ('file', 'ns') or (k == 'class' and last != stack[-1][1].split('::')[-1])):
                return None      # `name(args);` with nothing before it at file / namespace scope (or in a class, where only the constructor looks so) is a macro invocation (static_assert_size(...))
            q = qualify(nm)
            defs.append((nm.split('::')[-1], q, 'function' if terminator == '{' else 'decl', bufl_[min(pi, len(bufl_)) - 1]))
            return q
        if terminator == ';':
            cut = eq if eq >= 0 else len(buf_)
            head = [t for t in buf_[:cut] if t not in ('[', ']')]
            ids = [t for t in head if (t[0].isalpha() or t[0] == '_') and t not in KEYWORDS]
            if ids and not (len(buf_) >= 2 and buf_[0] in ('class', 'struct', 'union', 'enum', 'namespace')) and len(head) >= 2:
                ln = bufl_[buf_.index(ids[-1])] if ids[-1] in buf_ else bufl_[0]
                defs.append((ids[-1], qualify(ids[-1]), 'variable', ln))
        return None

    while i < n:
        t, pos = toks[i]
        if t == '\n':
            line += 1
            i += 1
            continue
        top = stack[-1][0]
        if t == '{':
            kw = next((x for x in buf if x in ('namespace', 'class', 'struct', 'union', 'enum')), None)
            has_paren = '(' in buf
            if kw == 'namespace':
                idx = buf.index('namespace')
                nm = ''
                j = idx + 1
                parts = []
                while j < len(buf) and (buf[j][0].isalpha() or buf[j][0] == '_' or buf[j] == '::'):
                    parts.append(buf[j])
                    j += 1
                nm = ''.join(parts)
                pre = scope_q()
                q = (pre + '::' + nm if pre and nm else nm or pre)
                if nm:
                    defs.append((nm.split('::')[-1], q, 'namespace', bufl[idx]))
                stack.append(('ns', q))
            elif kw and not has_paren and top in ('file', 'ns', 'class', 'enum'):
                idx = buf.index(kw)
                j = idx + 1
                if kw == 'enum' and j < len(buf) and buf[j] in ('class', 'struct'):
                    j += 1
                nm = ''
                while j < len(buf) and buf[j] in ('final', 'alignas'):
                    j += 1
                if j < len(buf) and (buf[j][0].isalpha() or buf[j][0] == '_') and buf[j] not in KEYWORDS:
                    nm = buf[j]
                pre = scope_q()
                q = (pre + '::' + nm) if pre and nm else (nm or pre)
                if nm:
                    defs.append((nm, q, kw if kw != 'enum' else 'enum', bufl[idx]))
                stack.append(('enum' if kw == 'enum' else 'class', q))
            elif top in ('file', 'ns', 'class') and has_paren and buf and buf[0] not in ('=',):
                q = decl_from(buf, bufl, '{')
                stack.append(('func', q or cur_func() or '?'))
            elif top == 'enum':
                stack.append(('block', ''))
            elif top in ('file', 'ns', 'class') and buf and buf[0] == 'extern':
                stack.append(('ns', stack[-1][1]))
            else:
                stack.append(('block', ''))
            buf, bufl = [], []
        elif t == '}':
            if top == 'enum' and buf:
                decl_from(buf, bufl, ',')
            if len(stack) > 1:
                stack.pop()
            buf, bufl = [], []
        elif t == ';':
            if top in ('file', 'ns', 'class', 'enum'):
                decl_from(buf, bufl, ';')
            buf, bufl = [], []
        else:
            buf.append(t)
            bufl.append(line)
            if top == 'enum' and t == ',':
                decl_from(buf[:-1], bufl[:-1], ',')
                buf, bufl = [], []
            elif t[0].isalpha() or t[0] == '_':
                if t not in KEYWORDS and len(t) >= 3:
                    nxt = toks[i + 1][0] if i + 1 < n else ''
                    j = i + 1
                    while j < n and toks[j][0] == '\n':
                        j += 1
                    nxt = toks[j][0] if j < n else ''
                    kind = 'call' if nxt == '(' else ('write' if nxt in ('=', '+=', '-=', '|=', '&=', '*=', '/=', '^=', '<<=', '>>=', '++', '--') else 'ref')
                    if kind == 'ref' and i >= 1 and toks[i - 1][0] in ('++', '--'):
                        kind = 'write'
                    qual = ''
                    if i >= 2 and toks[i - 1][0] == '::' and (toks[i - 2][0][0].isalpha() or toks[i - 2][0][0] == '_'):
                        qual = toks[i - 2][0]
                    elif i >= 1 and toks[i - 1][0] in ('->', '.'):
                        qual = '@member'       # x->Name( / x.Name(: the receiver's type is not known to a lexer index
                    add_ref(t, qual, kind, line)
        i += 1
    return defs, refs


def str_refs(text):
    """identifier-like string literals (cvar names given to GetFlagByName("x"), log tags) -> {name: [lines]}"""
    out = {}
    cm = qx5.mask_comments(text)
    for m in STR_ID.finditer(cm):
        out.setdefault(m.group(1), []).append(cm.count('\n', 0, m.start()) + 1)
    return out


def one_file(job):
    sdk, rel = job
    p = os.path.join(sdk, rel)
    text = qx5.read_text(p)
    try:
        defs, refs = lex_file(text)
    except Exception as e:   # a lexer bug in one file must not lose the others
        return rel, [], {}, {}, [], repr(e)
    cv = []
    cm = qx5.mask_comments(text)
    for m, ln in qx5.def_matches(cm):      # not the macro body of a #define
        cv.append((m.group(2), m.group(1).lower(), ln))
    return rel, defs, refs, str_refs(text), cv, ''


def build(root, jobs=8, sdk_arg=None, out=None):
    out = out or sys.stdout
    sdk = os.path.abspath(sdk_arg) if sdk_arg else qx5.sdk_root(root)
    if not sdk or not os.path.isdir(sdk):
        raise SystemExit('q sdk: ' + (f'--sdk {sdk_arg}: no such folder' if sdk_arg else 'thirdparty/rexglue-sdk-src not found (this checkout or the main one)'))
    path = db_path(root, sdk_arg)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    t0 = time.time()
    old = {}
    if os.path.exists(path):
        try:
            c0 = sqlite3.connect(path)
            if c0.execute("SELECT value FROM meta WHERE key='schema'").fetchone()[0] == str(SCHEMA):
                old = {r[0]: (r[1], r[2]) for r in c0.execute('SELECT path, size, mtime_ns FROM files')}
            c0.close()
        except sqlite3.Error:
            old = {}
    files = scan(sdk)
    jobs = safe_jobs(jobs)
    changed = [r for r, v in files.items() if old.get(r) != v]
    removed = [r for r in old if r not in files]
    tmp = path + '.tmp'
    if os.path.exists(tmp):
        os.remove(tmp)
    if old and os.path.exists(path):
        import shutil
        shutil.copyfile(path, tmp)
    con = sqlite3.connect(tmp)
    con.executescript('''
        CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);
        CREATE TABLE IF NOT EXISTS files(path TEXT PRIMARY KEY, size INTEGER, mtime_ns INTEGER);
        CREATE TABLE IF NOT EXISTS defs(name TEXT, qname TEXT, kind TEXT, file TEXT, line INTEGER, extra TEXT);
        CREATE TABLE IF NOT EXISTS refs(name TEXT, qual TEXT, func TEXT, kind TEXT, file TEXT, lines TEXT);
        CREATE INDEX IF NOT EXISTS defs_name ON defs(name);
        CREATE INDEX IF NOT EXISTS refs_name ON refs(name);
        CREATE INDEX IF NOT EXISTS defs_file ON defs(file);
        CREATE INDEX IF NOT EXISTS refs_file ON refs(file);''')
    for r in changed + removed:
        con.execute('DELETE FROM defs WHERE file = ?', (r,))
        con.execute('DELETE FROM refs WHERE file = ?', (r,))
        con.execute('DELETE FROM files WHERE path = ?', (r,))
    errs = []
    import concurrent.futures as cf
    with cf.ProcessPoolExecutor(max_workers=jobs) as ex:
        for rel, defs, refs, srefs, cv, err in ex.map(one_file, [(sdk, r) for r in changed], chunksize=8):
            if err:
                errs.append((rel, err))
            con.executemany('INSERT INTO defs VALUES(?,?,?,?,?,?)', [(d[0], d[1], d[2], rel, d[3], '') for d in defs] + [(n, n, 'cvar', rel, ln, ty) for n, ty, ln in cv])
            con.executemany('INSERT INTO refs VALUES(?,?,?,?,?,?)',
                            [(n, q, f, k, rel, ','.join(str(x) for x in sorted(set(ls))[:60])) for (n, q, f, k), ls in refs.items()]
                            + [(n, '', '', 'str', rel, ','.join(str(x) for x in sorted(set(ls))[:60])) for n, ls in srefs.items()])
            con.execute('INSERT OR REPLACE INTO files VALUES(?,?,?)', (rel, *files[rel]))
    con.execute("INSERT OR REPLACE INTO meta VALUES('schema', ?)", (str(SCHEMA),))
    con.execute("INSERT OR REPLACE INTO meta VALUES('built', ?)", (time.strftime('%Y-%m-%d %H:%M'),))
    con.execute("INSERT OR REPLACE INTO meta VALUES('built_ts', ?)", (str(int(time.time())),))
    con.execute("INSERT OR REPLACE INTO meta VALUES('sdk_root', ?)", (sdk,))
    con.execute("INSERT OR REPLACE INTO meta VALUES('sdk_head', ?)", (sdk_head(sdk) or '',))
    con.commit()
    n = (con.execute('SELECT COUNT(*) FROM defs').fetchone()[0], con.execute('SELECT COUNT(*) FROM refs').fetchone()[0])
    con.close()
    for attempt in range(4):             # WinError 5 / 32 while another q process holds the db open: retry, then raise
        try:
            os.replace(tmp, path)
            break
        except OSError:
            if attempt == 3:
                raise
            time.sleep(0.5)
    print(f'q sdk: {len(files)} files ({len(changed)} re-read, {len(removed)} removed), {n[0]} definitions, {n[1]} reference rows, {jobs} workers, {time.time() - t0:.1f} s -> {path}', file=out)
    for rel, e in errs[:5]:
        print(f'q sdk: lexer error in {rel}: {e}', file=sys.stderr)
    return 0


def acquire_lock(path, wait_s=None):
    """one writer at a time (a lock file next to the db; a lock older than 10 min is a dead writer's and is taken over). -> the lock path, or None when another writer still holds it after wait_s"""
    lk = path + '.lock'
    t0 = time.time()
    if wait_s is None:
        wait_s = float(os.environ.get('WWE_QSDK_LOCK_WAIT', '120'))
    while True:
        try:
            fd = os.open(lk, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, str(os.getpid()).encode())
            os.close(fd)
            return lk
        except FileExistsError:
            try:
                if time.time() - os.path.getmtime(lk) > 600:
                    os.remove(lk)
                    continue
            except OSError:
                pass
            if time.time() - t0 > wait_s:
                return None
            time.sleep(0.3)
        except OSError:
            return None


def ensure_fresh(root, sdk_arg=None, jobs=None):
    """-> (state, changed files, seconds): state 'fresh' | 'refreshed' | 'built' | 'STALE' (a writer holds the lock, or the refresh failed; answers come from the old index and say so). Builds a missing index."""
    sdk = os.path.abspath(sdk_arg) if sdk_arg else qx5.sdk_root(root)
    if not sdk or not os.path.isdir(sdk):
        return 'fresh', 0, 0.0
    path = db_path(root, sdk_arg)
    t0 = time.time()
    have = os.path.exists(path)
    old = None
    if have:
        try:
            c0 = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
            if c0.execute("SELECT value FROM meta WHERE key='schema'").fetchone()[0] == str(SCHEMA):
                old = {r[0]: (r[1], r[2]) for r in c0.execute('SELECT path, size, mtime_ns FROM files')}
            c0.close()
        except (sqlite3.Error, TypeError):
            old = None
    now = scan(sdk)
    if old is not None:
        stale = [r for r, v in now.items() if old.get(r) != v] + [r for r in old if r not in now]
        if not stale:
            return 'fresh', 0, time.time() - t0
    else:
        stale = list(now)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    lk = acquire_lock(path)
    if lk is None:
        return 'STALE', len(stale), time.time() - t0
    try:
        build(root, jobs or MAX_JOBS, sdk_arg, out=sys.stderr)
    except (SystemExit, Exception) as e:
        print(f'q sdk: the refresh failed: {e}', file=sys.stderr)
        return 'STALE', len(stale), time.time() - t0
    finally:
        try:
            os.remove(lk)
        except OSError:
            pass
    return ('refreshed' if have and old is not None else 'built'), len(stale), time.time() - t0


HEADER = ''      # the one-line index status every answer starts with (set by connect)


def age_text(secs):
    if secs < 90:
        return f'{int(secs)} s'
    if secs < 5400:
        return f'{int(secs // 60)} min'
    if secs < 172800:
        return f'{secs / 3600:.1f} h'
    return f'{secs / 86400:.1f} days'


def connect(root, sdk_arg=None):
    """refresh the index when it is missing / stale (automatic, a few seconds at worst), then open it read-only; sets HEADER"""
    global HEADER
    again = ' --sdk ' + sdk_arg if sdk_arg else ''
    state, nchg, secs = ensure_fresh(root, sdk_arg)
    p = db_path(root, sdk_arg)
    if not os.path.exists(p):
        raise SystemExit('q sdk: no index and the SDK source was not found (thirdparty/rexglue-sdk-src in this checkout or the main one)' + (f' ({sdk_arg})' if sdk_arg else ''))
    con = sqlite3.connect(f'file:{p}?mode=ro', uri=True)
    con.row_factory = sqlite3.Row
    m = dict(con.execute('SELECT key, value FROM meta').fetchall())
    if m.get('schema') != str(SCHEMA):
        raise SystemExit(f'q sdk: the index at {p} has schema {m.get("schema")}, this q needs {SCHEMA} and the automatic refresh could not replace it: run  py tools/q/q.py sdk update{again}')
    try:
        age = age_text(time.time() - int(m.get('built_ts', '0')))
    except ValueError:
        age = '?'
    nf = con.execute('SELECT COUNT(*) FROM files').fetchone()[0]
    if state == 'fresh':
        what = f'0 files changed since ({secs:.1f} s check)'
    elif state == 'STALE':
        what = f'WARNING STALE: {nchg} files changed and the refresh did not run (another writer, or it failed): answers may be out of date; py tools/q/q.py sdk update{again}'
    else:
        what = f'{state} just now: {nchg} files re-read in {secs:.1f} s'
    HEADER = f'[index built {m.get("built", "?")} ({age} ago), {nf} files; {what}]'
    return con


def split_name(arg):
    parts = [x for x in re.split(r'::|\.', arg.strip()) if x]
    return (parts[-1] if parts else ''), parts[:-1]


PART = None      # --part NAME: keep only the hits of one part (sdk, sdl3, imgui, ...)


def keep_part(rows):
    rows = [r for r in rows if PART is None or part_of(r['file']) == PART]
    return sorted(rows, key=lambda r: (part_of(r['file']) != 'sdk', r['file']))      # the SDK's own hits first, then the third-party parts


def find_defs(con, arg):
    tail, quals = split_name(arg)
    rows = keep_part(con.execute('SELECT * FROM defs WHERE name = ? ORDER BY file, line', (tail,)).fetchall())
    if quals:
        want = '::'.join(quals) + '::' + tail
        rows = [r for r in rows if r['qname'].endswith(want) or r['qname'] == tail]
    return rows


def cmd_def(con, arg):
    rows = find_defs(con, arg)
    return {'symbol': arg, 'defs': [dict(r) for r in rows]}


def cmd_refs(con, arg, calls_only=False):
    tail, quals = split_name(arg)
    q = 'SELECT * FROM refs WHERE name = ?' + (" AND kind = 'call'" if calls_only else '') + ' ORDER BY file, func'
    rows = keep_part(con.execute(q, (tail,)).fetchall())
    out = []
    for r in rows:
        d = dict(r)
        if not quals or r['qual'] == quals[-1] or r['func'].endswith(quals[-1] + '::' + tail) or r['func'].split('::')[0:-1][-1:] == [quals[-1]] or r['kind'] == 'str':
            out.append(d)
        elif r['qual'] == '@member':      # x->Name( / x.Name(: kept for a qualified query, the receiver's type is not known to a lexer index
            d['note'] = 'receiver type unknown'
            out.append(d)
    out.sort(key=lambda x: bool(x.get('note')))     # exact (qualified) rows first, the receiver-unknown member calls after
    return {'symbol': arg, 'refs': out, 'defs': [dict(r) for r in find_defs(con, arg)][:6]}


def print_def(r, args):
    ds = r['defs']
    print(f'sdk def {r["symbol"]}: {len(ds)} match(es)  {HEADER}')
    for d in ds[:args.limit * 2]:
        print(f'  {d["file"]}:{d["line"]}  [{part_of(d["file"])}] {d["kind"]:<11} {d["qname"]}' + (f'   (REXCVAR_DEFINE_{d["extra"].upper()})' if d['kind'] == 'cvar' else ''))


def print_refs(r, args, what='references'):
    rs = r['refs']
    total = sum(len(x['lines'].split(',')) for x in rs)
    unk = sum(1 for x in rs if x.get('note'))
    print(f'sdk {what} {r["symbol"]}: {total} lines in {len(rs)} places' + (f' ({len(rs) - unk} qualified / in the class, then {unk} through -> / . with the receiver type unknown)' if unk else '') + (f'; defined: {"; ".join(d["file"] + ":" + str(d["line"]) + " (" + d["kind"] + ")" for d in r["defs"][:4])}' if r['defs'] else '') + f'  {HEADER}')
    shown = 0
    for x in rs:
        ls = x['lines'].split(',')
        print(f'  {x["file"]}:{",".join(ls[:6])}{"..." if len(ls) > 6 else ""}  in {x["func"] or "<file scope>"}  [{part_of(x["file"])}] [{x["kind"]}{", " + x["note"] if x.get("note") else ""}]')
        shown += 1
        if shown >= args.limit * 2:
            print(f'  ... {len(rs) - shown} more (--limit)')
            break


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(prog='q sdk', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['update', 'def', 'refs', 'callers', 'info'])
    ap.add_argument('arg', nargs='?', default='')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--limit', type=int, default=30)
    ap.add_argument('-j', type=int, default=MAX_JOBS)
    ap.add_argument('--part', default=None, help='keep only one part: sdk (src + include), sdl3, imgui, vulkan-memory-allocator, spdlog, fmt, cli11, tomlplusplus, crypto, disasm, pe')
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--sdk', default=None, help='a thirdparty/sdk-wt/<name> tree instead of thirdparty/rexglue-sdk-src (its own index sdk-<name>.db in this checkout)')
    args = ap.parse_args(argv)
    global PART
    PART = args.part
    if args.cmd == 'update':
        sdk = os.path.abspath(args.sdk) if args.sdk else qx5.sdk_root(args.root)
        lk = acquire_lock(db_path(args.root, args.sdk)) if sdk else None
        try:
            return build(args.root, args.j, args.sdk)
        finally:
            if lk:
                try:
                    os.remove(lk)
                except OSError:
                    pass
    con = connect(args.root, args.sdk)
    if args.cmd == 'info':
        m = dict(con.execute('SELECT key, value FROM meta').fetchall())
        m['files'] = con.execute('SELECT COUNT(*) FROM files').fetchone()[0]
        m['defs'] = con.execute('SELECT COUNT(*) FROM defs').fetchone()[0]
        m['refs'] = con.execute('SELECT COUNT(*) FROM refs').fetchone()[0]
        parts = {}
        for (f,) in con.execute('SELECT path FROM files'):
            parts[part_of(f)] = parts.get(part_of(f), 0) + 1
        m['parts'] = ', '.join(f'{k} {v}' for k, v in sorted(parts.items()))
        m['status'] = HEADER
        other = other_index_files(db_path(args.root, args.sdk))      # S105 D: named here only, not in every answer's header
        if other:
            m['other_index_files'] = ', '.join(other) + ' (other schemas, left alone)'
        import json
        print(json.dumps(m, indent=1) if args.json else '\n'.join(f'{k}: {v}' for k, v in m.items()))
        return 0
    if not args.arg:
        print('q sdk: give a NAME', file=sys.stderr)
        return 2
    if args.cmd == 'def':
        r, pr = cmd_def(con, args.arg), print_def
    elif args.cmd == 'refs':
        r, pr = cmd_refs(con, args.arg), print_refs
    else:
        r, pr = cmd_refs(con, args.arg, True), lambda a, b: print_refs(a, b, 'callers')
    if args.json:
        import json
        r["index"] = HEADER
        print(json.dumps(r, indent=1, default=str))
    else:
        pr(r, args)
    return 0
