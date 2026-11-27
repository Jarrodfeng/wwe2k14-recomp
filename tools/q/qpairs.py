r"""q pairs (S111 lane CDJOIN, piece 1 of the code diff): THE JOIN = which game function(s) each of OUR functions mirrors.

    py tools\q\q.py pairs OURS_NAME     the game addresses it cites / mirrors (offset kept: +0x1D8 names the exact instruction), file:line, the text
    py tools\q\q.py pairs FILE          every pair whose our-side lives in that file (a name with a / or a source extension)
    py tools\q\q.py pairs --game ADDR   every one of ours that cites the game function (sub_XXXXXXXX | 0x8XXXXXXX | XXXXXXXX)
    py tools\q\q.py pairs --stats       counts: our functions with >= 1 citation, distinct game functions cited, the most cited
    py tools\q\q.py pairs --coverage    q ostate's state-setting sites (DrawState blocks, kTable rows, InstancedDraw stores): which the code
                                        diff can pair, the unpaired ones with their candidates (tools/q/qpairs_cov.py; --no-game skips q gstate)
    (all: --json, --root DIR = another checkout / a fixture tree, --rebuild = drop the cache, --limit N / --all)

Sources of a pair (the `src` column):
  comment / text  a citation `sub_8XXXXXXX` or `0x82..` / `0x83..` (+ optional `+0xNN`) in a comment or string literal of src/**: attributed to the
                  enclosing OUR function (qsym.Src brace scan); a comment ABOVE a definition belongs to the definition that follows (the next code
                  line: a function, a type, else the declared name = a constant / field); a trailing comment to its own line's declared name;
                  a comment above nothing (file head, before an #include / namespace) to `(file scope)`.
  code            the same token in CODE (REX_HOOK_RAW(sub_X) { ... }, a name that is the address): the function IS that game function.
  claim           db/claims/*.json rows that carry `ours` = file:line: the game address (function +offset, else the data address) <-> the function
                  that holds that line now.
  seam            db/seams/*.json: every game row's address of a topic <-> the function holding each of the topic's ours rows (anchor located by
                  qseam.row_state); TOPIC-LEVEL only (the file does not say which game row a given ours row stands against).
  seam-why        a game address named in an ours row's own `why` text <-> that row's function (precise).
A split part (.inc with a `// split:` parent) is read as itself, its names carry the parent's namespace / type chain (qsym.Src.seed) and `unit`
is the parent file the build compiles (tools/split/read_unit.unit_of).

Range rule: `sub_` + 8 hex starting with 8, or `0x82XXXXXX` / `0x83XXXXXX` (the image; 0x80000000 masks and 0x8F.. colours are not game addresses),
an offset only straight after the address: after a `sub_` token `+0x1D8`, `+7F0` or `+1994` (bare digits are hex, one optional space before the `+`, a bare
one needs a digit); after a `0x82..` data address only the `+0x..` form (`[[0x83929A68]+300]` is not an offset).

Relation (the `rel` column, tools/q/qpairs_rel.py, S111 PAIRMEAN): MIRRORS (our function reimplements the game function: the only rows
q codediff / q fieldsdiff / q pairs --coverage pair), WATCHES (a hook / driver / wrapper / handler / logger: our code observes or drives
it), CITES (any other mention, and every undecidable row); `why` says the rule that decided it. --rel CLASS keeps one class.

Cache: out\harness\index\pairs.s3.db of the checkout asked (per file: size + mtime stamp, plus the stamps of the includer chain of a part;
db/*.json rows are rebuilt when a db file or a src file they point into changed). Full scan once, then only what changed.
"""
import argparse
import json
import os
import re
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
SPLIT = os.path.join(HERE, '..', 'split')
if SPLIT not in sys.path:
    sys.path.insert(0, SPLIT)

ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
SCHEMA = 3      # 2: bare-hex offsets after a sub_ token; 3: the relation columns (qpairs_rel) and the hooks table
EXTS = ('.h', '.hpp', '.cpp', '.cc', '.inl', '.inc', '.c', '.glsl')
SKIP_DIRS = {'__pycache__', 'node_modules', '.git', 'spv', 'spirv'}
MAX_BYTES = 3_000_000
TEXT_CAP = 170
# selftest only: WWE_QPAIRS_PLANT=wrong_owner attributes every citation to the function BEFORE the right one (the planted twin the selftest must see fail)
PLANT = os.environ.get('WWE_QPAIRS_PLANT', '')

# group 1 = `sub_` (then group 2 = its address, group 4 = an offset `+0xNN` or a bare hex `+7F0` / `+1994` after at most one space; a bare one needs a digit, so
# `+add` words are not offsets); else group 3 = a `0x82..` address, whose offset (group 5) is only the `+0xNN` form: a bare `+300` after a data address is
# `[[0x83929A68]+300]` text, not an instruction offset
CITE = re.compile(r'(?<![\w])(?:(sub_)(8[0-9A-Fa-f]{7})|0x(8[23][0-9A-Fa-f]{6}))(?![\w])'
                  r'(?:(?(1)[ ]?\+(?:[ ]?0x|(?=[0-9A-Fa-f]{0,3}[0-9]))([0-9A-Fa-f]{1,5})|[ ]?\+[ ]?0x([0-9A-Fa-f]{1,5}))(?![\w]))?')
_BARE_AFTER_DATA = re.compile(r'[ ]?\+([0-9A-Fa-f]{1,4})(?![\w])')


class PairsError(Exception):
    pass


def _root(root):
    return os.path.abspath(root) if root else ROOT


def db_path(root=None):
    return os.path.join(_root(root), 'out', 'harness', 'index', 'pairs.s%d.db' % SCHEMA)


def norm(a):
    """'sub_82c224a0' / '0x82C224A0' / '82c224a0' -> '82C224A0' (None when it is not an address)"""
    m = re.fullmatch(r'\s*(?:sub_|0[xX])?([0-9A-Fa-f]{8})\s*', str(a or ''))
    return m.group(1).upper() if m else None


def cites_in(text):
    """[(HEX8, offset 'NN' hex upper or '', token start)] of the citations in one line"""
    out = []
    for m in CITE.finditer(text):
        addr = (m.group(2) or m.group(3)).upper()
        off = (m.group(4) or m.group(5) or '').upper()
        if PLANT == 'drop_decimal' and off.isdigit():              # planted twin: an all-digit offset is not read as hex
            off = ''
        if PLANT == 'data_bare' and m.group(3) and not off:        # planted twin: a bare offset after a 0x data address is taken
            b = _BARE_AFTER_DATA.match(text, m.end())
            off = b.group(1).upper() if b else ''
        out.append((addr, off, m.start()))
    return out


# ---------------------------------------------------------------------------------------------------------------- attribution

_DECL_CUT = re.compile(r'[=;{(,]')


def _decl_name(stmt):
    """the declared name of a statement head: the last identifier before the first `=` / `(` / `;` / `{` / `,`, else None"""
    m = _DECL_CUT.search(stmt)
    head = stmt[:m.start()] if m else stmt
    head = re.sub(r'\[[^\]]*\]\s*$', '', head.rstrip())
    ids = re.findall(r'[A-Za-z_]\w*', head)
    return ids[-1] if ids else None


class Owners:
    """line -> (qual, kind) of OUR entity one file attributes a citation on that line to (qsym.Src extents; no parser of our own beyond the declared name)"""

    def __init__(self, src):
        self.src = src
        src._scan()
        self.ml = src.mlines
        self.seed_q = '::'.join(n for _k, n in src.seed if n)
        self.heads = {}
        for sp in src.spans:
            if sp['kind'] in ('type', 'enum'):
                for ln in range(sp['start'], (sp['open'] or sp['start']) + 1):
                    self.heads.setdefault(ln, sp)
        self.fn_list = [sp for sp in src.spans if sp['kind'] == 'func']

    def code_at(self, ln):
        return ln - 1 < len(self.ml) and self.ml[ln - 1].strip() != ''

    def _scope_qual(self, ln):
        s = self.src
        i = s._sc_at[ln] if ln < len(s._sc_at) else None
        return (s._spans[i]['qual'] if i is not None else '') or self.seed_q

    def of(self, ln):
        s = self.src
        f = s.func_at(ln)
        if f is not None:
            return (f['qual'] or f['name']), 'func'
        n = ln
        while n <= len(self.ml) and not self.code_at(n) and n - ln < 60:
            n += 1
        if n > len(self.ml) or not self.code_at(n):
            return '(file scope)', 'file'
        line = self.ml[n - 1].lstrip()
        if line.startswith('#'):
            return '(file scope)', 'file'
        f = s.func_at(n)
        if f is not None:
            return (f['qual'] or f['name']), 'func'
        sp = self.heads.get(n)
        if sp is not None:
            if sp['kind'] == 'type' and sp['qual']:
                return sp['qual'], 'type'
            if sp['kind'] == 'enum' and sp['qual']:
                return sp['qual'], 'type'
            return '(file scope)', 'file'
        if re.match(r'(namespace|extern|using|typedef|template|public|private|protected)\b', line) and not re.search(r'\bconstexpr\b', line):
            return '(file scope)', 'file'
        stmt = ' '.join(self.ml[k].strip() for k in range(n - 1, min(n + 2, len(self.ml))))
        mac = re.match(r'([A-Z][A-Z0-9_]{2,})\s*\(([^()]*)\)', stmt)
        name = '%s(%s)' % (mac.group(1), mac.group(2).strip()[:40]) if mac else _decl_name(stmt)      # REX_EXTERN(__imp__sub_X): the macro line is the name
        if not name or name in ('const', 'static', 'inline', 'constexpr', 'struct', 'class', 'enum', 'typedef', 'using'):
            return '(file scope)', 'file'
        q = self._scope_qual(n)
        return (q + '::' + name if q else name), 'decl'

    def previous_func(self, ln):
        prev = [sp for sp in self.fn_list if sp['end'] is not None and sp['end'] < ln]
        return prev[-1] if prev else None


def _stamp(path):
    try:
        st = os.stat(path)
    except OSError:
        return None
    return '%d:%d' % (st.st_size, st.st_mtime_ns)


def _unit_rel(root, rel):
    import read_unit
    try:
        u = read_unit.unit_of(os.path.join(root, rel))
        return os.path.relpath(str(u), os.path.realpath(root)).replace('\\', '/')
    except (OSError, ValueError):
        return rel


def _chain(root, rel):
    """the includer chain of a split part: [rel, ...] of its parents (read_unit.includer_of), [] for a normal file"""
    import read_unit
    out = []
    p = os.path.join(root, rel)
    seen = set()
    real = os.path.realpath(root)
    while p not in seen:
        seen.add(p)
        inc = read_unit.includer_of(p) if p.endswith('.inc') else None
        if not inc:
            break
        p = str(inc[0])
        out.append(os.path.relpath(p, real).replace('\\', '/'))
        p = os.path.join(root, out[-1])
    return out


def scan_file(root, rel, text, src_cache):
    """-> (rows, nfunc, hooks) for one src file: rows = dicts src/ours/kind/line/game/off/text/rel0/why0; hooks = qpairs_rel.hook_rows"""
    import qpairs_rel
    import qsym
    rows = []
    lines = text.split('\n')
    hits = []
    for i, ln in enumerate(lines, 1):
        if '8' in ln and ('sub_8' in ln or '0x8' in ln):
            c = cites_in(ln)
            if c:
                hits.append((i, ln, c))
    src = qsym.Src(root, rel)
    src_cache[rel] = src
    ow = Owners(src)
    nfunc = len({sp['qual'] or sp['name'] for sp in ow.fn_list})
    for i, ln, c in hits:
        masked = ow.ml[i - 1] if i - 1 < len(ow.ml) else ln
        qual, kind = ow.of(i)
        if PLANT == 'wrong_owner' and kind == 'func':
            pf = ow.previous_func(i)
            if pf is not None:
                qual = pf['qual'] or pf['name']
        txt = re.sub(r'\s+', ' ', ln.strip())
        if len(txt) > TEXT_CAP:
            txt = txt[:TEXT_CAP - 3] + '...'
        if kind == 'file':
            qual = '(file scope of %s)' % rel
        seen = set()
        for addr, off, pos in c:
            n = 12 if ln[pos] == 's' else 10
            in_code = masked[pos:pos + n] == ln[pos:pos + n]      # the mask keeps offsets: the token survives it only in code
            key = (in_code, addr, off)
            if key in seen:
                continue
            seen.add(key)
            rows.append({'src': 'code' if in_code else 'comment', 'ours': qual, 'kind': kind, 'line': i, 'game': addr, 'off': off, 'text': txt})
    facts = ow.facts = qpairs_rel.Facts(ow)
    blocks = {}
    for r in rows:
        if r['line'] not in blocks:
            blocks[r['line']] = qpairs_rel.comment_block(lines, ow.ml, r['line'])
        r['rel0'], r['why0'] = qpairs_rel.classify(r, facts.of(r['ours']) if r['kind'] == 'func' else None, blocks[r['line']])
    return rows, nfunc, qpairs_rel.hook_rows(facts, rows)


# ---------------------------------------------------------------------------------------------------------------- the db-sourced pairs

def _db_rows(root, src_cache):
    """-> (rows, targets): the pair rows of db/claims + db/seams, and the src files they point into"""
    import glob
    import qpairs_rel
    import qseam
    import qsym
    rows = []
    targets = set()
    owners = {}

    def relate(r, text):
        o = owners.get(r['file'])
        f = None
        if o is not None and r['kind'] == 'func':
            if getattr(o, 'facts', None) is None:
                o.facts = qpairs_rel.Facts(o)
            f = o.facts.of(r['ours'])
        r['rel0'], r['why0'] = qpairs_rel.classify(r, f, text)
        return r

    def owner_at(rel, ln):
        if rel not in owners:
            try:
                s = src_cache.get(rel) or qsym.Src(root, rel)
                src_cache[rel] = s
                owners[rel] = Owners(s) if s.text else None
            except (OSError, ValueError):
                owners[rel] = None
        o = owners[rel]
        if o is None or ln is None or ln < 1:
            return None
        targets.add(rel)
        q, k = o.of(ln)
        if k == 'file':
            s = o.src
            lab = s.label(ln)
            q = lab if lab and not lab.startswith('(file') else q
            k = 'type' if lab.startswith('(in ') else k
            if lab.startswith('(in '):
                q = lab[4:-1]
            elif k == 'file':
                q = '(file scope of %s)' % rel
        return q, k

    for p in sorted(glob.glob(os.path.join(root, 'db', 'claims', '*.json'))):
        try:
            with open(p, encoding='utf-8') as f:
                d = json.load(f)
        except (OSError, ValueError):
            continue
        for r in d if isinstance(d, list) else []:
            if not isinstance(r, dict) or not r.get('ours'):
                continue
            m = re.fullmatch(r'(.+?):(\d+)', str(r['ours']))
            if not m:
                continue
            rel, ln = m.group(1).replace('\\', '/'), int(m.group(2))
            a = norm(r.get('function')) or norm(r.get('address'))
            if not a:
                continue
            off = (r.get('offset') or '').upper().replace('0X', '')
            off = '' if off in ('0', '') else off
            o = owner_at(rel, ln)
            if o is None:
                continue
            txt = re.sub(r'\s+', ' ', str(r.get('claim') or ''))
            rows.append(relate({'src': 'claim', 'ours': o[0], 'kind': o[1], 'file': rel, 'line': ln, 'game': a, 'off': off,
                                'text': (txt[:TEXT_CAP - 3] + '...') if len(txt) > TEXT_CAP else txt}, txt))
    for p in sorted(glob.glob(os.path.join(root, 'db', 'seams', '*.json'))):
        try:
            with open(p, encoding='utf-8') as f:
                d = json.load(f)
        except (OSError, ValueError):
            continue
        if not isinstance(d, dict):
            continue
        topic = d.get('topic') or os.path.splitext(os.path.basename(p))[0]
        games = [(norm(g.get('addr')), re.sub(r'\s+', ' ', str(g.get('what') or ''))) for g in d.get('game') or [] if isinstance(g, dict)]
        games = [(a, w) for a, w in games if a]
        for o_row in d.get('ours') or []:
            if not isinstance(o_row, dict):
                continue
            st = qseam.row_state(o_row, root)
            if st['state'] != 'ok':
                continue
            rel = str(o_row.get('path', '')).replace('\\', '/')
            o = owner_at(rel, st['line'])
            if o is None:
                continue
            why_text = re.sub(r'\s+', ' ', str(o_row.get('why') or ''))
            for a, w in games:
                rows.append(relate({'src': 'seam', 'ours': o[0], 'kind': o[1], 'file': rel, 'line': st['line'], 'game': a, 'off': '',
                                    'text': '[%s] %s' % (topic, w[:TEXT_CAP - 12])}, why_text))
            for a, off, _pos in cites_in(str(o_row.get('why') or '')):
                rows.append(relate({'src': 'seam-why', 'ours': o[0], 'kind': o[1], 'file': rel, 'line': st['line'], 'game': a, 'off': off,
                                    'text': '[%s] %s' % (topic, why_text[:TEXT_CAP - 12])}, why_text))
    return rows, sorted(targets)


def _db_stamp(root):
    import glob
    parts = []
    for sub in ('claims', 'seams'):
        for p in sorted(glob.glob(os.path.join(root, 'db', sub, '*.json'))):
            parts.append('%s=%s' % (os.path.basename(p), _stamp(p)))
    return '|'.join(parts)


# ---------------------------------------------------------------------------------------------------------------- the table

def _walk(root):
    out = {}
    top = os.path.join(root, 'src')
    stack = [top]
    while stack:
        d = stack.pop()
        try:
            it = list(os.scandir(d))
        except OSError:
            continue
        for e in it:
            if e.is_dir(follow_symlinks=False):
                if e.name not in SKIP_DIRS and not e.name.startswith('.'):
                    stack.append(e.path)
            elif e.name.endswith(EXTS):
                try:
                    st = e.stat()
                except OSError:
                    continue
                if st.st_size <= MAX_BYTES:
                    out[os.path.relpath(e.path, root).replace('\\', '/')] = '%d:%d' % (st.st_size, st.st_mtime_ns)
    return out


SQL = """CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE IF NOT EXISTS files(rel TEXT PRIMARY KEY, stamp TEXT, unit TEXT, nfunc INT, deps TEXT);
CREATE TABLE IF NOT EXISTS pairs(src TEXT, ours TEXT, kind TEXT, file TEXT, line INT, unit TEXT, game TEXT, off TEXT, text TEXT, owner TEXT,
                                 rel0 TEXT, why0 TEXT, rel TEXT, why TEXT);
CREATE TABLE IF NOT EXISTS hooks(owner TEXT, fn TEXT, games TEXT, callees TEXT);
CREATE INDEX IF NOT EXISTS hooks_owner ON hooks(owner);
CREATE INDEX IF NOT EXISTS pairs_ours ON pairs(ours);
CREATE INDEX IF NOT EXISTS pairs_game ON pairs(game);
CREATE INDEX IF NOT EXISTS pairs_file ON pairs(file);
CREATE INDEX IF NOT EXISTS pairs_owner ON pairs(owner);"""


def _read(root, rel):
    import read_unit
    try:
        with open(os.path.join(root, rel), 'rb') as f:
            return f.read().decode('utf-8', errors='replace').replace('\r\n', '\n').replace('\r', '\n')
    except OSError:
        return None


def update(root=None, force=False, log=None):
    """bring the table up to date with the files on disk; -> (connection, {'scanned': n, 'removed': n, 'db_rebuilt': bool, 'files': n})"""
    root = _root(root)
    path = db_path(root)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if force and os.path.exists(path):
        try:
            os.remove(path)
        except OSError:
            pass
    con = sqlite3.connect(path, timeout=60)
    con.row_factory = sqlite3.Row
    con.executescript(SQL)
    m = {r['key']: r['value'] for r in con.execute('SELECT key, value FROM meta')}
    import qpairs_rel
    stamp = '%d/rel %s%s' % (SCHEMA, qpairs_rel.VERSION, ' ' + qpairs_rel.PLANT if qpairs_rel.PLANT else '')   # a relation rule change rescans
    if m.get('schema') != stamp:
        con.executescript('DELETE FROM pairs; DELETE FROM files; DELETE FROM meta; DELETE FROM hooks;')
        m = {}
    import qsym
    import read_unit
    qsym._INCLUDER.clear()
    read_unit._DIR_MARKS.clear()
    now = _walk(root)
    have = {r['rel']: (r['stamp'], r['deps']) for r in con.execute('SELECT rel, stamp, deps FROM files')}
    changed = set()
    for rel, st in now.items():
        old = have.get(rel)
        if old is None or old[0] != st:
            changed.add(rel)
            continue
        deps = json.loads(old[1] or '{}')
        if any(_stamp(os.path.join(root, d)) != s for d, s in deps.items()):
            changed.add(rel)
    gone = set(have) - set(now)
    src_cache = {}
    n = 0
    with con:
        for rel in gone:
            con.execute('DELETE FROM pairs WHERE owner = ?', (rel,))
            con.execute('DELETE FROM hooks WHERE owner = ?', (rel,))
            con.execute('DELETE FROM files WHERE rel = ?', (rel,))
        for rel in sorted(changed):
            text = _read(root, rel)
            if text is None:
                continue
            rows, nfunc, hooks = scan_file(root, rel, text, src_cache) if ('sub_8' in text or '0x8' in text or rel.endswith(('.h', '.cpp', '.inc', '.hpp', '.cc', '.inl', '.c', '.glsl'))) else ([], 0, [])
            chain = _chain(root, rel)
            unit = chain[-1] if chain else rel
            deps = {d: _stamp(os.path.join(root, d)) for d in chain}
            con.execute('DELETE FROM pairs WHERE owner = ?', (rel,))
            con.execute('DELETE FROM hooks WHERE owner = ?', (rel,))
            con.executemany('INSERT INTO pairs VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)',
                            [(r['src'], r['ours'], r['kind'], rel, r['line'], unit, r['game'], r['off'], r['text'], rel, r['rel0'], r['why0'], r['rel0'], r['why0'])
                             for r in rows])
            con.executemany('INSERT INTO hooks VALUES(?,?,?,?)', [(rel,) + h for h in hooks])
            con.execute('INSERT OR REPLACE INTO files VALUES(?,?,?,?,?)', (rel, now[rel], unit, nfunc, json.dumps(deps)))
            n += 1
        dbs = _db_stamp(root)
        targets = json.loads(m.get('db_targets') or '[]')
        db_rebuilt = False
        if m.get('db_stamp') != dbs or (changed | gone) & set(targets) or 'db_stamp' not in m:
            rows, targets = _db_rows(root, src_cache)
            con.execute("DELETE FROM pairs WHERE owner = '@db'")
            unit_of = {r['rel']: r['unit'] for r in con.execute('SELECT rel, unit FROM files')}
            con.executemany('INSERT INTO pairs VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)',
                            [(r['src'], r['ours'], r['kind'], r['file'], r['line'], unit_of.get(r['file'], r['file']), r['game'], r['off'], r['text'], '@db',
                              r['rel0'], r['why0'], r['rel0'], r['why0']) for r in rows])
            con.executemany('INSERT OR REPLACE INTO meta VALUES(?,?)', [('db_stamp', dbs), ('db_targets', json.dumps(targets))])
            db_rebuilt = True
        if n or gone or db_rebuilt:
            import qpairs_rel
            qpairs_rel.finish(con)     # rule 3 (a hook's handler) reads every file's hooks: rerun over the table on any change
        con.execute("INSERT OR REPLACE INTO meta VALUES('schema', ?)", (stamp,))
    return con, {'scanned': n, 'removed': len(gone), 'db_rebuilt': db_rebuilt, 'files': len(now)}


# ---------------------------------------------------------------------------------------------------------------- queries

def _rowd(r):
    return {k: r[k] for k in ('src', 'ours', 'kind', 'file', 'line', 'unit', 'game', 'off', 'text', 'rel', 'why')}


def mirrors(r, precise=('comment', 'code', 'claim', 'seam-why')):
    """the one test every diff tool pairs by: a precise source whose relation is MIRRORS (tools/q/qpairs_rel.py)"""
    return r['src'] in precise and r['rel'] == 'MIRRORS'


def fmt_game(r):
    return '0x%s%s' % (r['game'],(' +0x' + r['off']) if r['off'] else '')


def by_ours(con, name):
    """rows of ours whose qualified name is `name` (a `::` suffix of it, or its last component), or every row of a FILE"""
    if '/' in name or name.endswith(EXTS):
        rel = name.replace('\\', '/')
        return [r for r in con.execute('SELECT * FROM pairs WHERE file = ? OR unit = ? ORDER BY file, line, game', (rel, rel))]
    want = [w for w in name.split('::') if w]
    if not want:
        raise PairsError('empty name')
    tail = want[-1]
    out = []
    for r in con.execute('SELECT * FROM pairs WHERE ours = ? OR ours LIKE ? ORDER BY file, line, game', (name, '%' + tail + '%')):
        qs = [x for x in re.sub(r'\(.*$', '', r['ours']).split('::') if x]
        if len(qs) >= len(want) and qs[-len(want):] == want:
            out.append(r)
    return out


def by_game(con, addr):
    a = norm(addr)
    if not a:
        raise PairsError('%s: not a game address (sub_82C30A10, 0x82C30A10 or 82C30A10)' % addr)
    return a, [r for r in con.execute('SELECT * FROM pairs WHERE game = ? ORDER BY ours, file, line', (a,))]


def stats(con):
    one = lambda q, *a: con.execute(q, a).fetchone()[0]
    precise = "src IN ('comment','code','claim','seam-why')"
    s = {
        'src_files': one('SELECT COUNT(*) FROM files'),
        'our_functions': one('SELECT COALESCE(SUM(nfunc),0) FROM files'),
        'pairs': one('SELECT COUNT(*) FROM pairs'),
        'by_source': {r[0]: r[1] for r in con.execute('SELECT src, COUNT(*) FROM pairs GROUP BY src ORDER BY 2 DESC')},
        'by_rel': {r[0]: r[1] for r in con.execute('SELECT rel, COUNT(*) FROM pairs GROUP BY rel ORDER BY 2 DESC')},
        'functions_mirroring': one("SELECT COUNT(*) FROM (SELECT DISTINCT ours, file FROM pairs WHERE kind = 'func' AND rel = 'MIRRORS' AND %s)" % precise),
        'func_pairs_by_rel': {r[0]: r[1] for r in con.execute(
            "SELECT rel, COUNT(*) FROM (SELECT DISTINCT ours, file, game, rel FROM pairs WHERE kind = 'func' AND %s) GROUP BY rel ORDER BY 2 DESC" % precise)},
        'entities_with_cite': one("SELECT COUNT(*) FROM (SELECT DISTINCT ours, file FROM pairs WHERE ours NOT LIKE '(%')"),
        'functions_with_cite': one("SELECT COUNT(*) FROM (SELECT DISTINCT ours, file FROM pairs WHERE kind = 'func')"),
        'functions_with_precise_cite': one("SELECT COUNT(*) FROM (SELECT DISTINCT ours, file FROM pairs WHERE kind = 'func' AND %s)" % precise),
        'functions_with_comment_or_code_cite': one("SELECT COUNT(*) FROM (SELECT DISTINCT ours, file FROM pairs WHERE kind = 'func' AND src IN ('comment','code'))"),
        'by_kind': {r[0]: r[1] for r in con.execute("SELECT kind, COUNT(*) FROM (SELECT DISTINCT ours, file, kind FROM pairs) GROUP BY kind")},
        'file_scope_pairs': one("SELECT COUNT(*) FROM pairs WHERE kind = 'file'"),
        'src_files_with_cite': one('SELECT COUNT(DISTINCT file) FROM pairs'),
        'game_distinct': one('SELECT COUNT(DISTINCT game) FROM pairs'),
        'game_distinct_precise': one('SELECT COUNT(DISTINCT game) FROM pairs WHERE %s' % precise),
        'game_with_offset': one("SELECT COUNT(DISTINCT game || off) FROM pairs WHERE off != ''"),
        'pairs_with_offset': one("SELECT COUNT(*) FROM pairs WHERE off != ''"),
    }
    s['top'] = [{'game': '0x' + r[0], 'ours': r[1], 'pairs': r[2]} for r in con.execute(
        "SELECT game, COUNT(DISTINCT ours || '|' || file), COUNT(*) FROM pairs WHERE kind != 'file' GROUP BY game ORDER BY 2 DESC, 3 DESC LIMIT 15")]
    return s


def print_stats(s, info):
    pct = lambda a, b: ('%.1f%%' % (100.0 * a / b)) if b else 'n/a'
    print('q pairs --stats: %d src files (%d with a citation), %d of our functions (distinct name per file); table %d pairs' %
          (s['src_files'], s['src_files_with_cite'], s['our_functions'], s['pairs']))
    print('  pairs by source: ' + ', '.join('%s %d' % kv for kv in s['by_source'].items()))
    print('  pairs by relation: ' + ', '.join('%s %d' % kv for kv in s['by_rel'].items()) +
          '; (function, game) pairs of the precise sources: ' + ', '.join('%s %d' % kv for kv in s['func_pairs_by_rel'].items()) +
          '; functions with a MIRRORS pair: %d' % s['functions_mirroring'])
    print('  our functions with >= 1 citation: %d of %d (%s)  [comment / code / claim / seam-why = precise: %d (%s); comment / code only: %d (%s)]' %
          (s['functions_with_cite'], s['our_functions'], pct(s['functions_with_cite'], s['our_functions']),
           s['functions_with_precise_cite'], pct(s['functions_with_precise_cite'], s['our_functions']),
           s['functions_with_comment_or_code_cite'], pct(s['functions_with_comment_or_code_cite'], s['our_functions'])))
    print('  (seam rows alone are topic-level: every game row of a topic against each ours row; the precise numbers leave them out)')
    print('  entities by kind: ' + ', '.join('%s %d' % kv for kv in s['by_kind'].items()) + '; pairs with no named owner (file scope): %d' % s['file_scope_pairs'])
    print('  distinct game addresses cited: %d (precise sources only: %d); with an offset: %d distinct addr+offset, %d pairs' %
          (s['game_distinct'], s['game_distinct_precise'], s['game_with_offset'], s['pairs_with_offset']))
    print('  most cited (distinct ours):')
    for t in s['top']:
        print('    %-14s %4d of ours (%d pairs)' % (t['game'], t['ours'], t['pairs']))
    if info:
        print('  (table: %s)' % info)


def print_rows(rows, head, limit, show_ours, show_game):
    print(head)
    for r in rows[:limit]:
        who = ('%-44s ' % r['ours'][:44]) if show_ours else ''
        what = ('%-22s ' % fmt_game(r)) if show_game else ''
        unit = '' if r['unit'] == r['file'] else ' (unit %s)' % r['unit']
        print('  %s%s%-8s %-7s %s:%d%s  %s' % (what, who, r['src'], r['rel'], r['file'], r['line'], unit, r['text']))
        print('  %s(%s)' % (' ' * (len(what) + len(who) + 9), r['why']))
    if len(rows) > limit:
        print('  ... %d more (--limit N or --all)' % (len(rows) - limit))


def main(argv, out=None):
    ap = argparse.ArgumentParser(prog='q pairs', add_help=True)
    ap.add_argument('name', nargs='?', help='our function / type / constant name (Scope::name narrows), or a src file')
    ap.add_argument('--game', metavar='ADDR', help='every one of ours that cites this game function')
    ap.add_argument('--stats', action='store_true')
    ap.add_argument('--coverage', action='store_true', help="q ostate's sites: paired or not, with candidates (tools/q/qpairs_cov.py)")
    ap.add_argument('--no-game', action='store_true', help='--coverage: skip the q gstate read (a cited game function counts paired)')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--root', help='another checkout / a fixture tree (default: this one)')
    ap.add_argument('--rebuild', action='store_true', help='drop the cache and scan everything')
    ap.add_argument('--rel', choices=('MIRRORS', 'WATCHES', 'CITES'), help='keep the rows of one relation class')
    ap.add_argument('--limit', type=int, default=60)
    ap.add_argument('--all', action='store_true')
    a = ap.parse_args(argv)
    limit = 10 ** 9 if a.all else a.limit
    if a.coverage:
        import qpairs_cov
        return qpairs_cov.main(argv, a.root, a.all, a.json, a.no_game)
    if not (a.name or a.game or a.stats):
        ap.error('give OURS_NAME, --game ADDR or --stats')
    con, info = update(a.root, force=a.rebuild)
    try:
        note = 'scanned %d of %d files%s' % (info['scanned'], info['files'], ', db rows rebuilt' if info['db_rebuilt'] else '')
        if a.stats:
            s = stats(con)
            if a.json:
                print(json.dumps({'stats': s, 'table': info}, indent=1))
            else:
                print_stats(s, note)
            return 0
        if a.game:
            addr, rows = by_game(con, a.game)
            rows = [r for r in rows if not a.rel or r['rel'] == a.rel]
            if a.json:
                print(json.dumps({'game': 'sub_' + addr, 'n': len(rows), 'pairs': [_rowd(r) for r in rows]}, indent=1))
                return 0
            print_rows(rows, 'sub_%s: cited by %d of ours (%d pairs)' % (addr, len({(r['ours'], r['file']) for r in rows}), len(rows)), limit, True, False)
            if not rows:
                print('  (no citation; the cache is %s)' % note)
            return 0
        rows = [r for r in by_ours(con, a.name) if not a.rel or r['rel'] == a.rel]
        if a.json:
            print(json.dumps({'ours': a.name, 'n': len(rows), 'pairs': [_rowd(r) for r in rows]}, indent=1))
            return 0
        print_rows(rows, '%s: %d pairs, %d game addresses, %d matching entities' % (
            a.name, len(rows), len({r['game'] for r in rows}), len({(r['ours'], r['file']) for r in rows})), limit, True, True)
        if not rows:
            print('  (no pair: nothing in a comment, a claim or a seam row ties this name to a game address; the cache is %s)' % note)
        return 0
    finally:
        con.close()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
