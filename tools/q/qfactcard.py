"""qfactcard: print the settled facts INSIDE the outputs agents already read (S111 Lane A FACTCARD, docs/research/s111/INDEX_PLAN.md T1).

One merged card per function, built from db/claims/*.json (the lodged claim rows):
  * a row is attached to the SITE (function + offset) it is about: a typed row (Lane B's `kind` / `key` / `name` / `site` fields, lodge_note.py)
    by its `site` (kind site) or its own address; an untyped row by its own `offset` column and the `sub_X +OFF` its evidence text cites (parse_sites);
  * rows whose sites lie close together form one cluster (one line: the function's own newest [C] row, `xN` notes, `rev` once reviewed), printed on
    that row's own cited offset; a row about another function that cites this one says so: `NOTE (about sub_X)`;
  * [L] rows print only under `leads:` in the header, never as a block comment;
  * two [C] rows on one typed `key` (any functions) that name it differently print DISPUTED and are never merged;
  * a row whose explicit offset is not an instruction of its function prints STALE.
Printers: header (<= 8 lines of <= 160 chars), one `; [C] NOTE: ...` comment (<= 100 chars) before the block holding a cited offset
(q decomp), the facts inside an asm window (q asm --at), one fact per caller (q callers), the claims citing each access site (q field).
`--facts none|all` (default: the capped card).

PROVIDER API (Lanes C / D / E plug their own facts into the same printing, without editing qdecomp.py):
    import qfactcard
    qfactcard.register('when', fn, where=('decomp', 'asm'))     # kind (a label), fn, the printers that call it
    fn(ctx) -> iterable of qfactcard.Fact                       # ctx.addr (int), ctx.size (int or None), ctx.con, ctx.where
    Fact(tag, text, off=None, hi=None, prio=50, weight=1)
        tag    the bracket label printed: 'C', 'L', 'when', 'enum', 'vcall' ...
        text   one line, no newline (the printer cuts it: 160 chars in the header, 100 in a block comment)
        off    byte offset from the function start, or None = a header line (function level); hi = end of a range (inclusive)
        prio   header order, high first (claims: DISPUTED 100, STALE 95, summary 90, function-level 80, sites 70, leads 60; default 50)
        weight which block comments survive the byte budget (more first)
A provider that raises prints one `[kind] provider failed` header line; it never hides.  Selftest: py -I tools\\q\\qfactcard_selftest.py
"""
import os
import re
import sys
import time
from collections import namedtuple

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER_LINES = 8
HEADER_CHARS = 160
COMMENT_CHARS = 100
CLUSTER_GAP = 0x20       # two cited offsets this close (bytes) are one site cluster
CLUSTER_MAX = 0x60       # a cluster never grows wider than this
BUDGET_MIN = 300         # bytes of header + comments a decomp may carry at least (a tiny function still gets its one fact) ...
BUDGET_FRAC = 0.15       # ... and this share of the plain output (M7: +15% at most)
MAX_CALLER_FACTS = 30

Fact = namedtuple('Fact', 'tag text off hi prio weight')
Fact.__new__.__defaults__ = (None, None, 50, 1)
Ctx = namedtuple('Ctx', 'addr size con where mode')

_PROVIDERS = []          # [(kind, fn, where)]
_ROWS = []               # injected rows (selftest) or [] = q.load_claims()
_INDEX = []


def register(kind, fn, where=('decomp', 'asm')):
    """add a provider; re-registering a kind replaces it"""
    _PROVIDERS[:] = [p for p in _PROVIDERS if p[0] != kind]
    _PROVIDERS.append((kind, fn, tuple(where)))


def add_arg(ap):
    ap.add_argument('--facts', choices=('none', 'all', 'card'), default='card',
                    help='S111 FACTCARD: settled facts printed in this output: card = the capped card (default), all = every row, none = off')


def mode_of(args):
    return getattr(args, 'facts', None) or 'card'


def set_rows(rows):
    """selftest: use these claim rows instead of db/claims (None = back to the files)"""
    _ROWS[:] = [rows] if rows is not None else []
    _INDEX[:] = []


# ------------------------------------------------------------------ rows and sites
_VERB = re.compile(r'\b(decomp|fn|asm|callers|listing|field|args|order|q)\b(?:\s+--?\w+)*\s+(sub_[0-9A-Fa-f]{8})')
_OFF = re.compile(r'\+(0[xX][0-9A-Fa-f]+|[0-9A-Fa-f]{2,4})(?![\w])(?:\.\.\+?(0[xX][0-9A-Fa-f]+|[0-9A-Fa-f]{2,4})(?![\w]))?')
_ABS = re.compile(r'0[xX](8[0-9A-Fa-f]{7})(?:(?:\.\.|-)0[xX](8[0-9A-Fa-f]{7}))?')      # an absolute listing address: 0x82C056D4, 0x82C056D4..0x82C057BC or -0x82C057BC
_SUBOFF = re.compile(r'sub_([0-9A-Fa-f]{8})\+(0[xX][0-9A-Fa-f]+|[0-9A-Fa-f]{2,4})(?![\w])')
_BARE = re.compile(r'\bsub_([0-9A-Fa-f]{8})\b(?!\+)')                  # a function named with no verb
_BAREVERB = re.compile(r'\b(?:decomp|asm|listing)\s+(?=\+)')           # `(decomp +0x0..+0x2C)`: the function the segment named last
_WORD_BEFORE = re.compile(r'([A-Za-z_]\w*)[\s(]*$')
# a word before `+OFF` that makes it a field / slot offset, not a site: `vtable +124`, `record +0x90`, `the model word +368`
_FIELD_WORDS = frozenset(('vtable', 'vt', 'slot', 'vslot', 'vcall', 'word', 'words', 'field', 'handle', 'record', 'model', 'mesh', 'char',
                          'character', 'node', 'part', 'entry', 'offset', 'bank', 'table', 'info', 'object', 'member', 'element', 'param',
                          'index', 'struct'))
# what may stand between two cited offsets of one chain: separators, a callee's name and closed parentheses (one level of nesting)
_CHAIN = re.compile(r'(?:[\s,/]|\.\.|sub_[0-9A-Fa-f]{8}\b|\((?:[^()]|\([^()]*\))*\))*')


def _num(s):
    return int(s, 16)


def _paren_loose(text):
    """[(start, end)] of the parenthesised stretches that hold '=' or '[': prose about registers / fields, where only 0x offsets count"""
    out, stack = [], []
    for i, c in enumerate(text):
        if c == '(':
            stack.append(i)
        elif c == ')' and stack:
            s = stack.pop()
            seg = text[s:i]
            if '=' in seg or '[' in seg:
                out.append((s, i))
    return out


def parse_sites(evidence, own):
    """evidence text (an UNTYPED row) -> [(func, lo, hi, strict)]: offsets cited as `decomp sub_X +OFF / +OFF2, +A..+B`. The owner of an offset is
    the last `<verb> sub_X` before it (`callers sub_X`: none, its sites are in the callers), or a bare `sub_Y +OFF`; a verb with no function
    (`(decomp +0x0..+0x2C)`) means the function the segment named last; any other bare sub_X is a callee. Not a site: an offset after a field
    word (`vtable +124`, `record +0x90`), and a decimal-looking one (`+680`) that is not chained to the owner or a cited offset by separators and
    closed parentheses only. strict = written 0x..: a wrong one is STALE."""
    out = []
    owner = own
    for seg in str(evidence or '').split(';'):
        loose = _paren_loose(seg)
        marks, taken = [], []
        for m in _VERB.finditer(seg):
            marks.append((m.start(2), m.end(2), 'o', None if m.group(1) == 'callers' else _num(m.group(2)[4:])))
            taken.append((m.start(2), m.end(2)))
        for m in _SUBOFF.finditer(seg):
            marks.append((m.start(), m.end(), 's', m))
            taken.append((m.start(), m.end()))
        for m in _BARE.finditer(seg):
            if not any(s <= m.start() < e for s, e in taken):
                marks.append((m.start(), m.end(), 'b', _num(m.group(1))))
        for m in _BAREVERB.finditer(seg):
            marks.append((m.start(), m.end(), 'v', None))
        for m in _OFF.finditer(seg):
            marks.append((m.start(), m.end(), 'x', m))
        for m in _ABS.finditer(seg):
            marks.append((m.start(), m.end(), 'a', m))
        marks.sort(key=lambda t: t[0])
        anchor, last_bare = 0, None
        for pos, end, kind, v in marks:
            if kind == 'o':
                owner, anchor = v, end
            elif kind == 'b':
                last_bare = v
                if re.match(r'\s+\+', seg[end:]) and not re.search(r'\+(?:0[xX])?[0-9A-Fa-f]+\s*$', seg[:pos]):    # `+A0 sub_Y +B0`: a callee label
                    owner, anchor = v, end
            elif kind == 'v':
                if last_bare is not None:
                    owner = last_bare
                anchor = end
            elif kind == 's':
                lo = _num(v.group(2))
                if lo % 4 == 0 and lo >= 4:
                    out.append((_num(v.group(1)), lo, lo, v.group(2)[:2].lower() == '0x'))
                anchor = end
            elif kind == 'a':
                if owner is not None:
                    lo = _num(v.group(1)) - owner
                    hi = _num(v.group(2)) - owner if v.group(2) else lo
                    if 4 <= lo <= hi < 0x40000 and lo % 4 == 0 and hi % 4 == 0:
                        out.append((owner, lo, hi, False))
                        anchor = end
            else:
                if owner is None:
                    continue
                a, b = v.group(1), v.group(2)
                hexed = a[:2].lower() == '0x'
                prev = seg[pos - 1] if pos > 0 else ' '
                if prev not in ' ,/(;':
                    continue
                w = _WORD_BEFORE.search(seg[:pos])
                if w and w.group(1).lower() in _FIELD_WORDS:
                    continue
                if not hexed and (prev == '(' or any(s < pos < e for s, e in loose) or not _CHAIN.fullmatch(seg, anchor, pos)):
                    continue
                lo = _num(a)
                hi = _num(b) if b else lo
                if hi < lo or lo % 4 or (b and hi % 4):
                    continue
                if not hexed and lo < 4:
                    continue
                out.append((owner, lo, hi, hexed))
                anchor = end
    return out


def _sess(note, file):
    m = re.search(r'/s(\d+)/', str(note or '').replace('\\', '/')) or re.search(r's(\d+)_', str(file or ''))
    return int(m.group(1)) if m else 0


def _stem(note):
    return os.path.splitext(os.path.basename(str(note or '?').replace('\\', '/')))[0]


def _type_of(r):
    """Lane B's TYPE fields of a lodged row (lodge_note.py writes kind / key (canonical, tools/q/qfactkey.py) / name, and `site` = the instruction
    address of a kind-site row) -> (kind, key, name); an untyped row -> (None, None, '')"""
    k = r.get('kind')
    if not k:
        return None, None, ''
    return str(k), (str(r.get('key') or '') or None), str(r.get('name') or '')


def _typed_site(r, key):
    """a kind-site row -> (func, offset) from its `site` field (the function from its key `sub_X+OFF`), or None"""
    m = re.match(r'sub_([0-9A-Fa-f]{8})\+', str(key or ''))
    try:
        s = int(str(r.get('site')), 16)
    except (TypeError, ValueError):
        return None
    return (_num(m.group(1)), s - _num(m.group(1))) if m else None


class Row:
    __slots__ = ('func', 'mark', 'text', 'note', 'rev', 'kind', 'key', 'name', 'offset', 'sites', 'sup', 'order', 'tkey')

    def __init__(self, r, order, mt):
        try:
            self.func = int(str(r.get('function')), 16)
        except (TypeError, ValueError):
            self.func = None
        self.mark = str(r.get('mark') or '?')
        self.text = ' '.join(str(r.get('claim') or '').split())
        self.note = _stem(r.get('note'))
        self.rev = bool(r.get('rev'))        # only the computed mark (tools/handback/rev_mark.py)
        self.kind, self.key, self.name = _type_of(r)
        try:
            self.offset = int(str(r.get('offset') or '0x0'), 16)
        except ValueError:
            self.offset = 0
        self.tkey = (_sess(r.get('note'), r.get('file')), mt, order)       # newest = largest
        self.order = order
        self.sup = str(r.get('supersedes') or '')
        sites = []
        if self.func is not None:     # a typed row is placed where Lane B's fields say (qfactkey.facts_at_site); the evidence parse is for untyped rows
            ts = _typed_site(r, self.key) if self.kind == 'site' else None
            if ts is not None:
                sites.append((ts[0], ts[1], ts[1], True))
            elif self.offset:
                sites.append((self.func, self.offset, self.offset, True))
            if not self.kind:
                sites += parse_sites(r.get('evidence'), self.func)
        self.sites = sites


class FuncCard:
    def __init__(self, func):
        self.func = func
        self.rows = []         # every row printed for this function: [(Row, [(lo, hi, strict)])]  (an empty site list = function level)


def _build():
    if _INDEX:
        return _INDEX[0]
    if _ROWS:
        rows = _ROWS[0]
    else:
        if HERE not in sys.path:
            sys.path.insert(0, HERE)
        import q
        rows = q.load_claims()
    mts = {}
    cards = {}
    keys = {}            # Lane B key -> [Row]: DISPUTED is a property of the key, whichever functions its rows sit on
    for i, r in enumerate(rows):
        f = str(r.get('file') or '')
        if f not in mts:
            try:
                mts[f] = os.path.getmtime(os.path.join(os.path.dirname(HERE), '..', 'db', f))
            except OSError:
                mts[f] = 0
        row = Row(r, i, mts[f])
        if row.func is None:
            continue
        if row.key:
            keys.setdefault(row.key, []).append(row)
        by = {}
        for fn, lo, hi, strict in row.sites:
            by.setdefault(fn, []).append((lo, hi, strict))
        by.setdefault(row.func, [])
        for fn, sites in by.items():
            cards.setdefault(fn, FuncCard(fn)).rows.append((row, sites))
    _INDEX.extend((cards, keys))
    return cards


def _keyed():
    _build()
    return _INDEX[1]


# ------------------------------------------------------------------ clusters
class Cluster:
    def __init__(self, lo, hi):
        self.lo, self.hi = lo, hi
        self.rows = []       # [Row]
        self.points = []     # [(lo, hi)]


def _clusters(rowsites):
    pts = sorted(((lo, hi, row) for row, sites in rowsites for lo, hi, _s in sites), key=lambda t: (t[0], t[1]))
    out = []
    for lo, hi, row in pts:
        c = out[-1] if out else None
        if c is not None and lo <= c.hi + CLUSTER_GAP and max(hi, c.hi) - c.lo <= CLUSTER_MAX:
            c.hi = max(c.hi, hi)
        else:
            c = Cluster(lo, hi)
            out.append(c)
        c.points.append((lo, hi))
        if row not in c.rows:
            c.rows.append(row)
    return out


def _span(c):
    return f'+0x{c.lo:X}' if c.lo == c.hi else f'+0x{c.lo:X}..+0x{c.hi:X}'


def _live(rows):
    """drop rows another row supersedes (`supersedes: NOTE ...`): the old row is struck, it does not print"""
    struck = set()
    for r in rows:
        if r.sup:
            stem = _stem(r.sup.split()[0]) if r.sup.split() else ''
            for o in rows:
                if o is not r and o.note == stem and o.tkey < r.tkey:
                    struck.add(o.order)
    return [r for r in rows if r.order not in struck], len(struck)


def _newest(rows, mark=None):
    rs = [r for r in rows if mark is None or r.mark == mark]
    return max(rs, key=lambda r: (r.tkey, -len(r.sites))) if rs else None


def _clip(s, n):
    s = ' '.join(str(s).split())
    return s if len(s) <= n else s[:max(n - 3, 0)] + '...'


def _label(rows):
    notes = []
    for r in sorted(rows, key=lambda r: r.tkey, reverse=True):
        if r.note not in notes:
            notes.append(r.note)
    return notes


# ------------------------------------------------------------------ the claims provider
def _at(c, sites):
    """the row's own cited points inside cluster c -> (lo, hi): its comment sits there, not on another row's point earlier in the cluster"""
    p = [(lo, hi) for lo, hi, _s in sites if c.lo <= lo <= c.hi]
    return (min(lo for lo, _h in p), max(h for _l, h in p)) if p else (c.lo, c.hi)


def _who(r, addr):
    """the note label of a row: `NOTE`, or `NOTE (about sub_X)` for a row about another function that cites this one"""
    return r.note if r.func == addr else f'{r.note} (about sub_{r.func:08X})'


def claims_provider(ctx):
    card = _build().get(ctx.addr)
    if card is None:
        return []
    allrows = []
    for row, _s in card.rows:
        if row not in allrows:
            allrows.append(row)
    live, nstruck = _live(allrows)
    livei = {r.order for r in live}
    facts = []
    size = ctx.size
    # STALE: an explicit offset of this function's own row that is no instruction of it (another function's row citing past the end is parse noise)
    stale = []
    if size:
        for row, sites in card.rows:
            if row.order not in livei or row.func != ctx.addr:
                continue
            for lo, hi, strict in sites:
                if strict and hi >= size:
                    stale.append((row, lo))
    # sites inside the function (an offset past the end of a function whose size is known is parse noise: dropped, or STALE above); a row about
    # another function is in this card only through a site inside this one
    rs = []
    for row, sites in card.rows:
        if row.order not in livei:
            continue
        ok = [(lo, hi, s) for lo, hi, s in sites if not size or hi < size]
        if ok or row.func == ctx.addr:
            rs.append((row, ok))
    pts_of = {r.order: s for r, s in rs}
    nC = len({id(r) for r, _ in rs if r.mark == 'C'})
    nL = len({id(r) for r, _ in rs if r.mark == 'L'})
    notes = _label([r for r, _ in rs])
    nrev = len({id(r) for r, _ in rs if r.rev})
    # DISPUTED: two live [C] rows on one typed key (Lane B's canonical `key`, any functions) that name it differently
    keyed = {}
    allkeys = _keyed()
    for k in sorted({r.key for r, _ in rs if r.key and r.mark == 'C'}):
        for r in _live(allkeys.get(k, []))[0]:
            if r.mark == 'C':
                keyed.setdefault(k, {}).setdefault(r.name, []).append(r)
    disputed = {k: v for k, v in keyed.items() if len({n for n in v if n}) > 1}
    cl_all = _clusters([(r, s) for r, s in rs if s])
    head = (f'{len(rs)} rows, {len(notes)} notes ([C] {nC}, [L] {nL}), {nrev} reviewed; {len(cl_all)} sites'
            + (f'; DISPUTED {len(disputed)}' if disputed else '') + (f'; STALE {len(stale)}' if stale else '')
            + (f'; {nstruck} superseded' if nstruck else '') + '  (q decomp FUNC --facts all = every row)')
    facts.append(Fact('facts', head, None, None, 90))
    for k, v in sorted(disputed.items()):
        facts.append(Fact('DISPUTED', k + ': ' + ' vs '.join(f'{n or "?"} ({", ".join(sorted({r.note for r in rr}))})' for n, rr in sorted(v.items())), None, None, 100))
    for row, lo in stale[:2]:
        facts.append(Fact('STALE', f'{row.note} cites +0x{lo:X}: not an instruction of this function (size 0x{size:X}): {_clip(row.text, 70)}', None, None, 95))
    # function level: rows with no site here
    stale_ids = {r.order for r, _lo in stale}
    fl = [r for r, s in rs if not s and r.order not in stale_ids]
    allm = ctx.mode == 'all'
    flc = [r for r in fl if r.mark == 'C']
    nr = _newest(flc)
    if nr is not None and not allm:
        nn = len(_label(flc))
        facts.append(Fact('C rev' if nr.rev else 'C', f'{nr.note}{" x%d notes" % nn if nn > 1 else ""}: {nr.text}', None, None, 80))
    elif allm:
        for r in sorted(flc, key=lambda r: r.tkey, reverse=True):
            facts.append(Fact('C rev' if r.rev else 'C', f'{r.note}: {r.text}', None, None, 80))
    # leads: the [L] rows
    ls = [r for r, _ in rs if r.mark in ('L', 'U')]
    if ls:
        lead = sorted(ls, key=lambda r: r.tkey, reverse=True)
        if allm:
            for r in lead:
                facts.append(Fact('leads', f'[{r.mark}] {_who(r, ctx.addr)}: {r.text}', None, None, 60))
        else:
            facts.append(Fact('leads', ' | '.join(f'[{r.mark}] {_who(r, ctx.addr)}: {_clip(r.text, 60)}' for r in lead[:3]) + (f' | +{len(lead) - 3} more' if len(lead) > 3 else ''),
                              None, None, 60))
    # sites: one block comment per cluster that holds a [C] row; and one header line listing them
    parts = []
    used = set()      # a wide row cites many clusters: each cluster shows the newest row no other cluster shows yet (the most constrained cluster first)
    todo = sorted(((c, [r for r in c.rows if r.mark == 'C']) for c in cl_all), key=lambda t: (len(t[1]), t[0].lo))
    shown = {}
    lead_of = {}
    for r, s in rs:
        for c in cl_all:
            if s and c.lo <= s[0][0] <= c.hi:
                lead_of[r.order] = c.lo
    for c, cs in todo:
        if cs:
            lead = {r.order: lead_of.get(r.order) for r in cs}      # a row is about the site it cites first: that cluster shows it before any other
            nw = max(cs, key=lambda r: (r.func == ctx.addr, lead[r.order] == c.lo, r.order not in used, r.tkey, -len(r.sites)))
            used.add(nw.order)
            shown[c.lo] = nw
    for c in cl_all:
        cs = [r for r in c.rows if r.mark == 'C']
        if not cs:
            continue
        nw = shown[c.lo]
        nl = _label(cs)
        rev = any(r.rev for r in cs)
        tag = 'C rev' if nw.rev else 'C'      # `rev` is the shown row's own mark: a reviewer may take that line as read
        parts.append((len(nl), c, f'{_span(c)} x{len(nl)}' + (' rev' if rev else '')))
        if allm:
            for r in sorted(cs, key=lambda r: r.tkey, reverse=True):
                lo, hi = _at(c, pts_of.get(r.order, []))
                facts.append(Fact('C rev' if r.rev else 'C', f'{_who(r, ctx.addr)}: {r.text}', lo, hi, 70, 1))
        else:
            lo, hi = _at(c, pts_of.get(nw.order, []))
            facts.append(Fact(tag, _who(nw, ctx.addr) + (f' x{len(nl)}' if len(nl) > 1 else '') + f': {nw.text}', lo, hi, 70, len(nl) * 4 + len(cs)))
    parts.sort(key=lambda t: -t[0])
    if parts and not allm:
        facts.append(Fact('sites', ' | '.join(p[2] for p in parts), None, None, 70))
    return facts


register('claims', claims_provider, where=('decomp', 'asm', 'callers', 'field'))
register('when', lambda ctx: __import__('qwhen').facts(ctx), where=('decomp', 'asm'))      # S111 WHEN (Lane C): tools/q/qwhen.py
register('notes', lambda ctx: __import__('qnoteseam').notes_facts(ctx), where=('decomp', 'asm'))      # S111 NOTESEAM (qnoteseam.py): the lane notes that name the function
register('seams', lambda ctx: __import__('qnoteseam').seams_facts(ctx), where=('decomp', 'asm'))      # ... and the db/seams topics at it
import qenum  # noqa: E402,F401  (S111 Lane D ENUMS: registers the 'enum' provider: a literal argument at a call the table names, inline)
register('vcall', lambda ctx: __import__('qvtarget').provider(ctx), where=('decomp', 'asm'))      # S111 VCALL (qvtarget.py): the known targets at each vtable call site


# ------------------------------------------------------------------ collecting and printing
def collect(addr, con=None, size=None, where='decomp', mode='card'):
    ctx = Ctx(addr, size, con, where, mode)
    out = []
    for kind, fn, wh in list(_PROVIDERS):
        if where not in wh:
            continue
        try:
            out += list(fn(ctx) or [])
        except Exception as e:      # a failing provider is shown, not hidden
            out.append(Fact(kind, f'provider failed: {type(e).__name__}: {e}', None, None, 99))
    return out


def header_lines(facts, mode):
    if mode == 'none':
        return []
    hs = sorted([f for f in facts if f.off is None], key=lambda f: -f.prio)
    if mode == 'all':
        return [_fmt(f) for f in hs]
    return [_clip(_fmt(f), HEADER_CHARS) for f in hs[:HEADER_LINES]]


_LABELS = ('facts', 'sites', 'leads', 'DISPUTED', 'STALE', 'notes', 'seams')


def _fmt(f):
    return f'; {f.tag}: {f.text}' if f.tag in _LABELS else f'; [{f.tag}] {f.text}'


def _span_of(f):
    return f'+0x{f.off:X}' if f.hi in (None, f.off) else f'+0x{f.off:X}..+0x{f.hi:X}'


def comments(facts, mode, header_bytes=0, base_bytes=0):
    """-> [(lo, hi, text)] block comments, the heaviest first, within the byte budget"""
    if mode == 'none':
        return []
    cs = sorted([f for f in facts if f.off is not None and f.tag != 'sites'], key=lambda f: (-f.weight, f.off))
    out = []
    if mode == 'all':
        return [(f.off, f.hi if f.hi is not None else f.off, _fmt(f)) for f in cs]
    budget = max(int(base_bytes * BUDGET_FRAC), BUDGET_MIN) - header_bytes
    used = 0
    for f in cs:
        t = _clip(_fmt(f), COMMENT_CHARS)
        if used + len(t) + 1 > budget:
            continue
        used += len(t) + 1
        out.append((f.off, f.hi if f.hi is not None else f.off, t))
    return out


_RANK = {'DISPUTED': 100, 'STALE': 95, 'leads': 60, 'sites': 50, 'facts': 40}


def _comment_rank(f):
    """a block comment's rank in the card: the claims' own [C] comments at 80 (above leads / sites / facts), another provider's comment at 30, BELOW
    every claim header (S111 VCALL review: vcall [L] comments evicted the facts / sites / leads lines of 6 of the 37 most-read decomps). The heavier first."""
    return (80 if f.tag in ('C', 'C rev') else 30) + min(f.weight, 99) / 100.0


def _allocate(facts, base_bytes, place, indent):
    """the card within the byte budget max(15% of the plain output, BUDGET_MIN): DISPUTED, STALE, the function-level [C], then the claims' block comments
    (the heaviest cluster first), then leads, the site list and the count line, then other providers' comments (_comment_rank: a provider comment never
    takes the place of a claim header). place(lo, hi) -> the body line index a comment sits before, or None (it prints
    nowhere and costs nothing); a comment costs its text, its newline and the indentation of that line. -> (header lines in prio order,
    [(line index, text)] comments). The first fact always fits."""
    budget = max(int(base_bytes * BUDGET_FRAC), BUDGET_MIN)
    cand = []
    for f in facts:
        if f.off is None:
            rank = _RANK.get(f.tag, 90 if f.tag.startswith('C') else f.prio)
            cand.append((rank, f, _clip(_fmt(f), HEADER_CHARS), None))
        elif f.tag != 'sites':
            i = place(f.off, f.hi if f.hi is not None else f.off)
            if i is not None:
                cand.append((_comment_rank(f), f, _clip(_fmt(f), COMMENT_CHARS), i))
    cand.sort(key=lambda t: -t[0])
    used, hs, cs = 0, [], []
    for _rank, f, text, i in cand:
        cost = len(text) + 1 + (indent(i) if i is not None else 0)
        if (hs or cs) and (used + cost > budget or (f.off is None and len(hs) >= HEADER_LINES)):
            continue
        used += cost
        if f.off is None:
            hs.append((f, text))
        else:
            cs.append((i, text))
    return [t for f, t in sorted(hs, key=lambda x: -x[0].prio)], cs


_LINE_OFF = re.compile(r'^\s*\+([0-9A-F]+)\s')
_LINE_MARK = re.compile(r'//\s*\+([0-9A-F]+)[,\s]')


def apply(lines, facts, mode):
    """decomp body lines -> (header lines, body lines with the block comments `; [C] NOTE: ...` before the line holding the cited offset);
    lines = None (q decomp --fields): the header only"""
    if mode == 'none' or not facts:
        return [], list(lines or [])
    if lines is None:
        return header_lines(facts, mode), []
    offs = []
    for i, l in enumerate(lines):
        m = _LINE_OFF.match(l) or _LINE_MARK.search(l)
        if m:
            offs.append((int(m.group(1), 16), i))

    def place(lo, hi):
        cand = [(o, i) for o, i in offs if o >= lo]
        if not cand:
            return None
        o, i = min(cand)
        return None if o > hi + CLUSTER_GAP else i

    if mode == 'all':
        hdr = header_lines(facts, mode)
        cm = [(i, text) for lo, hi, text in comments(facts, mode) for i in [place(lo, hi)] if i is not None]
    else:
        hdr, cm = _allocate(facts, sum(len(l) + 1 for l in lines), place, lambda i: len(lines[i]) - len(lines[i].lstrip()))
    put = {}
    for i, text in cm:
        put.setdefault(i, []).append(text)
    out = []
    for i, l in enumerate(lines):
        for t in put.get(i, []):
            out.append(re.match(r'\s*', l).group(0) + t)
        out.append(l)
    return hdr, out


def window_marks(facts, lo, hi, mode, limit=12):
    """asm --at: [(offset, text)] for the cited clusters that overlap the window [lo, hi] (byte offsets)"""
    if mode == 'none':
        return []
    out = []
    for f in sorted([f for f in facts if f.off is not None and f.tag != 'sites'], key=lambda f: f.off):
        fh = f.hi if f.hi is not None else f.off
        if fh >= lo and f.off <= hi:
            out.append((f.off, _clip(f'; [{f.tag}] {_span_of(f)} {f.text}', HEADER_CHARS if mode != 'all' else 4000)))
    return out[:limit] if mode != 'all' else out


def site_fact(facts, off, mode, label=''):
    """the one fact citing the site `off` (q field access rows, q callers call sites) or None; label = what the line is about (`caller +0x3D8: `)"""
    if mode == 'none':
        return None
    for f in sorted([f for f in facts if f.off is not None and f.tag != 'sites'], key=lambda f: -f.weight):
        if f.off <= off <= (f.hi if f.hi is not None else f.off):
            return _clip(f'; [{f.tag}] {label}{f.text}', HEADER_CHARS)
    return None


def fn_fact(facts, mode, label=''):
    """the one function-level fact for a caller line: the function's own [C] line, else its heaviest cited site"""
    if mode == 'none':
        return None
    for f in sorted([f for f in facts if f.off is None and f.tag in ('C', 'C rev')], key=lambda f: -f.prio):
        return _clip(f'; [{f.tag}] {label}{f.text}', HEADER_CHARS)
    cs = sorted([f for f in facts if f.off is not None and f.tag != 'sites'], key=lambda f: -f.weight)
    return _clip(f'; [{cs[0].tag}] {label}{_span_of(cs[0])} {cs[0].text}', HEADER_CHARS) if cs else None
