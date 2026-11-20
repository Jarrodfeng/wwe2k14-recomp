"""qbody (S105 QC): small answers that replace whole-file reads of our C++.

    q ours body FUNC            just that function's lines (ours.db spans; every overload up to 3; capped at --limit lines, default 400)
    q ours outline FILE         the functions (line-endline), types and their fields (line) of a file; no bodies
    q ours enclosing FILE:LINE  the function holding that line, with its span (and the type / namespace-less nearest def above)

The rule these answers keep (QC fix round): a line range is printed under the plain label `[index]` ONLY when the file on disk is the text the index
was built from (a worktree: not in qfresh's changed / new lists; anywhere else: size + sha1 match the index's files row). For any other file the
index spans are re-found in the file's own text (the nearest definition of the same name, its end by brace matching) and the label says so;
a function the branch added is found by the same scan and marked. A file the index does not know is answered from the scan alone, labelled.
"""
import hashlib
import os
import re
import sqlite3

FUNC_KINDS = (8, 21, 24, 25, 26, 30)
TYPE_KINDS = {2: 'struct', 3: 'union', 4: 'class', 5: 'enum'}
CPP = ('.h', '.hpp', '.cpp', '.inl', '.cc', '.inc')
TOPS = ('src', 'tools', 'probes')


class BodyError(Exception):
    pass


def _lines(root, rel):
    p = os.path.join(root, rel)
    try:
        with open(p, encoding='utf-8', errors='replace') as f:
            return f.read().split('\n')
    except OSError:
        raise BodyError(f'{rel}: cannot read {p}')


def resolve_file(con, root, name):
    """a path (repo-relative, or a unique basename / tail) -> rel path of an indexed or on-disk C++ file"""
    rel = name.replace('\\', '/')
    if os.path.isfile(os.path.join(root, rel)):
        return rel
    rows = [r[0] for r in con.execute("SELECT path FROM files WHERE path = ? OR path LIKE ? ESCAPE '\\'", (rel, '%/' + rel.replace('_', '\\_')))]
    if len(rows) == 1:
        return rows[0]
    if len(rows) > 1:
        raise BodyError(f'{name}: ambiguous: ' + ', '.join(rows[:6]))
    raise BodyError(f'{name}: no such file in the index or the checkout (give a path from the repo root)')


# ---- the text scan -------------------------------------------------------------------------------------------------------------------------------------

def _clean(lines):
    """the file's lines with comments, string / char literals and preprocessor lines blanked (same line count; #if 0 / #else second branches blanked too)"""
    out = []
    in_block = False
    cont = False
    ifs = []                     # per open #if: True while its inactive branch runs
    tok = re.compile(r'//|/\*|"|\'')
    for raw in lines:
        t = raw.rstrip('\r')
        res = []
        pos = 0
        n = len(t)
        while pos < n:
            if in_block:
                e = t.find('*/', pos)
                if e < 0:
                    res.append(' ' * (n - pos))
                    pos = n
                else:
                    res.append(' ' * (e + 2 - pos))
                    pos = e + 2
                    in_block = False
                continue
            m = tok.search(t, pos)
            if not m:
                res.append(t[pos:])
                break
            res.append(t[pos:m.start()])
            q = m.group(0)
            if q == '//':
                res.append(' ' * (n - m.start()))
                pos = n
            elif q == '/*':
                in_block = True
                res.append('  ')
                pos = m.end()
            elif q == "'" and m.start() > 0 and (t[m.start() - 1].isalnum() or t[m.start() - 1] == '_'):
                res.append(q)                      # a digit separator: 1'000
                pos = m.end()
            else:
                k = m.end()
                while k < n and t[k] != q:
                    k += 2 if t[k] == '\\' else 1
                k = min(k + 1, n)
                res.append(' ' * (k - m.start()))
                pos = k
        line = ''.join(res)
        s = line.lstrip()
        was_cont = cont
        cont = raw.rstrip().endswith('\\') and (was_cont or s.startswith('#'))
        if was_cont or s.startswith('#'):
            if not was_cont:
                d = s[1:].lstrip()
                if re.match(r'if(?:n?def)?\b', d):
                    ifs.append(bool(re.match(r'if\s+0\b', d)))
                elif re.match(r'(?:else|elif)\b', d) and ifs:
                    ifs[-1] = not ifs[-1] if d.startswith('else') and ifs[-1] else True
                elif d.startswith('endif') and ifs:
                    ifs.pop()
            out.append('')
            continue
        out.append('' if any(ifs) else line)
    return out


_SKIP = {'if', 'for', 'while', 'switch', 'catch', 'return', 'else', 'do', 'sizeof', 'defined', 'alignas', 'decltype', 'static_assert'}
_NAME_END = re.compile(r'((?:~?\w+(?:<[^<>]*>)?::)*(?:operator\s*(?:\(\s*\)|[^\s\w(]+|new|delete)|~?\w+))\s*$')
_REST_OK = re.compile(r'^(?:(?:const|noexcept(?:\s*\([^)]*\))?|override|final|volatile|&&|&|throw\s*\([^)]*\)|->[^{;=]+|REX_\w+|\w+_ATTR)\s*)*$')


def _groups(s):
    """top-level (...) groups of s: [(open, close)]"""
    out = []
    d = 0
    o = 0
    for i, ch in enumerate(s):
        if ch == '(':
            if d == 0:
                o = i
            d += 1
        elif ch == ')' and d:
            d -= 1
            if d == 0:
                out.append((o, i))
    return out


def _classify(stmt, bl):
    """the statement text that precedes a `{` at namespace / class level -> ('ns',) | ('fn', name, line) | ('init',) | ('blk',)"""
    off = len(stmt) - len(stmt.lstrip())
    m = re.match(r'(?:(?:public|private|protected|signals|slots)\s*:\s*)+', stmt[off:])
    if m:
        off += m.end()
    s = stmt[off:]
    if re.match(r'^(?:inline\s+)?namespace\b[^=;]*$', s) or re.match(r'^extern\s*$', s):
        return ('ns',)
    s2 = re.sub(r'\b(?:alignas|__declspec|__attribute__)\s*\([^)]*\)', ' ', s)
    if re.match(r'^(?:template\s*<.*>\s*)?(?:typedef\s+)?(?:struct|class|union)\b[^=;()]*$', s2):
        return ('ns',)
    if re.match(r'^(?:typedef\s+)?enum\b', s2):
        return ('blk',)
    for o, c in _groups(s):
        mm = _NAME_END.search(s[:o])
        if not mm:
            continue
        name = mm.group(1)
        tail = name.split('::')[-1]
        if tail in _SKIP:
            continue
        rest = s[c + 1:].strip()
        if rest.startswith(':') and not rest.startswith('::'):
            if re.search(r'[\w>]$', rest):
                return ('init',)           # `C() : a{1}`: this brace initialises a member
            return ('fn', name, bl[off + mm.start(1)])
        if _REST_OK.match(rest):
            return ('fn', name, bl[off + mm.start(1)])
    return ('blk',)


def scan_funcs(lines):
    """text fallback: [(name, line, endline)] of function definitions found by brace matching over comment / string / #if-else cleaned text:
    a statement ending in `{` whose parenthesised head is followed only by qualifiers, `-> T` or a constructor initialiser list, at namespace / class /
    extern-block level. Heuristic (not the clang AST): used for files the index lacks and to re-find index spans in an edited file."""
    code = _clean(lines)
    out = []
    stack = []                      # ['ns'] | ['fn', name, line, depth] | ['blk', depth] | ['init', depth]
    buf = []
    bl = []
    par = 0
    for ln, t in enumerate(code, 1):
        for ch in t + '\n':
            top = stack[-1] if stack else None
            if top is not None and top[0] != 'ns':
                if ch == '{':
                    top[-1] += 1
                elif ch == '}':
                    top[-1] -= 1
                    if top[-1] == 0:
                        stack.pop()
                        if top[0] == 'fn':
                            out.append((top[1], top[2], ln))
                            buf, bl, par = [], [], 0
                        elif top[0] == 'init':
                            buf.extend('{}')
                            bl.extend([ln, ln])
                        else:
                            buf, bl, par = [], [], 0
                continue
            if ch == '{':
                if par > 0:
                    stack.append(['init', 1])
                    continue
                kind = _classify(''.join(buf), bl)
                if kind[0] == 'ns':
                    stack.append(['ns'])
                    buf, bl = [], []
                elif kind[0] == 'fn':
                    stack.append(['fn', kind[1], kind[2], 1])
                elif kind[0] == 'init':
                    stack.append(['init', 1])
                else:
                    stack.append(['blk', 1])
            elif ch == '}':
                if top is not None:
                    stack.pop()
                buf, bl, par = [], [], 0
            elif ch == ';' and par == 0:
                buf, bl = [], []
            else:
                if ch == '(':
                    par += 1
                elif ch == ')' and par:
                    par -= 1
                if buf or not ch.isspace():
                    buf.append(ch)
                    bl.append(ln)
    return out


# ---- is this file the text the index was built from? ---------------------------------------------------------------------------------------------------

def file_state(con, root, rel, info):
    """'fresh' (the index's text), 'edited' (differs from the index build, or cannot be told) or 'new' (not in the index)."""
    if info is not None:
        if rel in info.get('new', []):
            return 'new'
        return 'edited' if rel in info.get('changed', []) else 'fresh'
    try:
        row = con.execute('SELECT sha, size FROM files WHERE path = ?', (rel,)).fetchone()
    except sqlite3.Error:
        return 'edited'
    if not row:
        return 'new'
    try:
        p = os.path.join(root, rel)
        if os.stat(p).st_size != row[1]:
            return 'edited'
        with open(p, 'rb') as f:
            return 'fresh' if hashlib.sha1(f.read()).hexdigest() == row[0] else 'edited'
    except OSError:
        return 'edited'


def edited_files(con, root):
    """C++ files of src / tools / probes that differ from the index build or are not in it (a walk + sha1; only asked on a miss)"""
    base = {}
    try:
        base = {r[0]: (r[1], r[2]) for r in con.execute('SELECT path, sha, size FROM files')}
    except sqlite3.Error:
        pass
    out = []
    for top in TOPS:
        for dp, dn, fn in os.walk(os.path.join(root, top)):
            dn[:] = [d for d in dn if d not in ('__pycache__', 'node_modules')]
            for f in fn:
                if not f.endswith(CPP):
                    continue
                p = os.path.join(dp, f)
                rel = os.path.relpath(p, root).replace('\\', '/')
                b = base.get(rel)
                try:
                    if not b or os.stat(p).st_size != b[1]:
                        out.append(rel)
                        continue
                    with open(p, 'rb') as fh:
                        if hashlib.sha1(fh.read()).hexdigest() != b[0]:
                            out.append(rel)
                except OSError:
                    out.append(rel)
    return out


def _tail(qname):
    return qname.split('::')[-1]


def refind(rows, scan, stale):
    """index rows [(qname, a, b)] of an EDITED file -> ([(qname, a2, b2, mark)], dropped, added) by their own text: the nearest scanned definition of the same
    name (start) and its brace-matched end; scanned definitions no row claimed are added (mark 'added'). stale = the lines were not remapped at all."""
    used = set()
    res = []
    dropped = []
    for qn, a, b in rows:
        cands = [(abs(s - a), i) for i, (n, s, e) in enumerate(scan) if _tail(n) == _tail(qn) and i not in used]
        allc = sum(1 for n, s, e in scan if _tail(n) == _tail(qn))
        if not cands:
            dropped.append(qn)
            continue
        _, i = min(cands)
        used.add(i)
        n, s, e = scan[i]
        mark = ''
        if stale and allc > 1:
            mark = f'~ambiguous ({allc} definitions of that name; the nearest to the index line)'
        res.append((qn, s, e, mark))
    added = [(n, s, e, 'added') for i, (n, s, e) in enumerate(scan) if i not in used and not any(r[1] <= s <= r[2] and r[1] != s for r in res)]
    return res, dropped, added


def file_funcs(con, root, rel, info=None):
    """-> (rows [(qname, line, endline, mark)], label, note) for one file. 'index' only when the file is the indexed text; else the index rows re-found by
    text scan (label says so) plus the functions the scan finds that the index lacks; a file the index does not know: the scan alone."""
    rows = [(r[0], r[1], r[2]) for r in con.execute(
        'SELECT s.qname, f.line, f.endline FROM funcs f JOIN syms s ON s.usr = f.usr WHERE f.file = ? ORDER BY f.line', (rel,))]
    state = file_state(con, root, rel, info)
    if rows and state == 'fresh':
        return [(a, b, c, '') for a, b, c in rows], 'index', ''
    scan = scan_funcs(_lines(root, rel))
    if not rows:
        return [(n, a, b, '') for n, a, b in scan], 'text scan (file not in the index; heuristic)', ''
    stale = info is None or rel in info.get('stale', [])
    res, dropped, added = refind(rows, scan, stale)
    res += added
    res.sort(key=lambda r: r[1])
    note = f'{len(added)} added by this branch; ' if added else ''
    if dropped:
        note += f'{len(dropped)} index rows not found in the text and left out ({", ".join(dropped[:3])}); '
    label = f'index spans re-found by text scan: this file differs from the index build{", lines NOT remapped" if stale else ""}'
    return res, label, note.rstrip('; ')


# ---- the commands ---------------------------------------------------------------------------------------------------------------------------------------

def _fmt(lines, a, b, limit):
    out = []
    b = min(b, len(lines))
    for ln in range(a, min(b, a + limit - 1) + 1):
        out.append(f'{ln}\t{lines[ln - 1]}')
    if b - a + 1 > limit:
        out.append(f'... ({b - a + 1 - limit} more lines; q show FILE:{a + limit}-{b})')
    return out


def cmd_body(con, root, name, limit=400, find_syms=None, info=None):
    name = name.strip()
    cap = max(limit, 400)
    res = []
    tail = name.split('::')[-1]
    skipped = []
    if find_syms:
        for s in find_syms(con, name):
            if s['kind'] not in FUNC_KINDS:
                continue
            fr = con.execute('SELECT file, line, endline FROM funcs WHERE usr = ?', (s['usr'],)).fetchone()
            if not fr:
                continue
            rel = fr[0]
            if file_state(con, root, rel, info) == 'fresh':
                res.append((s['qname'], rel, fr[1], fr[2], 'index', ''))
                continue
            scan = scan_funcs(_lines(root, rel))
            stale = info is None or rel in info.get('stale', [])
            got, dropped, _ = refind([(s['qname'], fr[1], fr[2])], scan, stale)
            if not got:
                skipped.append(f'{s["qname"]} ({rel}: in the index, but not found in the edited text)')
                continue
            qn, a, b, mark = got[0]
            res.append((qn, rel, a, b, 'index span re-found by text scan: ' + rel + ' differs from the index build' + (', lines NOT remapped' if stale else ''), mark))
    scanned = 0
    if not res:           # a function the branch added (worktree), or added since the index build (main checkout): scan the edited / new files
        cand = (list(info.get('new', [])) + list(info.get('changed', []))) if info is not None else edited_files(con, root)
        for rel in cand:
            if not rel.endswith(CPP):
                continue
            try:
                ls = _lines(root, rel)
            except BodyError:
                continue
            scanned += 1
            for n, a, b in scan_funcs(ls):
                if n == name or _tail(n) == tail:
                    res.append((n, rel, a, b, 'text scan (new / changed file; heuristic)', ''))
    if not res:
        if skipped:
            raise BodyError(f'{name}: ' + '; '.join(skipped) + ' (q ours --worktree . body for an exact answer)')
        if scanned:
            raise BodyError(f'{name}: not in the index; the text scan of {scanned} edited / new files found no function of that name (the scan is a heuristic: '
                            f'q ours def {name} searches words)')
        raise BodyError(f'{name}: no such function in our code (not in the index, and no edited / new C++ files to scan; q ours def {name}; a data member or macro has no body)')
    out = []
    for qn, rel, a, b, src, mark in res[:3]:
        ls = _lines(root, rel)
        out.append(f'== {qn}  {rel}:{a}-{b}  [{src}]' + (f'  {mark}' if mark else ''))
        out += _fmt(ls, a, b, cap)
    if len(res) > 3:
        out.append(f'... {len(res) - 3} more overloads: {", ".join(f"{r[1]}:{r[2]}" for r in res[3:8])}')
    return out


def cmd_outline(con, root, name, limit=400, info=None):
    rel = resolve_file(con, root, name)
    out = []
    funcs, src, note = file_funcs(con, root, rel, info)
    try:
        total = len(_lines(root, rel))
    except BodyError:
        total = '?'
    out.append(f'== {rel}  ({total} lines)  [{src}]' + (f'  ({note})' if note else ''))
    types = []
    for r in con.execute('SELECT qname, kind, line FROM syms WHERE file = ? AND kind IN (2, 3, 4, 5) AND isdef = 1 ORDER BY line', (rel,)):
        types.append((r[0], TYPE_KINDS[r[1]], r[2]))
    fields = {}
    for r in con.execute('SELECT qname, line FROM syms WHERE file = ? AND kind = 6 ORDER BY line', (rel,)):
        owner, _, nm = r[0].rpartition('::')
        fields.setdefault(owner, []).append((nm, r[1]))
    if src != 'index' and types:
        out.append('  (type / field lines are the index lines, remapped by hunk only: they may be off in an edited file)')
    items = [(l, 't', q, k) for q, k, l in types] + [(a, 'f', q, b, m) for q, a, b, m in funcs]
    items.sort(key=lambda x: (x[0], x[1] == 'f'))
    for it in items[:limit]:
        if it[1] == 't':
            fl = fields.get(it[2], [])
            fs = ', '.join(f'{n}@{l}' for n, l in fl[:24]) + (f' ... +{len(fl) - 24}' if len(fl) > 24 else '')
            out.append(f'{it[0]:>6}  {it[3]} {it[2]}' + (f'   fields: {fs}' if fs else ''))
        else:
            out.append(f'{it[0]:>6}-{it[3]:<6} {it[2]}' + (f'   + ({it[4]}; text scan)' if it[4] == 'added' else (f'   {it[4]}' if it[4] else '')))
    if len(items) > limit:
        out.append(f'... {len(items) - limit} more (--limit N)')
    if not items:
        out.append('(no functions or types found)')
    return out


def cmd_enclosing(con, root, spec, info=None):
    m = re.match(r'^(.*?):(\d+)$', spec.strip())
    if not m:
        raise BodyError('usage: q ours enclosing FILE:LINE')
    rel = resolve_file(con, root, m.group(1))
    ln = int(m.group(2))
    funcs, src, note = file_funcs(con, root, rel, info)
    tag = f'[{src}]' + (f' ({note})' if note else '')
    hits = [f for f in funcs if f[1] <= ln <= f[2]]
    out = []
    if hits:
        inner = max(hits, key=lambda f: f[1])   # innermost (a lambda / local class method inside another function)
        q, a, b, mk = inner
        out.append(f'{rel}:{ln}  in {q}  {rel}:{a}-{b}  (line {ln - a + 1} of {b - a + 1})  {tag}' + (f'  {mk}' if mk and mk != 'added' else '') + ('  (added by this branch)' if mk == 'added' else ''))
        for o in hits:
            if o is not inner:
                out.append(f'  also inside {o[0]}  {o[1]}-{o[2]}')
    else:
        above = [f for f in funcs if f[2] < ln]
        below = [f for f in funcs if f[1] > ln]
        out.append(f'{rel}:{ln}  not inside a function {tag}')
        if above:
            f = max(above, key=lambda f: f[2])
            out.append(f'  previous: {f[0]}  {f[1]}-{f[2]}')
        if below:
            f = min(below, key=lambda f: f[1])
            out.append(f'  next:     {f[0]}  {f[1]}-{f[2]}')
    return out
