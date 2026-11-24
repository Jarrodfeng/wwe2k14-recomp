"""q find (S108 lane FD): q's front door. One call tries every way a name / word / path / address can be answered and prints what matched,
one labelled block per kind; kinds with no hit are named once on the closing line. No sdk refresh or game-index rebuild: the indexes as
cached. Two self-refreshing caches can still run: the fact library index (rebuilt, ~1-2 s, when a doc changed since it was built) and, for a
log-line question only, the tools python / ps1 cache that `q ours line` reads (files whose text changed are parsed).

    py tools/q/q.py find X [X2 ...] [--in PATH] [--all] [--limit N] [--json]
    py tools/q/q.py X ...                  (an unknown first word = q find X ...)

Each X is one question ("a phrase" in quotes = one question). What is tried (docs/research/s108/FD_plan.md):
  FILE:N-M            the lines (q show)
  C++ symbol          text: the definition-shaped lines of the name and its call sites (tools/q/qsym.py: whole word, case-sensitive; no index)
  python / ps1 name   text level: `def X(` / `class X` / module-level `X =` / ps1 `function X`, and the lines using it (exact: q py refs X)
  file name / stem    git ls-files: the file, and who invokes / imports / #includes / names it
  path literal        X with / or \\: the lines holding it (/ and \\ match as one)
  cvar / env var      REXCVAR_DEFINE_*(X, ...) lines; environment reads / sets of X
  log line            "[tag] words": the printf / Log sites and the tools that parse it (q ours line)
  game                sub_X / 0x8XXXXXXX / a mapped or db name: the function, its db name, callers; a data address: its db entry
  sdk                 the SDK index as last built (never refreshed here)
  text in code / docs one git grep over src tools config probes android + root scripts / docs, root *.md, .claude, CMake
  fact                the knowledge library (q fact)
Sizing (tools/q/qcap.py): every list says its total; a cut list ends `showing N of M ...; all: <the exact call>, or narrow: add --in PATH`;
exact matches first; --in PATH narrows (path contains PATH); --all = everything; --json is never cut.
Exit: 0 every question had a hit; 1 a question had none; 2 usage.
"""
import argparse
import json
import os
import re
import sqlite3
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import qcap  # noqa: E402

CODE_PATHS = ('src', 'tools', 'config', 'probes', 'android', ':(glob)*.ps1', ':(glob)*.py', ':(glob)*.bat', ':(glob)*.cmd')
DOCS_PATHS = ('docs', ':(glob)*.md', '.claude', 'CMakeLists.txt', 'cmake')
SKIP_PARTS = ('__pycache__', 'node_modules', 'spv', 'spirv')
FILE_EXTS = ('py', 'ps1', 'psm1', 'cpp', 'h', 'hpp', 'cc', 'inl', 'inc', 'md', 'json', 'txt', 'exe', 'bat', 'cmd', 'toml', 'cmake', 'glsl', 'frag',
             'vert', 'hlsl', 'cfg', 'tsv', 'csv', 'log', 'db', 'pac', 'sh', 'java', 'kt', 'rsc', 'zip', 'map', 'pdb', 'dll', 'lib')
ADDR = re.compile(r'^(?:sub_|0x|__imp__sub_|loc_)?([0-9A-Fa-f]{8})$')
IDENT = re.compile(r'^\$?[A-Za-z_~][\w$]*(?:(?:::|\.)[A-Za-z_~][\w]*)*$')
CAP_SYMS, CAP_FILES, CAP_FACT, CAP_CALLERS = 5, 6, 3, 3


class FindError(Exception):
    pass


# ---------------------------------------------------------------- shape

def is_docs(rel):
    return rel.startswith(('docs/', '.claude/', 'cmake/')) or ('/' not in rel and not rel.lower().endswith(('.ps1', '.py', '.bat', '.cmd')))


def has_ext(x):
    m = re.search(r'\.([A-Za-z0-9]{1,6})$', x)
    return bool(m) and m.group(1).lower() in FILE_EXTS


def pathlike(x):
    return ('/' in x or '\\' in x) and not x.startswith('[')


def needle_of(x):
    """the text one -F grep looks for: a qualified identifier's tail (lines with the full X rank first), a path's basename, else X"""
    x = x.strip()
    if pathlike(x):
        return re.split(r'[\\/]+', x.rstrip('\\/'))[-1] or x
    if has_ext(x):
        return x
    if IDENT.match(x):
        t = x.lstrip('$')
        return re.split(r'::|\.', t)[-1] or t
    return x


def addr_of(x):
    m = ADDR.match(x.strip())
    if not m or not re.search(r'[0-9]', m.group(1)):
        return None
    a = int(m.group(1), 16)
    return a if 0x82000000 <= a < 0x85000000 else None


# ---------------------------------------------------------------- git

def _git(root, args, out):
    try:
        p = subprocess.run(['git', '-C', root] + args, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60)
        out['rc'], out['text'] = p.returncode, p.stdout
    except (OSError, subprocess.SubprocessError) as e:
        out['rc'], out['text'] = -1, ''
        out['err'] = str(e)


def grep_args(pattern, fixed=True):
    return ['grep', '--untracked', '-n', '-I', '-i', '-F' if fixed else '-P', '-e', pattern, '--'] + list(CODE_PATHS) + list(DOCS_PATHS)


def parse_grep(text):
    out = []
    for ln in text.splitlines():
        m = re.match(r'^(.*?):(\d+):(.*)$', ln)
        if not m:
            continue
        rel = m.group(1)
        if any(p in SKIP_PARTS for p in rel.split('/')):
            continue
        out.append((rel, int(m.group(2)), m.group(3)))
    return out


def path_regex(x):
    parts = [p for p in re.split(r'[\\/]+', x.strip().strip('"')) if p]
    return r'[\\/]+'.join(re.escape(p) for p in parts)


class Probe:
    """the shared, cheap reads of one question: one -F grep (code + docs), the file list, the path-literal grep (path-like X only)"""

    def __init__(self, root, x):
        self.root, self.x, self.needle = root, x, needle_of(x)
        jobs = {'grep': grep_args(self.needle), 'files': ['ls-files', '-co', '--exclude-standard']}
        if pathlike(x):
            jobs['path'] = grep_args(path_regex(x), fixed=False)
        res = {k: {} for k in jobs}
        ts = [threading.Thread(target=_git, args=(root, a, res[k])) for k, a in jobs.items()]
        for t in ts:
            t.start()
        for t in ts:
            t.join()
        if res['grep'].get('rc') not in (0, 1):
            raise FindError(f'git grep failed in {root} ({res["grep"].get("err") or "exit " + str(res["grep"].get("rc"))}): is it a git checkout?')
        self.lines = parse_grep(res['grep']['text'])
        self.files = [f for f in res['files'].get('text', '').splitlines() if f]
        self.path_lines = parse_grep(res['path']['text']) if 'path' in res and res['path'].get('rc') in (0, 1) else None


# ---------------------------------------------------------------- kinds

def line_rank(text, x, needle):
    if x != needle:
        r = qcap.rank(text, x)
        if r < 3:
            return r
        return 3 + min(2, qcap.rank(text, needle))
    return min(2, qcap.rank(text, needle))


def text_block(lines, x, needle, spec):
    """lines -> {total, files, exact, list of files ranked: exact before partial, then by path}"""
    by = {}
    tot = 0
    out_n = 0
    for rel, n, t in lines:
        if not qcap.in_scope(rel, spec):
            out_n += 1
            continue
        tot += 1
        e = by.setdefault(rel, {'file': rel, 'lines': [], 'rank': 9, 'best': ''})
        r = line_rank(t, x, needle)
        e['lines'].append(n)
        if r < e['rank']:
            e['rank'], e['best'] = r, t.strip()[:110]
    files = sorted(by.values(), key=lambda e: (e['rank'], e['file']))
    exact = sum(1 for rel, n, t in lines if qcap.in_scope(rel, spec) and line_rank(t, x, needle) == 0)
    return {'lines': tot, 'n_files': len(files), 'exact_lines': exact, 'outside_in': out_n, 'files': files}


PY_DEF = r'^\s*(?:async\s+)?def\s+{n}\s*\(|^\s*class\s+{n}\b|^{n}\s*(?::[^=\n]*)?=(?!=)'
PS_DEF = r'^\s*(?:function|filter)\s+{n}\b'


def py_block(lines, x, needle, spec):
    if not IDENT.match(x) and not re.fullmatch(r'[A-Za-z_][\w-]*', x):      # a ps1 function name holds '-' (Invoke-Thing)
        return None
    n = re.escape(needle.lstrip('$'))
    pyd, psd = re.compile(PY_DEF.format(n=n)), re.compile(PS_DEF.format(n=n), re.I)
    defs, uses = [], []
    for rel, ln, t in lines:
        if not rel.endswith(('.py', '.ps1', '.psm1')):
            continue
        if (rel.endswith('.py') and pyd.search(t)) or (not rel.endswith('.py') and psd.search(t)):
            defs.append({'file': rel, 'line': ln, 'text': t.strip()[:110]})
        elif qcap.rank(t, needle.lstrip('$')) == 0 and qcap.in_scope(rel, spec):
            uses.append((rel, ln))
    if not defs:
        return None
    if spec:      # --in: the definitions there; when it only USES the name, the definitions stay (what it uses), and uses say how many are inside
        inside = [d for d in defs if qcap.in_scope(d['file'], spec)]
        if inside:
            defs = inside
        elif not uses:
            return None
    qual = re.split(r'::|\.', x)[-2] if re.search(r'::|\.', x) else None
    if qual:      # `qtools.load`: the definitions in qtools.py (or in class qtools) first
        defs.sort(key=lambda d: (os.path.splitext(os.path.basename(d['file']))[0] != qual, d['file'], d['line']))
    return {'defs': defs, 'uses': len(uses), 'use_files': len({u[0] for u in uses})}


def cvar_block(lines, x, needle, sdk):
    if not IDENT.match(x):
        return None
    rx = re.compile(r'REXCVAR_DEFINE_(\w+)\s*\(\s*' + re.escape(needle) + r'\s*,')
    defs = [{'file': rel, 'line': ln, 'type': m.group(1), 'text': t.strip()[:120]} for rel, ln, t in lines for m in [rx.search(t)] if m]
    sdk_defs = [d for d in (sdk or {}).get('defs', []) if d.get('kind') == 'cvar']
    if not defs and not sdk_defs:
        return None
    uses = sum(1 for rel, ln, t in lines if not is_docs(rel) and qcap.rank(t, needle) == 0) - len(defs)
    return {'defs': defs, 'sdk_defs': sdk_defs, 'uses': max(0, uses)}


def env_block(lines, x, spec):
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]{2,}', x):
        return None
    import qm
    word, setp, readp = qm.env_rx(x)
    sets, reads = [], []
    for rel, ln, t in lines:
        if is_docs(rel) or not word.search(t) or not qcap.in_scope(rel, spec):
            continue
        if t.lstrip().startswith(('#', '//', '*', '/*', 'REM ', '::')):
            continue
        if any(r.search(t) for r in setp):
            sets.append({'file': rel, 'line': ln, 'text': t.strip()[:110]})
        elif any(r.search(t) for r in readp):
            reads.append({'file': rel, 'line': ln, 'text': t.strip()[:110]})
    if not sets and not reads:
        return None
    return {'set': sets, 'read': reads}


def file_block(pr, x, spec):
    xs = x.replace('\\', '/').strip('/')
    xl = xs.lower()
    base_only = '/' not in xs
    hits = []
    for f in pr.files:
        fl = f.lower()
        b = fl.rsplit('/', 1)[-1]
        if fl == xl or fl.endswith('/' + xl) or (base_only and (b == xl or os.path.splitext(b)[0] == xl)):
            hits.append(f)
    if not hits:
        return None
    hits.sort(key=lambda f: (f.lower().rsplit('/', 1)[-1] != xl and f.lower() != xl, f.count('/'), f))
    out = []
    for f in hits:
        b = f.rsplit('/', 1)[-1]
        stem = os.path.splitext(b)[0]
        try:
            with open(os.path.join(pr.root, f), 'rb') as fh:
                nl = fh.read().count(b'\n')
        except OSError:
            nl = None
        imp = re.compile(r'^\s*(?:import\s+[\w., ]*\b|from\s+[\w.]*\b)' + re.escape(stem) + r'\b') if f.endswith('.py') else None
        inc = re.compile(r'#\s*include\s*[<"][^>"]*\b' + re.escape(b) + r'[>"]', re.I)
        brx = re.compile(r'(?<![A-Za-z0-9_.-])' + re.escape(b) + r'(?![A-Za-z0-9_-])', re.I)
        inv = re.compile(r'(?:^|[\s"\'(\[,&])(?:py|python3?|pwsh|powershell|bash|sh|cmd)(?:\.exe)?\s+[^\n]*' + re.escape(b) + r'|^\s*[&.]\s+\S*' + re.escape(b)
                         + r'|Start-Process|subprocess|\bPopen\b|sys\.executable|\bsystem\(|-File\s', re.I)
        code_ext = ('.py', '.ps1', '.psm1', '.bat', '.cmd', '.sh', '.cmake', '.cpp', '.h', '.hpp')
        groups = {'import': [], 'include': [], 'invoke': [], 'mention': []}
        for rel, ln, t in pr.lines:
            if rel == f or not qcap.in_scope(rel, spec):
                continue
            if imp and not is_docs(rel) and rel.endswith('.py') and imp.search(t):
                groups['import'].append((rel, ln, t))
            elif brx.search(t.replace('\\', '/')):
                if inc.search(t):
                    groups['include'].append((rel, ln, t))
                elif rel.endswith(code_ext) and inv.search(t) and not t.lstrip().startswith(('#', '//', '"""', '*')):
                    groups['invoke'].append((rel, ln, t))
                else:
                    groups['mention'].append((rel, ln, t))
        out.append({'file': f, 'lines': nl, **{k: [{'file': a, 'line': b_, 'text': c.strip()[:110]} for a, b_, c in v] for k, v in groups.items()}})
    return {'matches': out}


def cpp_block(root, db, x, spec):
    """the C++ symbol probe, by text (tools/q/qsym.py: a whole-word, case-sensitive search of src / tools / probes; no index, the same in any worktree): -> (block or None, label)"""
    if not IDENT.match(x) or has_ext(x) and not re.search(r'::', x):
        return None, ''
    import qsym
    blk = qsym.find_block(root, x.lstrip('$'), spec, qcap.in_scope)
    return blk, ('text search of the files on disk' if blk else '')


def game_block(qmod, index, x, fcon):
    """sub_X / 0x8XXXXXXX / a mapped name / a db name -> the game function (or data address) as the index and the db have it"""
    if qmod is None:
        return None
    a = addr_of(x)
    db_hit = None
    if a is None:
        if not re.fullmatch(r'[A-Za-z_][\w@?$]*', x):
            return None
        if fcon is not None:
            r = fcon.execute('SELECT addr, name, confidence, system, file FROM names WHERE name = ? ORDER BY rank DESC LIMIT 1', (x,)).fetchone()
            if r:
                a, db_hit = r['addr'], dict(r)
    try:
        con = qmod.connect(index or qmod.DB)
    except Exception as e:     # noqa: BLE001 - no game index: say so, never fail find
        return {'error': f'game index not readable: {e}'} if a is not None else None
    try:
        if a is None:
            row = con.execute('SELECT addr FROM funcs WHERE name = ?', (x,)).fetchone()
            if not row:
                return None
            a = row['addr']
        f = con.execute('SELECT * FROM funcs WHERE addr = ?', (a,)).fetchone()
        inside = None
        if f is None:
            c = qmod.containing(con, a)
            if c:
                inside, f = a - c['addr'], c
        if db_hit is None and fcon is not None:
            r = fcon.execute('SELECT addr, name, confidence, system, file FROM names WHERE addr = ? ORDER BY rank DESC LIMIT 1', (f['addr'] if f else a,)).fetchone()
            db_hit = dict(r) if r else None
        out = {'addr': qmod.h(a), 'db': db_hit}
        if f is not None:
            n = con.execute('SELECT COUNT(*) AS n, COUNT(DISTINCT src) AS f FROM edges WHERE dst = ?', (f['addr'],)).fetchone()
            first = [qmod.fname(con, r['src']) for r in con.execute('SELECT DISTINCT src FROM edges WHERE dst = ? ORDER BY src LIMIT ?', (f['addr'], CAP_CALLERS))]
            out.update(func=f['name'], func_addr=qmod.h(f['addr']), offset=inside, size=f['size'], call_sites=n['n'], callers=n['f'], first_callers=first)
        else:
            out['data'] = True
        return out
    finally:
        con.close()


def sdk_block(x):
    if not IDENT.match(x) or has_ext(x):
        return None
    try:
        import qsdk
        p = qsdk.db_path(ROOT)
    except Exception:      # noqa: BLE001
        return None
    if not os.path.exists(p):
        return None
    tail = re.split(r'::|\.', x)[-1]
    try:
        con = sqlite3.connect(f'file:{p}?mode=ro', uri=True)
        con.row_factory = sqlite3.Row
        defs = [dict(r) for r in con.execute('SELECT * FROM defs WHERE name = ? ORDER BY file, line', (tail,))]
        nref = con.execute('SELECT COUNT(*) FROM refs WHERE name = ?', (tail,)).fetchone()[0]
        con.close()
    except sqlite3.Error:
        return None
    if not defs and not nref:
        return None
    return {'defs': defs, 'refs': nref}


def fact_block(x, spec, root, limit=50):
    import qfact
    old = (qfact.ROOT, qfact.INDEX)
    if root and os.path.normcase(os.path.abspath(root)) != os.path.normcase(qfact.ROOT):
        qfact.use_root(root)
    try:
        con, info = qfact.connect()
        # review: under --in the total is counted over every match fact keeps (400), not over the first `limit` hits
        r = qfact.search(con, [w for w in re.split(r'\s+', x) if w], 400 if spec else limit)
    except Exception as e:    # noqa: BLE001
        return None, None
    finally:      # review: q multi runs questions in one process: --root must not leave the library pointed at another tree
        qfact.ROOT, qfact.INDEX = old
    hits = [h for h in r['hits'] if qcap.in_scope(h['file'], spec)]
    if not hits:
        return None, con
    return {'mode': r['mode'], 'total': min(r.get('n_matches', len(hits)), 400) if not spec else len(hits), 'hits': hits,
            'capped': r.get('n_matches', 0) >= 400, 'rebuilt_s': info.get('rebuilt_s')}, con


def line_block(root, x, spec):
    if not (x.startswith('[') and ']' in x):
        return None
    import qtext
    r = qtext.cmd_line(root, x, lambda rel, ln: '')
    if not r or (not r['sites'] and not r['parsers'] and not r.get('fallback')):
        return None
    return {'sites': [s for s in r['sites'] if qcap.in_scope(s['file'], spec)], 'parsers': [p for p in r['parsers'] if qcap.in_scope(p['file'], spec)],
            'fallback': r.get('fallback')}


def show_block(root, x):
    import qshow
    if not qshow.SPEC.match(x.strip()):
        return None
    try:
        return qshow.cmd_show(root, [x])
    except qshow.ShowError:
        return None


# ---------------------------------------------------------------- one question

def answer(x, root, db=None, spec=None, qmod=None, index=None, structural=True, full=False):
    """-> {question, kinds: {name: block}, no_hit: [...], labels, took_s}. structural=False: the elsewhere probes only (no C++ / game / sdk)"""
    t0 = time.time()
    x = x.strip()
    kinds, labels = {}, {}
    sh = show_block(root, x)
    if sh:
        return {'question': x, 'kinds': {'lines': sh}, 'no_hit': [], 'labels': {}, 'took_s': round(time.time() - t0, 2)}
    pr = Probe(root, x)
    code = [l for l in pr.lines if not is_docs(l[0])]
    docs = [l for l in pr.lines if is_docs(l[0])]
    tried = []

    def put(name, blk):
        tried.append(name)
        if blk:
            kinds[name] = blk
    if structural:
        blk, lab = cpp_block(root, db, x, spec)
        if lab:
            labels['C++ symbol'] = lab
        put('C++ symbol', blk)
    put('python / ps1 name', py_block(code, x, pr.needle, spec))
    put('file', file_block(pr, x, spec))
    if pr.path_lines is not None:
        pl = text_block(pr.path_lines, x, x, spec)
        put('path literal', pl if pl['lines'] else None)
    sdk = sdk_block(x) if structural and not spec else None       # --in names one of OUR paths: the SDK is outside it
    put('cvar', cvar_block(code, x, pr.needle, sdk))
    put('env var', env_block(code + [l for l in docs if '/' not in l[0]], x, spec))
    put('log line', line_block(root, x, spec))
    fb, fcon = fact_block(x, spec, root, 400 if full else 50)      # --json / --all: every section (never cut)
    if structural:
        put('game', game_block(qmod, index, x, fcon))
        put('sdk', sdk if sdk and not kinds.get('cvar', {}).get('sdk_defs') else None)
    tb = text_block(code, x, pr.needle, spec)
    put('text in code', tb if tb['lines'] else None)
    db_ = text_block(docs, x, pr.needle, spec)
    put('docs', db_ if db_['lines'] else None)
    put('fact', fb)
    return {'question': x, 'needle': pr.needle, 'kinds': kinds, 'no_hit': [k for k in tried if k not in kinds], 'labels': labels,
            'outside_in': {'code': tb['outside_in'], 'docs': db_['outside_in']} if spec else {}, 'took_s': round(time.time() - t0, 2)}


# ---------------------------------------------------------------- print

def _ls(lines, k=6):
    return ','.join(str(n) for n in lines[:k]) + ('...' if len(lines) > k else '')


def _files(blk, lim, all_, call, indent='  '):
    shown, cut = qcap.cap(blk['files'], lim, all_)
    for e in shown:
        print(f'{indent}{e["file"]}:{_ls(e["lines"], 10**9 if all_ else 6)}  ({len(e["lines"])})  {e["best"]}')
    if cut:
        print(f'{indent}{qcap.trim_line(len(shown), blk["n_files"], "files", call)}')


def _rows(rows, lim, all_, call, what, indent='    '):
    shown, cut = qcap.cap(rows, lim, all_)
    for r in shown:
        print(f'{indent}{r["file"]}:{r["line"]}  {r["text"]}')
    if cut:
        print(f'{indent}{qcap.trim_line(len(shown), len(rows), what, call)}')


def print_answer(r, a):
    x = r['question']
    k = r['kinds']
    lim, all_ = a.limit, a.all
    inq = f' --in {qcap.requote([a.in_path])}' if a.in_path else ''
    q = qcap.requote([x])
    print(f'== find {x}' + (f'   (--in {a.in_path})' if a.in_path else ''))
    if 'lines' in k:
        import qshow
        qshow.print_show(k['lines'])
        return
    if 'C++ symbol' in k:
        ms = k['C++ symbol']['matches']
        print(f'C++ symbol: {len(ms)} match{"es" if len(ms) != 1 else ""}  ({r["labels"].get("C++ symbol", "")})')
        shown, cut = qcap.cap(ms, CAP_SYMS, all_)
        for m in shown:
            refs = ', '.join(f'{n} {kk}' for kk, n in sorted(m['refs'].items(), key=lambda t: -t[1])) or 'no references'
            print(f'  {m["kind"]} {m["qname"]}  {m["file"] or "(SDK)"}:{m["line"]}  {"defined" if m["defined"] else "declared"}   refs: {refs}')
            if 'callers' in m and m['callers']:
                cs, ccut = qcap.cap(m['callers'], CAP_CALLERS, all_)
                more = ''
                if ccut:
                    more = '; ' + qcap.trim_line(len(cs), len(m['callers']), 'calling places', 'q ours callers ' + qcap.requote([m['qname']]) + ' --all' + inq, narrow=False)
                print(f'    {len(m["callers"])} calling places: ' + '; '.join(f'{c["func"]} ({c["file"]}:{c["line"]})' for c in cs) + more)
        if cut:
            print(f'  {qcap.trim_line(len(shown), len(ms), "symbols", f"q ours def {q}", narrow=False)}')
        print(f'  next: q ours callers|refs|fn|body {q}')
    if 'python / ps1 name' in k:
        p = k['python / ps1 name']
        print(f'python / ps1 name (text match; exact: q py refs {q}): {len(p["defs"])} definition line(s); used on {p["uses"]} lines in {p["use_files"]} files')
        _rows(p['defs'], CAP_SYMS, all_, f'q py def {q}', 'definitions')
    if 'file' in k:
        ms = k['file']['matches']
        shown, cut = qcap.cap(ms, CAP_SYMS, all_)
        for m in shown:
            print(f'file: {m["file"]}' + (f'  ({m["lines"]} lines)' if m['lines'] is not None else ''))
            b = qcap.requote([m['file'].rsplit('/', 1)[-1]])
            for g, what, call in (('import', 'imported by', f'q py refs {b}'), ('include', 'included by', f'q ours includers {b}'),
                                  ('invoke', 'invoked by', f'q ours runs {b}'), ('mention', 'named by', f'q ours runs {b}')):
                rows = m[g]
                if rows:
                    print(f'  {what}: {len(rows)} lines in {len({x_["file"] for x_ in rows})} files')
                    _rows(rows, 3 if g == 'mention' else lim, all_, call + inq, 'lines')
        if cut:
            print(f'  {qcap.trim_line(len(shown), len(ms), "files of that name", f"q find {q} --all", narrow=False)}')
    if 'path literal' in k:
        b = k['path literal']
        print('path literal (/ and \\ match as one): ' + f'{b["lines"]} lines in {b["n_files"]} files')
        _files(b, lim, all_, f'q ours lit {q} --all{inq}')
    if 'cvar' in k:
        c = k['cvar']
        print(f'cvar {x}: {len(c["defs"])} define(s) in our code' + (f', {len(c["sdk_defs"])} in the SDK' if c['sdk_defs'] else '') + f'; {c["uses"]} other lines name it; next: q ours cvar {q}')
        _rows(c['defs'], CAP_SYMS, all_, f'q ours cvar {q}', 'defines')
        for d in c['sdk_defs'][:3]:
            print(f'    {d["file"]}:{d["line"]}  [sdk] {d["qname"]}')
    if 'env var' in k:
        e = k['env var']
        print(f'env var {x}: set on {len(e["set"])} lines, read on {len(e["read"])}; next: q ours env {q}')
        for what in ('set', 'read'):
            if e[what]:
                print(f'  {what}:')
                _rows(e[what], CAP_FILES, all_, f'q ours env {q}', f'{what} lines')
    if 'log line' in k:
        l_ = k['log line']
        print(f'log line: printed by {len(l_["sites"])} site(s), parsed / named by {len(l_["parsers"])} tool line(s); next: q ours line {q}')
        for s in l_['sites'][:CAP_FILES]:
            print(f'    printed: {s["file"]}:{s["line"]}  "{s["fmt"][:90]}"')
        for s in l_['parsers'][:3]:
            print(f'    parsed:  {s["file"]}:{s["line"]}  {s["how"]}')
        if l_.get('fallback') and not l_['sites']:
            fbk = l_['fallback']
            print(f'    UNCONFIRMED (no single printf literal): {fbk["n"]} lines hold {fbk["needle"]!r}' + (' (WEAK)' if fbk['weak'] else ''))
    if 'game' in k:
        g = k['game']
        if 'error' in g:
            print(f'game: {g["error"]}')
        elif g.get('data'):
            d = g.get('db') or {}
            print(f'game data address {g["addr"]}' + (f' = {d.get("name")} [{d.get("system")}, {d.get("confidence")}] ({d.get("file")})' if d else ' (no function, no db entry)')
                  + f'; next: q addr {g["addr"]} | q field {g["addr"]} --stores | q db {g["addr"]}')
        else:
            d = g.get('db') or {}
            off = f' (+0x{g["offset"]:X} inside it)' if g.get('offset') else ''
            print(f'game function {g["func"]} {g["func_addr"]}{off}' + (f' = {d.get("name")} [{d.get("system")}, {d.get("confidence")}] ({d.get("file")})' if d else ' (no db entry)')
                  + f'  size {g["size"]}; {g["call_sites"]} call sites in {g["callers"]} functions' + (': ' + ', '.join(g['first_callers']) + (' ...' if g['callers'] > len(g['first_callers']) else '') if g['first_callers'] else ''))
            print(f'  next: q fn {g["func_addr"]} | q decomp {g["func_addr"]} | q callers {g["func_addr"]}')
    if 'sdk' in k:
        s = k['sdk']
        print(f'sdk (index as last built): {len(s["defs"])} definition(s), {s["refs"]} reference lines; next: q sdk refs {q}')
        for d in s['defs'][:3]:
            print(f'    {d["file"]}:{d["line"]}  {d["kind"]} {d["qname"]}')
        if len(s['defs']) > 3:
            print(f'    {qcap.trim_line(3, len(s["defs"]), "definitions", f"q sdk def {q}", narrow=False)}')
    for name, call in (('text in code', f'q ours text {q} --all{inq}'), ('docs', f'q ours text {q} --docs --all{inq}')):
        if name in k:
            b = k[name]
            print(f'{name}: {b["lines"]:,} lines in {b["n_files"]:,} files ({b["exact_lines"]:,} exact whole-word)'
                  + (f'; {b["outside_in"]:,} more lines outside --in' if b['outside_in'] else ''))
            _files(b, lim, all_, f'q find {q} --all{inq}' if not a.all else call)
    if 'fact' in k:
        f = k['fact']
        hits = f['hits']
        sh, cut = qcap.cap(hits, CAP_FACT, all_)
        print(f'fact: {f["total"]}{"+" if f["total"] >= 400 or f.get("capped") else ""} matching sections' +('' if f['mode'] == 'all' else f' ({f["mode"]})') + (f'  (library index rebuilt in {f["rebuilt_s"]:.1f} s)' if f.get('rebuilt_s') else ''))
        for h in sh:
            print(f'  {h["file"]}:{h["line"]}  [{h["kind"]}]  {h["heading"][:100]}')
        if cut or f['total'] > len(sh):
            print(f'  {qcap.trim_line(len(sh), f["total"], "sections", f"q fact {x} --limit {max(f["total"], 8)}", narrow=False)}')
    if r['no_hit']:
        left = r.get('outside_in') or {}
        gone = [f'{left[w]:,} {w} lines' for w, name in (('code', 'text in code'), ('docs', 'docs')) if left.get(w) and name not in k]
        tail = (f'  (--in {a.in_path}: {" and ".join(gone)} are outside --in; drop --in to see them)' if gone else
                '' if k else '  (nothing anywhere: check the spelling; `q missed "<question>"` logs the gap)')
        print(f'no hit as: {", ".join(r["no_hit"])}' + tail)


# ---------------------------------------------------------------- elsewhere (item 4: no dead ends)

def elsewhere(root, x, spec=None):
    """what the other probes find for a name a structural index did not know: [lines]. Never raises."""
    try:
        r = answer(x, root, spec=spec, structural=False)
    except Exception as e:    # noqa: BLE001 - a fallback must never replace the real error
        return [f'(q find {x} could not run: {e})']
    k = r['kinds']
    if not k:
        return [f'also tried by q find: {", ".join(r["no_hit"])}: no hit anywhere']
    bits = []
    for m in k.get('file', {}).get('matches', [])[:3]:
        bits.append(f'file {m["file"]}')
    if 'python / ps1 name' in k:
        d = k['python / ps1 name']['defs'][0]
        bits.append(f'python / ps1 definition {d["file"]}:{d["line"]}')
    for name in ('cvar', 'env var', 'log line', 'path literal'):
        if name in k:
            bits.append(name)
    for name in ('text in code', 'docs'):
        if name in k:
            bits.append(f'{name} {k[name]["lines"]} lines in {k[name]["n_files"]} files')
    if 'fact' in k:
        bits.append(f'fact {k["fact"]["total"]} sections')
    return [f'found elsewhere: {"; ".join(bits)}', f'  -> q find {qcap.requote([x])}']


# ---------------------------------------------------------------- main

LAST = []      # the answers of the last main() call (q.py's unknown-verb route reads them)
STRUCTURAL = ('file', 'C++ symbol', 'python / ps1 name', 'cvar', 'env var', 'game')


def structural_hit(answers, x):
    """S108 FD review: did question `x` hit as a NAME (file / stem, C++ or python symbol, cvar, env var, game name or address)? Text-only hits
    (code text, docs, fact, a path literal) do not count: a mistyped verb found in some comment is still a typo."""
    for r in answers:
        if r.get('question') == x.strip():
            k = r.get('kinds', {})
            return any(n in k and not (n == 'game' and 'error' in k[n]) for n in STRUCTURAL)
    return False


def main(argv, qmod=None):
    ap = argparse.ArgumentParser(prog='q find', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('words', nargs='+', help='each is one question; quote a phrase')
    ap.add_argument('--in', dest='in_path', default=None, help='narrow every list to paths containing PATH')
    ap.add_argument('--all', action='store_true', help='no cap on any list')
    ap.add_argument('--limit', type=int, default=CAP_FILES, help=f'files per text list (default {CAP_FILES})')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--db', default=None, help='accepted and ignored (q ours has no index)')
    ap.add_argument('--index', default=None, help='the game index.db (default q\'s)')
    a = ap.parse_args(argv)
    a.root = os.path.abspath(a.root)
    a.in_path = qcap.scope_spec(a.in_path, a.root)
    LAST.clear()
    rc = 0
    out, nohit = [], []
    for x in a.words:
        try:
            r = answer(x, a.root, db=a.db, spec=a.in_path, qmod=qmod, index=a.index, full=a.json or a.all)
        except FindError as e:
            if a.json:
                out.append({'question': x, 'error': str(e)})
            else:
                print(f'q find: {e}', file=sys.stderr)
            rc = 2
            continue
        LAST.append(r)
        if not r['kinds']:
            rc = max(rc, 1)
            nohit.append(x)
        if a.json:
            out.append(r)
        else:
            print_answer(r, a)
            if len(a.words) > 1:
                print()
            sys.stdout.flush()
    if a.json:
        print(json.dumps({'answers': out}, indent=1, default=str))
    elif len(a.words) > 1:
        print(f'find: {len(a.words)} questions' + (f'; no hit at all: {", ".join(nohit)} (exit 1)' if nohit else ''))
    return rc
