"""qm (S106 lane QM): the q misses of 2026-11-21, closed in one small module (no index needed: text / ast answers over this checkout's own files).

  closed pipe   `q ... | head` on Windows raises OSError [Errno 22] (not BrokenPipeError) on the next write: closed_pipe(e) / silence_stdout() / guarded(main)
                make every q entry point exit 0 quietly (qours.py, q.py, the `py` / `show` / `fact` dispatches).
  outline       `q ours outline FILE.py|.ps1`: functions, classes, methods with line-endline (ast; ps1: `function X {` by brace matching).
  runs          `q ours runs FILE|EXE`: which tools / config files mention it (file:line, marked `invoke` when the line launches it). Works for a file
                that is not in the repo (out/tools/a7_unit.exe).
  env           `q ours env NAME`: every read / set of an environment variable in py / ps1 / cpp / bat / sh / json / cmake files, line by line.
Selftest: tools/q/qm_selftest.py (fixture repo in a TEMP folder; a planted closed-pipe twin seen failing first).
"""
import ast
import errno
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PY_EXT = ('.py', '.ps1', '.psm1')
SKIP_DIRS = ('__pycache__', 'node_modules', '.git', 'out', 'thirdparty')
SCAN_TOPS = ('src', 'tools', 'probes', 'android', 'config', 'docs/agents')
MAX_BYTES = 1_500_000


# ---------------------------------------------------------------------------------------------------------------- closed pipe

def closed_pipe(e):
    """True when OSError e is a write to a pipe whose reader went away: BrokenPipeError, EPIPE, or Windows' EINVAL (Errno 22)."""
    return isinstance(e, BrokenPipeError) or (isinstance(e, OSError) and getattr(e, 'errno', None) in (errno.EINVAL, errno.EPIPE))


def silence_stdout():
    """stdout -> devnull (the Python object AND the OS handle, so the flush at interpreter exit cannot fail with exit code 120)."""
    try:
        sys.stdout = open(os.devnull, 'w')
    except OSError:
        pass
    try:
        fd = os.open(os.devnull, os.O_WRONLY)
        os.dup2(fd, 1)
        os.close(fd)
    except OSError:
        pass


def guarded(fn, *a, **k):
    """run fn; a closed stdout pipe (also at the final flush) ends quietly with 0; any other OSError is raised."""
    try:
        rc = fn(*a, **k)
        try:
            sys.stdout.flush()
        except OSError as e:
            if not closed_pipe(e):
                raise
            silence_stdout()
        return rc
    except OSError as e:
        if not closed_pipe(e):
            raise
        silence_stdout()
        return 0


# ---------------------------------------------------------------------------------------------------------------- outline

def py_outline(text):
    """-> [(line, endline, depth, kind, qname)] for a python file: module-level functions / classes and class members (nested defs inside functions are not listed)."""
    tree = ast.parse(text)
    out = []

    def visit(body, prefix, depth):
        for n in body:
            if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)):
                start = min([n.lineno] + [d.lineno for d in n.decorator_list])
                out.append((start, n.end_lineno, depth, 'def', prefix + n.name))
            elif isinstance(n, ast.ClassDef):
                start = min([n.lineno] + [d.lineno for d in n.decorator_list])
                out.append((start, n.end_lineno, depth, 'class', prefix + n.name))
                visit(n.body, prefix + n.name + '.', depth + 1)
            elif isinstance(n, (ast.If, ast.Try, ast.With)):   # `if __name__ == ...:` / try-import blocks hold defs too
                for fld in ('body', 'orelse', 'finalbody'):
                    visit(getattr(n, fld, []) or [], prefix, depth)
                for h in getattr(n, 'handlers', []) or []:
                    visit(h.body, prefix, depth)

    visit(tree.body, '', 0)
    out.sort()
    return out


PS_FN = re.compile(r'^\s*(?:filter|function)\s+([\w:.-]+)', re.I)


def ps1_outline(lines):
    import qtools
    out = []
    for i, ln in enumerate(lines, 1):
        m = PS_FN.match(ln)
        if m:
            out.append((i, qtools._ps1_end(lines, i), 0, 'function', m.group(1)))
    return out


def cmd_outline(root, spec):
    rel = spec.replace('\\', '/')
    r = root.replace('\\', '/').rstrip('/') + '/'
    if rel.startswith(r):
        rel = rel[len(r):]
    p = os.path.join(root, rel)
    if not os.path.isfile(p):
        import qtools
        got = qtools.find_file(root, spec)
        if not got or isinstance(got, list):
            raise ValueError(f'{spec}: ' + ('ambiguous: ' + ', '.join(got[:6]) if got else 'no such file'))
        rel, p = got, os.path.join(root, got)
    with open(p, encoding='utf-8', errors='replace') as f:
        text = f.read()
    lines = text.split('\n')
    note = ''
    if rel.lower().endswith('.py'):
        try:
            items = py_outline(text)
        except SyntaxError as e:
            items, note = [], f'(syntax error line {e.lineno}: no outline)'
    else:
        items = ps1_outline(lines)
    head = f'== {rel}  ({len(lines)} lines)  [{"python ast" if rel.lower().endswith(".py") else "ps1 function scan"}] {note}'.rstrip()
    out = [head]
    for a, b, d, k, q in items:
        out.append(f'{a:>6}-{b:<6} ' + '  ' * d + (f'class {q}' if k == 'class' else q))
    if not items and not note:
        out.append('(no functions or classes found)')
    return out


# ---------------------------------------------------------------------------------------------------------------- runs

def _norm(s):
    return s.replace('\\', '/').lower()


def _bound(name):
    return re.compile(r'(?<![A-Za-z0-9_.-])' + re.escape(name) + r'(?![A-Za-z0-9_-])', re.I)


def _norm_spec(root, spec):
    s = spec.replace('\\', '/')
    r = root.replace('\\', '/').rstrip('/') + '/'
    if s.startswith(r):
        s = s[len(r):]
    return s


def cmd_runs(root, spec):
    rel = _norm_spec(root, spec)
    base = rel.split('/')[-1]
    full_rx = re.compile(re.escape(rel).replace('/', r'[/\\]+'), re.I)
    base_rx = _bound(base)
    scripts = []
    skip_self = _norm(rel)
    for top in SCAN_TOPS + ('',):
        base_dir = os.path.join(root, top) if top else root
        if not os.path.isdir(base_dir):
            continue
        for dp, dn, fn in os.walk(base_dir):
            dn[:] = [x for x in dn if x not in SKIP_DIRS and not x.startswith('.')] if top else []
            for f in fn:
                if not f.endswith(('.py', '.ps1', '.psm1', '.bat', '.cmd', '.sh', '.txt', '.cmake', '.toml')) or (not top and not f.endswith(('.ps1', '.py', '.bat', '.cmake', '.txt'))):
                    continue
                p = os.path.join(dp, f)
                rp = os.path.relpath(p, root).replace('\\', '/')
                if _norm(rp) == skip_self:
                    continue
                try:
                    if os.path.getsize(p) > MAX_BYTES:
                        continue
                    with open(p, encoding='utf-8', errors='replace') as fh:
                        for i, ln in enumerate(fh, 1):
                            if full_rx.search(ln) or base_rx.search(ln.replace('\\', '/')):
                                code = not ln.lstrip().startswith(('#', '//', '"""', '*'))
                                kind = 'invoke' if code and (re.search(r'\b(py|python|pwsh|powershell|bash)\b[^#\n]*' + re.escape(base[:max(3, len(base) // 2)]), ln, re.I)
                                                             or re.search(r'^\s*[&.]\s*\S|Start-Process|subprocess|\bPopen\b|\bsystem\(|\bCreateProcess|-File\b|\bsh\s', ln)) else 'mentions'
                                scripts.append({'file': rp, 'line': i, 'kind': kind, 'text': ln.strip()[:150]})
                except OSError:
                    pass
    scripts.sort(key=lambda x: (x['file'], x['line']))
    return {'file': rel, 'exists': os.path.isfile(os.path.join(root, rel)), 'scripts': scripts}


def print_runs(r, args):
    print(f'{r["file"]}{"" if r["exists"] else " (not a file in this checkout: matched by name)"}: '
          f'{len(r["scripts"])} script lines mention it in {len({s["file"] for s in r["scripts"]})} files')
    seen = {}
    shown = 0
    for s in r['scripts']:
        seen[s['file']] = seen.get(s['file'], 0) + 1
        if seen[s['file']] <= 3 and shown < args.limit * 4:
            print(f'  {s["file"]}:{s["line"]}  [{s["kind"]}]  {s["text"]}')
            shown += 1
    if len(r['scripts']) > shown:
        print(f'  ... {len(r["scripts"]) - shown} more lines (--limit)')


# ---------------------------------------------------------------------------------------------------------------- env

ENV_EXT = ('.py', '.ps1', '.psm1', '.bat', '.cmd', '.sh', '.cpp', '.cc', '.h', '.hpp', '.inl', '.json', '.toml', '.cmake', '.txt', '.java', '.gradle')


def env_rx(name):
    n = re.escape(name)
    q = r'''['"]'''
    setp = [
        r'environ\s*\[\s*' + q + n + q + r'\s*\]\s*=(?!=)', r'environ\.setdefault\(\s*' + q + n, r'putenv\w*\(\s*[LU]?' + q + n, r'\benv\w*\[\s*' + q + n + q + r'\s*\]\s*=(?!=)',
        r'\$env:' + n + r'\s*\+?=(?!=)', r'SetEnvironmentVariable\w*\(\s*[LU]?' + q + n, r'setenv\(\s*' + q + n, r'Env:\\?' + n + r'\b.*Remove|Remove-Item\s+Env:\\?' + n,
        r'Environment\.(?:Remove|Add|Item)\w*\(\s*' + q + n, r'Environment\[\s*' + q + n + q + r'\s*\]\s*=(?!=)',
        r'^\s*@?set\s+"?' + n + r'=', r'^\s*export\s+' + n + r'=', r'^\s*' + n + r'=', r'\benv\w*\s*=\s*\{[^}]*' + q + n + q + r'\s*:', r'\bwith_env\b.*' + n,
        r'(?:env|ENV)\w*.*' + q + n + q + r'\s*:\s*',     # a dict literal handed to a child process as its env
    ]
    readp = [
        r'environ\.get\(\s*' + q + n, r'environ\s*\[\s*' + q + n + q + r'\s*\]', r'getenv\w*\(\s*[LU]?' + q + n, r'\$env:' + n + r'\b', r'GetEnvironmentVariable\w*\(\s*[LU]?' + q + n,
        r'%' + n + r'%', r'\$\{?' + n + r'\}?', q + n + q + r'\s+in\s+os\.environ', r'\benv\w*\.get\(\s*' + q + n, r'REX_ENV\w*\(\s*' + q + n,
    ]
    return re.compile(r'(?<![A-Za-z0-9_])' + n + r'(?![A-Za-z0-9_])'), [re.compile(p) for p in setp], [re.compile(p) for p in readp]


def cmd_env(root, name):
    word, setp, readp = env_rx(name)
    rows = []
    for top in SCAN_TOPS + ('',):
        base_dir = os.path.join(root, top) if top else root
        if not os.path.isdir(base_dir):
            continue
        for dp, dn, fn in os.walk(base_dir):
            dn[:] = [x for x in dn if x not in SKIP_DIRS and not x.startswith('.')] if top else []
            for f in fn:
                if not f.endswith(ENV_EXT) or (not top and not f.endswith(('.ps1', '.py', '.bat', '.cmake', '.txt', '.json'))):
                    continue
                p = os.path.join(dp, f)
                try:
                    if os.path.getsize(p) > MAX_BYTES or os.sep + 'spv' + os.sep in p:
                        continue
                    with open(p, encoding='utf-8', errors='replace') as fh:
                        for i, ln in enumerate(fh, 1):
                            if name in ln and word.search(ln):
                                if ln.lstrip().startswith(('#', '//', '*', '/*', 'REM ', '::')) and not f.endswith(('.txt', '.cmake')):
                                    kind = 'ref'    # a comment line documents the variable; it neither reads nor sets it
                                else:
                                    kind = 'set' if any(r.search(ln) for r in setp) else ('read' if any(r.search(ln) for r in readp) else 'ref')
                                rows.append({'file': os.path.relpath(p, root).replace('\\', '/'), 'line': i, 'kind': kind, 'text': ln.strip()[:150]})
                except OSError:
                    pass
    rows.sort(key=lambda x: (x['kind'], x['file'], x['line']))
    return {'name': name, 'rows': rows}


def print_env(r, args):
    rows = r['rows']
    cnt = {k: sum(1 for x in rows if x['kind'] == k) for k in ('set', 'read', 'ref')}
    print(f'{r["name"]}: {len(rows)} lines in {len({x["file"] for x in rows})} files: {cnt["set"]} set, {cnt["read"]} read, {cnt["ref"]} other mention (docs / json / unclassified)')
    for k in ('set', 'read', 'ref'):
        sel = [x for x in rows if x['kind'] == k]
        if not sel:
            continue
        print(f'  {k}:')
        for x in sel[:args.limit * 3]:
            print(f'    {x["file"]}:{x["line"]}  {x["text"]}')
        if len(sel) > args.limit * 3:
            print(f'    ... {len(sel) - args.limit * 3} more {k} lines (--limit)')
    if not rows:
        print('  (no line names it: py / ps1 / cpp / bat / sh / json / cmake files under src, tools, probes, android, config)')


def free_ram_gb():
    """free physical RAM in GB (Windows), or None when it cannot be read (the callers then use their fixed cap)"""
    try:
        import ctypes

        class MS(ctypes.Structure):
            _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong), ('ullTotalPhys', ctypes.c_ulonglong),
                        ('ullAvailPhys', ctypes.c_ulonglong), ('ullTotalPageFile', ctypes.c_ulonglong), ('ullAvailPageFile', ctypes.c_ulonglong),
                        ('ullTotalVirtual', ctypes.c_ulonglong), ('ullAvailVirtual', ctypes.c_ulonglong), ('ullAvailExtendedVirtual', ctypes.c_ulonglong)]
        m = MS()
        m.dwLength = ctypes.sizeof(MS)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
        return m.ullAvailPhys / 2**30
    except Exception:    # noqa: BLE001
        return None


# ---------------------------------------------------------------------------------------------------------------- entry (called from qours.text_cmd)

def run(args):
    if args.cmd == 'outline':
        try:
            lines = cmd_outline(args.root, args.arg)
        except ValueError as e:
            raise RuntimeError(str(e))
        print(json.dumps({'cmd': 'outline', 'lines': lines}, indent=1) if args.json else '\n'.join(lines))
        return 0
    if args.cmd == 'runs':
        r, pr = cmd_runs(args.root, args.arg), print_runs
    else:
        r, pr = cmd_env(args.root, args.arg), print_env
    if args.json:
        print(json.dumps(r, indent=1, default=str))
    else:
        pr(r, args)
    return 0
