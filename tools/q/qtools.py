"""qtools: `q ours` for OUR scripts (tools\\ python + ps1 + gate lane json), S103 QX2.

    py tools/q/q.py ours def|refs|callers|callees|fn NAME      NAME = a python / ps1 function, class, variable, attribute, parameter, or a script
                                                                (gate.py, gate_wait.main, Deleter.unlink, $RunDir); falls back to this index when the
                                                                C++ index has no such symbol; --tools asks for it first
    py tools/q/q.py ours covers tools/gate/gate.py             gate lanes / checks that name the file (affects, needs, cmd) + scripts that name it
    py tools/q/q.py ours lanes-of SCRIPT                       the same, one line per check with its command, and what the script invokes / who invokes it

No persistent index: tools\\ is ~1,600 files (parse of all of it 2-3 s); a per-checkout cache (out/harness/index/qtools_cache.pkl, size + mtime per
file) re-parses only files that changed, so the answer is always THIS checkout's (a worktree's own edits included, no staleness).
python: ast (defs, params, assignments incl. self.x, calls, reads, imports, string constants, regexes passed to re.*); ps1: regex (functions,
param() names, $variables, string literals, invoked scripts). Rows are grouped by file with the source line shown.
"""
import ast
import os
import pickle
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SKIP_DIRS = ('__pycache__', 'node_modules', '.git')
SCRIPT_TOK = re.compile(r'[\w$.:\\/-]*?[\w-]+\.(?:ps1|py|exe|bat|cmd)\b', re.I)
RE_FUNCS = {'compile', 'search', 'match', 'fullmatch', 'findall', 'finditer', 'sub', 'subn', 'split'}
RX_LIKE = re.compile(r'\\[\[\]dswbDSW.(){}]|\(\?|\.\*|\[\^?[\w\\-]+\][+*?]|\\d')
CACHE_VER = 3


def base_script(tok):
    t = tok.replace('\\', '/').split('/')[-1]
    return t.lower()


# ---------------------------------------------------------------- python

class PyScan(ast.NodeVisitor):
    def __init__(self, text):
        self.defs, self.refs, self.strs, self.rxs, self.invs = [], [], [], [], []
        self.scope = []
        self.cls = []
        self.globals = set()

    def q(self, n):
        return '.'.join(self.scope + [n])

    def fn(self):
        return '.'.join(self.scope) if self.scope else '<module>'

    def head(self, node):
        if node is None:
            return ''
        try:
            return ast.unparse(node)[:90]
        except Exception:
            return ''

    def visit_FunctionDef(self, n):
        self.defs.append((self.q(n.name), n.name, 'method' if self.cls and self.scope and self.scope[-1] == self.cls[-1] else 'function', n.lineno, getattr(n, 'end_lineno', n.lineno)))
        self.scope.append(n.name)
        for a in n.args.posonlyargs + n.args.args + n.args.kwonlyargs + ([n.args.vararg] if n.args.vararg else []) + ([n.args.kwarg] if n.args.kwarg else []):
            self.defs.append((self.q(a.arg), a.arg, 'param', a.lineno, a.lineno))
            self.refs.append((a.arg, 'write', a.lineno, self.fn()))
        for d in n.decorator_list:
            self.visit(d)
        for dflt in n.args.defaults + [x for x in n.args.kw_defaults if x]:
            self.visit(dflt)
        for st in n.body:
            self.visit(st)
        self.scope.pop()
    visit_AsyncFunctionDef = visit_FunctionDef

    def visit_ClassDef(self, n):
        self.defs.append((self.q(n.name), n.name, 'class', n.lineno, getattr(n, 'end_lineno', n.lineno)))
        for b in n.bases:
            self.visit(b)
        self.scope.append(n.name)
        self.cls.append(n.name)
        for st in n.body:
            self.visit(st)
        self.cls.pop()
        self.scope.pop()

    def visit_Global(self, n):
        self.globals.update(n.names)

    def target(self, t, value_node):
        value_text = None
        if isinstance(t, ast.Name):
            self.refs.append((t.id, 'write', t.lineno, self.fn()))
            if not self.scope or t.id in self.globals or (self.cls and self.scope == self.cls):
                value_text = self.head(value_node)
                self.defs.append((self.q(t.id) if not t.id in self.globals else t.id, t.id, 'variable', t.lineno, t.lineno, value_text))
        elif isinstance(t, ast.Attribute):
            self.refs.append((t.attr, 'write', t.lineno, self.fn()))
            if isinstance(t.value, ast.Name) and t.value.id in ('self', 'cls') and self.cls:
                value_text = self.head(value_node)
                self.defs.append((f'{self.cls[-1]}.{t.attr}', t.attr, 'attr', t.lineno, t.lineno, value_text))
            self.visit(t.value)
        elif isinstance(t, (ast.Tuple, ast.List)):
            for e in t.elts:
                self.target(e, value_node)
        elif isinstance(t, ast.Subscript):
            self.visit(t.value)
            self.visit(t.slice)
        elif isinstance(t, ast.Starred):
            self.target(t.value, value_node)

    def visit_Assign(self, n):
        for t in n.targets:
            self.target(t, n.value)
        self.visit(n.value)

    def visit_AnnAssign(self, n):
        self.target(n.target, n.value)
        if n.value:
            self.visit(n.value)

    def visit_AugAssign(self, n):
        self.target(n.target, n.value)
        self.visit(n.value)

    def visit_NamedExpr(self, n):
        self.target(n.target, n.value)
        self.visit(n.value)

    def visit_For(self, n):
        self.target(n.target, None)
        self.generic_visit(n)

    def visit_With(self, n):
        for it in n.items:
            if it.optional_vars is not None:
                self.target(it.optional_vars, it.context_expr)
        self.generic_visit(n)

    def visit_Import(self, n):
        for a in n.names:
            nm = (a.asname or a.name).split('.')[0]
            self.defs.append((self.q(nm), nm, 'import', n.lineno, n.lineno, a.name))
            self.refs.append((a.name.split('.')[-1], 'import', n.lineno, self.fn()))

    def visit_ImportFrom(self, n):
        for a in n.names:
            nm = a.asname or a.name
            self.defs.append((self.q(nm), nm, 'import', n.lineno, n.lineno, f'{n.module}.{a.name}'))
            self.refs.append((a.name, 'import', n.lineno, self.fn()))
        if n.module:
            self.refs.append((n.module.split('.')[-1], 'import', n.lineno, self.fn()))

    def visit_Call(self, n):
        f = n.func
        if isinstance(f, ast.Name):
            self.refs.append((f.id, 'call', n.lineno, self.fn()))
        elif isinstance(f, ast.Attribute):
            self.refs.append((f.attr, 'call', n.lineno, self.fn()))
            if f.attr in RE_FUNCS and isinstance(f.value, ast.Name) and f.value.id == 're' and n.args:
                a0 = n.args[0]
                if isinstance(a0, ast.Constant) and isinstance(a0.value, str):
                    self.rxs.append((a0.lineno, a0.value))
            self.visit(f.value)
        else:
            self.visit(f)
        for a in n.args + [k.value for k in n.keywords]:
            self.visit(a)

    def visit_Name(self, n):
        if isinstance(n.ctx, ast.Load):
            self.refs.append((n.id, 'read', n.lineno, self.fn()))

    def visit_Attribute(self, n):
        if isinstance(n.ctx, ast.Load):
            self.refs.append((n.attr, 'read', n.lineno, self.fn()))
        self.visit(n.value)

    def visit_Constant(self, n):
        if isinstance(n.value, str) and len(n.value) >= 4:
            v = n.value[:600]
            self.strs.append((n.lineno, v))
            if SCRIPT_TOK.search(v) and len(v) < 400:
                for m in SCRIPT_TOK.finditer(v):
                    self.invs.append((base_script(m.group(0)), n.lineno))


def scan_py(text):
    sc = PyScan(text)
    try:
        import warnings
        with warnings.catch_warnings():
            warnings.simplefilter('ignore')
            tree = ast.parse(text)
        sc.visit(tree)
    except (SyntaxError, ValueError, RecursionError):
        for i, ln in enumerate(text.split('\n'), 1):
            m = re.match(r'\s*(?:def|class)\s+(\w+)', ln)
            if m:
                sc.defs.append((m.group(1), m.group(1), 'function', i, i))
    rec = {'defs': [d + ('',) if len(d) == 5 else d for d in sc.defs], 'refs': sc.refs, 'strs': sc.strs, 'rxs': sc.rxs, 'invs': sc.invs}
    return rec


# ---------------------------------------------------------------- ps1

def scan_ps1(text):
    defs, refs, strs, invs = [], [], [], []
    fnames = set()
    lines = text.split('\n')
    for i, ln in enumerate(lines, 1):
        m = re.match(r'\s*function\s+([\w:-]+)', ln, re.I)
        if m:
            defs.append((m.group(1), m.group(1), 'function', i, i, ''))
            fnames.add(m.group(1).lower())
    m = re.search(r'(?im)^\s*param\s*\(', text)
    if m:
        depth, j, start = 0, m.end() - 1, m.end() - 1
        while j < len(text):
            c = text[j]
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        block = text[start:j]
        base_line = text.count('\n', 0, start) + 1
        for pm in re.finditer(r'\$(\w+)\s*(?:=|,|\)|$)', block, re.M):
            ln = base_line + block.count('\n', 0, pm.start())
            defs.append((pm.group(1), pm.group(1), 'param', ln, ln, ''))
    for i, ln in enumerate(lines, 1):
        for vm in re.finditer(r'\$(?:\w+:)?(\w+)(\s*(?:\+|-|\*)?=(?!=))?', ln):
            refs.append((vm.group(1), 'write' if vm.group(2) else 'read', i, ''))
        for w in re.finditer(r'[A-Za-z][\w-]+', ln):
            if w.group(0).lower() in fnames and not re.match(r'\s*function\s', ln, re.I):
                refs.append((w.group(0), 'call', i, ''))
        for sm in re.finditer(r"'([^'\n]{4,400})'|\"([^\"\n]{4,400})\"", ln):
            strs.append((i, sm.group(1) or sm.group(2)))
        for tm in SCRIPT_TOK.finditer(ln):
            invs.append((base_script(tm.group(0)), i))
    return {'defs': defs, 'refs': refs, 'strs': strs, 'rxs': [], 'invs': invs}


# ---------------------------------------------------------------- cache + load

def _cache_path(root):
    return os.path.join(root, 'out', 'harness', 'index', 'qtools_cache.pkl')


def list_files(root):
    out = []
    for top in ('tools', 'probes', 'src', 'android', 'assets'):
        for dp, dn, fn in os.walk(os.path.join(root, top)):
            dn[:] = [d for d in dn if d not in SKIP_DIRS and not d.startswith('.')]
            for f in fn:
                if f.endswith(('.py', '.ps1')):
                    out.append(os.path.join(dp, f))
    return out


def load(root=ROOT, budget=None):
    """-> {rel: record}; every .py / .ps1 of tools, probes, src, android, assets (parsed fresh when size / mtime differ from the cache)."""
    cp = _cache_path(root)
    cache = {}
    try:
        with open(cp, 'rb') as f:
            blob = pickle.load(f)
        if blob.get('ver') == CACHE_VER:
            cache = blob['files']
    except Exception:
        cache = {}
    out, dirty = {}, False
    t0 = time.time()
    for p in list_files(root):
        rel = os.path.relpath(p, root).replace('\\', '/')
        try:
            st = os.stat(p)
        except OSError:
            continue
        key = (st.st_size, st.st_mtime_ns)
        e = cache.get(rel)
        if e and e['key'] == key:
            out[rel] = e
            continue
        try:
            with open(p, encoding='utf-8', errors='replace') as fh:
                text = fh.read()
        except OSError:
            continue
        rec = scan_py(text) if rel.endswith('.py') else scan_ps1(text)
        rec['key'] = key
        rec['n'] = text.count('\n') + 1
        out[rel] = rec
        dirty = True
    if dirty or len(cache) != len(out):
        tmp = cp + f'.{os.getpid()}.tmp'
        try:
            os.makedirs(os.path.dirname(cp), exist_ok=True)
            with open(tmp, 'wb') as f:
                pickle.dump({'ver': CACHE_VER, 'files': out}, f, protocol=4)
            for attempt in (0, 1):             # WinError 5 when another q process holds the cache open: one retry, then give up
                try:
                    os.replace(tmp, cp)
                    break
                except OSError:
                    if attempt:
                        raise
                    time.sleep(0.3)
        except OSError:
            try:
                os.remove(tmp)                 # never leave a 21 MB .tmp behind
            except OSError:
                pass
    return out


_SRC_CACHE = {}


def src_line(root, rel, n):
    if rel not in _SRC_CACHE:
        try:
            with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
                _SRC_CACHE[rel] = f.read().split('\n')
        except OSError:
            _SRC_CACHE[rel] = []
    ls = _SRC_CACHE[rel]
    return ls[n - 1].strip()[:150] if 0 < n <= len(ls) else ''


# ---------------------------------------------------------------- queries

def resolve(idx, name):
    """name -> (matching defs [(rel, def)], name tail, restrict-to-file or None). `file.func` / `Class.method` / `module.func` forms work."""
    name = name.strip().replace('\\', '/')
    tail = re.split(r'[./]', name)[-1] if not name.endswith(('.py', '.ps1')) else name.split('/')[-1]
    onlyfile = None
    base = name.split('/')[-1]
    if base.endswith(('.py', '.ps1')):
        return [], base, None
    parts = name.replace('::', '.').split('.')
    tail = parts[-1]
    out = []
    for rel, rec in idx.items():
        for d in rec['defs']:
            if d[1] == tail and d[2] != 'import':
                q = d[0]
                if len(parts) == 1 or q.endswith('.'.join(parts[-2:])) or os.path.splitext(os.path.basename(rel))[0] == parts[-2] or q == name or q.endswith(name):
                    out.append((rel, d))
    return out, tail, onlyfile


def _row(root, rel, line, extra=''):
    return f'{rel}:{line}  {extra}'.rstrip()


def cmd_def(idx, name, args, root):
    defs, tail, _ = resolve(idx, name)
    base = name.replace('\\', '/').split('/')[-1]
    files = [r for r in idx if r.endswith('/' + base) or r == base] if base.endswith(('.py', '.ps1')) else []
    if not defs and not files:
        return None
    return {'symbol': name, 'matches': [{'file': r, 'qname': d[0], 'kind': d[2], 'line': d[3], 'endline': d[4], 'value': d[5], 'text': src_line(root, r, d[3])}
                                         for r, d in sorted(defs, key=lambda x: (x[0], x[1][3]))],
            'files': [{'file': f, 'lines': idx[f]['n']} for f in sorted(files)]}


def print_def(r, args):
    for m in r['matches'][:args.limit]:
        end = f'-{m["endline"]}' if m['endline'] and m['endline'] != m['line'] and m['kind'] in ('function', 'method', 'class') else ''
        print(f'{m["kind"]:<9} {m["qname"]}  {m["file"]}:{m["line"]}{end}   {m["text"][:110]}')
    for f in r['files']:
        print(f'script    {f["file"]}  ({f["lines"]} lines)')
    if len(r['matches']) > args.limit:
        print(f'  ... {len(r["matches"]) - args.limit} more (--limit)')


def ps_sources(text, defining):
    """True when a ps1 text dot-sources / imports one of the defining files (`. lib.ps1` in any path form, Import-Module lib, using module lib)"""
    for rel in defining:
        base = os.path.basename(rel)
        stem = os.path.splitext(base)[0]
        if re.search(r'(?im)^\s*(?:\.\s+|Import-Module\s+|using\s+module\s+)[^\n#]*' + re.escape(stem) + r'(?![\w-])', text):
            return True
    return False


def ps_mask(ln):
    """one ps1 line with the text inside quotes blanked and an unquoted comment (a `#` at a token start) cut off: what is left is code"""
    out, q, i, n = [], '', 0, len(ln)
    while i < n:
        c = ln[i]
        if q:
            if c == '`' and q == '"':
                out.append('  ')
                i += 2
                continue
            if c == q:
                q = ''
                out.append(c)
            else:
                out.append(' ')
        elif c in '"\'':
            q = c
            out.append(c)
        elif c == '#' and (i == 0 or ln[i - 1] in ' \t;({|,='):
            break
        else:
            out.append(c)
        i += 1
    return ''.join(out)


def cmd_refs(idx, name, args, root, kinds=None):
    defs, tail, _ = resolve(idx, name)
    base = name.replace('\\', '/').split('/')[-1].lower()
    is_script = base.endswith(('.py', '.ps1'))
    sites = {}
    parts = name.replace('::', '.').split('.')
    allowed = None
    if defs and len(parts) > 1 and not is_script:       # `gate_wait.main`: only the defining file(s) and the files that import it
        stems = {os.path.splitext(os.path.basename(r))[0] for r, d in defs}
        allowed = {r for r, d in defs} | {r for r, rec in idx.items() if any(k == 'import' and n_ in stems for n_, k, ln, fn in rec['refs'])}
    for rel, rec in idx.items():
        if allowed is not None and rel not in allowed:
            continue
        if is_script:
            for t, ln in rec['invs']:
                if t == base:
                    sites.setdefault((rel, '<invokes>'), {'lines': [], 'kinds': set()})['lines'].append(ln)
                    sites[(rel, '<invokes>')]['kinds'].add('invoke')
            mod = base.rsplit('.', 1)[0]
            for n_, k, ln, fn in rec['refs']:
                if k == 'import' and n_ == mod:
                    e = sites.setdefault((rel, fn or '<module>'), {'lines': [], 'kinds': set()})
                    e['lines'].append(ln)
                    e['kinds'].add('import')
            continue
        for n_, k, ln, fn in rec['refs']:
            if n_ == tail and (kinds is None or k in kinds):
                e = sites.setdefault((rel, fn or '<module>'), {'lines': [], 'kinds': set()})
                e['lines'].append(ln)
                e['kinds'].add(k)
    ps_defs = [r for r, d in defs if r.endswith('.ps1') and d[2] == 'function']
    if ps_defs and not is_script and (kinds is None or 'call' in kinds):
        # QX5: a ps1 function is called from OTHER scripts. Only scripts that dot-source / import the defining file count (same-named functions elsewhere are other functions;
        # this is also the `allowed` set of a qualified name), only in COMMAND position (statement start, or after | ( { = ; &) outside quotes and comments.
        wb = re.compile(r'(?:^|[|({=;&])\s*' + re.escape(tail) + r'(?![\w-])', re.I)
        for rel in idx:
            if rel in ps_defs or not rel.endswith('.ps1'):
                continue
            try:
                with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
                    text = f.read()
            except OSError:
                continue
            if tail.lower() not in text.lower() or not ps_sources(text, ps_defs):
                continue
            for i, ln in enumerate(text.split('\n'), 1):
                if re.match(r'\s*function\s', ln, re.I) or not wb.search(ps_mask(ln)):
                    continue
                e = sites.setdefault((rel, '<script>'), {'lines': [], 'kinds': set()})
                e['lines'].append(i)
                e['kinds'].add('call')
    if not sites and not defs:
        return None
    out = []
    for (rel, fn), e in sorted(sites.items()):
        ls = sorted(set(e['lines']))
        out.append({'file': rel, 'func': fn, 'lines': ls, 'kinds': sorted(e['kinds']), 'text': src_line(root, rel, ls[0])})
    return {'symbol': name, 'defs': [f'{r}:{d[3]} ({d[2]} {d[0]})' for r, d in defs[:8]], 'sites': out, 'n': sum(len(s['lines']) for s in out)}


def print_refs(r, args):
    print(f'{r["symbol"]}: {r["n"]} references in {len(r["sites"])} places' + (f'; defined: {"; ".join(r["defs"][:4])}' if r['defs'] else ''))
    for s in r['sites'][:args.limit]:
        ls = ','.join(str(x) for x in s['lines'][:6]) + ('...' if len(s['lines']) > 6 else '')
        print(f'  {s["file"]}:{ls}  in {s["func"]}  [{"/".join(s["kinds"])}]   {s["text"][:100]}')
    if len(r['sites']) > args.limit:
        print(f'  ... {len(r["sites"]) - args.limit} more (--limit)')


def cmd_callers(idx, name, args, root):
    r = cmd_refs(idx, name, args, root, kinds=('call', 'import', 'read') if not name.endswith(('.py', '.ps1')) else None)
    if not r:
        return None
    r['sites'] = [s for s in r['sites'] if set(s['kinds']) & {'call', 'invoke', 'import', 'read'}]
    return r


def cmd_callees(idx, name, args, root):
    defs, tail, _ = resolve(idx, name)
    if not defs:
        return None
    res = []
    for rel, d in defs:
        if d[2] not in ('function', 'method'):
            continue
        calls = {}
        for n_, k, ln, fn in idx[rel]['refs']:
            if k == 'call' and (fn == d[0] or fn.startswith(d[0] + '.')) and d[3] <= ln <= d[4]:
                calls.setdefault(n_, []).append(ln)
        res.append({'file': rel, 'qname': d[0], 'line': d[3], 'calls': [{'name': k, 'n': len(v), 'first': v[0]} for k, v in sorted(calls.items(), key=lambda kv: kv[1][0])]})
    return {'symbol': name, 'matches': res} if res else None


def print_callees(r, args):
    for m in r['matches']:
        print(f'{m["qname"]}  {m["file"]}:{m["line"]}  calls {len(m["calls"])}:')
        print('  ' + ', '.join(f'{c["name"]}{" x" + str(c["n"]) if c["n"] > 1 else ""}' for c in m['calls'][:40]))


def cmd_fn(idx, name, args, root):
    d = cmd_def(idx, name, args, root)
    if not d or not d['matches']:
        return d
    d['callees'] = (cmd_callees(idx, name, args, root) or {'matches': []})['matches']
    cl = cmd_refs(idx, name, args, root, kinds=('call', 'read', 'import'))
    d['callers'] = cl['sites'] if cl else []
    return d


def print_fn(r, args):
    print_def(r, args)
    print(f'  callers / users ({len(r["callers"])}):')
    for s in r['callers'][:10]:
        print(f'    {s["file"]}:{s["lines"][0]}  in {s["func"]}   {s["text"][:90]}')
    for m in r['callees']:
        print(f'  {m["qname"]} calls: ' + ', '.join(c['name'] for c in m['calls'][:30]))


DISPATCH = {'def': (cmd_def, print_def), 'refs': (cmd_refs, print_refs), 'callers': (cmd_callers, print_refs), 'callees': (cmd_callees, print_callees),
            'fn': (cmd_fn, print_fn), 'writes': (lambda i, n, a, r: cmd_refs(i, n, a, r, kinds=('write',)), print_refs),
            'reads': (lambda i, n, a, r: cmd_refs(i, n, a, r, kinds=('read', 'call')), print_refs)}


def looks_scripty(root, name, cmd):
    """`gate.dtab`, `gdeps.Table.idx`, `build_ours.main`, `gate_wait.py`: a dotted name whose qualifier is a script / class of the tools index asks the tools index first."""
    if cmd not in DISPATCH or '::' in name or '.' not in name:
        return False
    parts = name.strip().split('.')
    if name.endswith(('.py', '.ps1')):
        return True
    idx = load(root)
    q = parts[-2]
    return any(os.path.splitext(os.path.basename(r))[0] == q or any(d[1] == q and d[2] == 'class' for d in rec['defs']) for r, rec in idx.items())


def answer(cmd, name, args, root=ROOT):
    """-> (result, printer) or None when the tools index has nothing for NAME."""
    if cmd not in DISPATCH:
        return None
    idx = load(root)
    fn, pr = DISPATCH[cmd]
    r = fn(idx, name, args, root)
    return (r, pr) if r else None


# ---------------------------------------------------------------- covers / lanes-of

def find_file(root, spec):
    """a repo-relative path, an absolute one, or a unique basename -> repo-relative path of an existing file under tools / probes / src, else None."""
    spec = spec.replace('\\', '/')
    r = root.replace('\\', '/').rstrip('/') + '/'
    if spec.startswith(r):
        spec = spec[len(r):]
    if os.path.isfile(os.path.join(root, spec)):
        return spec
    hits = []
    base = spec.split('/')[-1]
    for top in ('tools', 'probes', 'src'):
        for dp, dn, fn in os.walk(os.path.join(root, top)):
            dn[:] = [d for d in dn if d not in SKIP_DIRS]
            if base in fn:
                hits.append(os.path.relpath(os.path.join(dp, base), root).replace('\\', '/'))
    hits = [h for h in hits if h.endswith(spec)]
    return hits[0] if len(hits) == 1 else (None if not hits else hits)


def cmd_lanes_of(rel, root, lanes_for, load_lanes, scripts_naming):
    idx = load(root)
    base = rel.split('/')[-1].lower()
    rows = []
    for lane, d in load_lanes(root):
        for c in d.get('checks', []):
            hs = lanes_for_one(lane, d, c, rel)
            if hs:
                cmdtxt = ' '.join(str(x) for x in c.get('cmd', [])) if isinstance(c.get('cmd'), list) else str(c.get('cmd', ''))
                rows.append({'lane': lane[:-5], 'check': c.get('name'), 'via': hs, 'cmd': cmdtxt[:160], 'retired': bool(c.get('retired') or d.get('retired'))})
    invokes = sorted({t for t, ln in idx.get(rel, {'invs': []})['invs']})
    invoked_by = sorted(r for r, rec in idx.items() if any(t == base for t, ln in rec['invs']) and r != rel)
    return {'file': rel, 'checks': rows, 'invokes': invokes, 'invoked_by': invoked_by, 'scripts_naming_it': scripts_naming(root, rel)}


def lanes_for_one(lane, d, c, rel):
    import qours
    via = []
    top = d.get('affects', [])
    ps = [p for p in list(c.get('affects', [])) + list(top) if isinstance(p, str) and qours.pat_match(p, rel)]
    if ps:
        via.append('affects' if any(p.replace('\\', '/') == rel for p in ps) else 'affects-glob')
    for key in ('needs', 'exes', 'programs'):
        for p in c.get(key, []) or []:
            if isinstance(p, str) and qours.pat_match(p, rel):
                via.append(key)
                break
    cmdtxt = ' '.join(str(x) for x in c.get('cmd', [])) if isinstance(c.get('cmd'), list) else str(c.get('cmd', ''))
    if re.search(re.escape(rel).replace('/', r'[/\\]+'), cmdtxt):
        via.append('cmd')
    return via


def print_lanes_of(r, args):
    named = [h for h in r['checks'] if h['via'] != ['affects-glob']]
    globs = [h for h in r['checks'] if h['via'] == ['affects-glob']]
    print(f'{r["file"]}: {len(named)} gate checks name it in {len({h["lane"] for h in named})} lanes; {len(globs)} more match only by an affects glob')
    for h in named[:args.limit]:
        print(f'  {h["lane"]} / {h["check"]}  via {"+".join(h["via"])}{"  [RETIRED]" if h["retired"] else ""}   {h["cmd"][:110]}')
    if globs:
        gl = sorted({h['lane'] for h in globs})
        print(f'  by glob only: lanes {", ".join(gl[:14])}{" ..." if len(gl) > 14 else ""}')
    if r['invokes']:
        print(f'  it invokes: {", ".join(r["invokes"][:14])}{" ..." if len(r["invokes"]) > 14 else ""}')
    if r['invoked_by']:
        print(f'  invoked / named by scripts ({len(r["invoked_by"])}): {", ".join(r["invoked_by"][:10])}{" ..." if len(r["invoked_by"]) > 10 else ""}')
    if r['scripts_naming_it']:
        print(f'  files that mention its path ({len(r["scripts_naming_it"])}): {", ".join(r["scripts_naming_it"][:8])}{" ..." if len(r["scripts_naming_it"]) > 8 else ""}')
