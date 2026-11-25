r"""q read claims (S110 lane VERIFYMECH): the MECHANICAL check of the claims inside a stored reading, at zero tokens.

    py tools/q/q.py read claims --build                 check every reading; the results replace table claim_read in read.s2.db
    py tools/q/q.py read claims --stats                 readings: all checkable claims checked / at least one refuted / nothing checkable
    py tools/q/q.py read claims UNIT|ADDR               the claim rows of a unit's readings (and of its functions' proposed names)
    py tools/q/q.py read claims --promote [--out TSV]   the name proposals the promotion rule WOULD promote (a list; never written to db/)

A reading is a unit_read row (its `because:` is checked against every member of the unit) or a name_read row (its evidence is
checked against that one function). Claim kinds, each a sentence form the sheet-reader copies from q sheet lines:
  str          str "TEXT"            a string the subject references (index strrefs + strings; a cut ~ / ... matches a prefix)
  calls        calls / out NAME|ADDR a direct callee (edges call/tail), a static indirect target (vcall icall_target) or an observed one (seen)
  only_callee  out only / only callee is NAME   the direct callees (less the __save/__rest helpers a sheet hides) are that one
  caller       called from|by / caller(s) NAME|ADDR   a direct caller, an observed caller or a static indirect site's function
  only_caller  called only from|by NAME   the direct callers are a subset of those named
  n_callers    N callers             N = distinct direct callers, those outside the unit, or those inside it
  vslot        vtADDR[+OFF]          a data word at ADDR+OFF points at the subject (index ptrs)
  vslot_class  vtADDR[+OFF] Class    the RTTI class of that table is Class or has Class as a base (index rtti_*)
  field        writes|reads [this|param N] +N[/+M..], writes|reads [global] 0xADDR [+N]   a row of fields.s2.db (rw W / R)
  mention      a quoted function name in a unit's because: a member, a callee or a caller of a member
  name_vs_db   the proposed name shares a word of 4+ letters with the function's HIGH db name (refuted: none shared)
  name_vs_game the same against the names the game carries (names.s2.db, not fn_string_shared)
A clause whose subject is another function ("called by X which writes ...", "reached from vtA[+4]") is not read as a claim.
A quoted list item that is a copied sheet line ("829BDE38 writes +108", "vslot vt82022900[+0]") or one quoted function name is read;
any other quoted text is a game string. An abbreviated str quote ("W/H", "2..5", "A ... B", \"-escapes) checks each literal run.
A bare "writes 0xV" may name a value written (a vtable): when the index has no such access it is not_checkable, never refuted.
Verdicts: checked | refuted (with the index fact that refutes it) | not_checkable (the form was seen, the name it rests on resolves to
no function). A reading with no recognised claim gets one row kind '-' verdict not_checkable.
SOUNDNESS: a refuted claim is a FLAG for a reader (the index may have changed since the reading, the reader may have meant a
neighbour); it lowers no confidence and never edits a reading or db/*.json. checked means "the index holds that fact", never that
the reading's purpose or name is right. claim_read is derived: --build deletes and rewrites it whole.
MAY BE INCOMPLETE: only the forms above are read; prose ("loops over", "like X", "listing shows ...") is not a claim here.
"""
import collections
import glob
import json
import os
import re
import sqlite3
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qnames  # noqa: E402
import qread  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
KINDS = ('str', 'calls', 'only_callee', 'caller', 'only_caller', 'n_callers', 'vslot', 'vslot_class', 'field', 'mention',
         'name_vs_db', 'name_vs_game')
VERDICTS = ('checked', 'refuted', 'not_checkable')
TABLE = ('create table if not exists claim_read(id integer primary key autoincrement, read_kind text, read_id integer, target integer,'
         ' kind text, claim text, verdict text, fact text, when_ text);'
         'create index if not exists claim_read_r on claim_read(read_kind, read_id);')
# q sheet printed the [p:kind] pattern tag from this commit on (c8680266e, S110 PATTERN): a reading stored before it never saw one
PATTERN_ON_SHEET = '2026-11-23 01:32:29'
# a pattern kind -> the name words that say the same role (a second label for the ROLE only, never for the whole name)
PATTERN_WORDS = {
    'deleting_dtor': {'dtor', 'destructor', 'destroy', 'delete', 'deleting'},
    'this_adjust_thunk': {'thunk', 'adjust', 'adjustor'},
    'forward_thunk': {'thunk', 'forward', 'forwarder', 'wrapper', 'tail', 'jump'},
    'component_getter': {'get', 'getter', 'find', 'component'},
    'sequence_tick': {'tick', 'update', 'step'},
    'render_node_draw': {'draw', 'render'},
    'thread_entry': {'thread', 'entry', 'proc', 'main', 'worker'},
    'read_end_process': {'readend', 'end'},
    'component_ctor': {'ctor', 'construct', 'constructor', 'create', 'init'},
    'field_accessor': {'get', 'set', 'getter', 'setter', 'field'},
}
CONF_TAG = re.compile(r'(?:\((?:h|m|l|\?)\)|\[(?:g|i)\])$')
HEX8 = re.compile(r'^(?:sub_|0x)?([0-9A-Fa-f]{8})$')
NOISE = re.compile(r'^__(save|rest)')          # q sheet hides these callees (qsheet.NOISE)
TOKEN = re.compile(r'(?:sub_)?[0-9A-Fa-f]{8}\b|[A-Za-z_][\w:.]*(?:\((?:h|m|l|\?)\)|\[(?:g|i)\])?')
JOIN = re.compile(r'\s*(?:,|/|\+|&|\band\b|\bor\b)?\s*')
JOIN_NC = re.compile(r'\s*(?:/|\+|&|\band\b|\bor\b)?\s*')
# a word in the clause before a claim that says the claim's subject is another function
OTHER = re.compile(r'\b(?:called|caller|callers|from|by|via|reached|which|that|whose|while|beside|like|sibling|through|helper|helpers|predicates|also|unit)\b', re.I)
# words right after a claim that hand it to another function ("calls X via Y", "reads +516..+548")
AFTER = re.compile(r'\s*(?:\.\.|via\b|through\b)', re.I)


class ClaimError(Exception):
    pass


def words(name):
    """the words of 4+ letters of a name (after the last :: or .), camel case split, lower case (q sheet's CONFLICT rule)"""
    tail = re.split(r'::|\.', name or '')[-1]
    tail = re.sub(r'([a-z])([A-Z])', r'\1 \2', tail)
    return set(w.lower() for w in re.findall(r'[A-Za-z]{4,}', tail))


def all_words(name):
    """every word of a snake / camel name, lower case (for the pattern-role agreement)"""
    t = re.sub(r'([a-z])([A-Z])', r'\1_\2', name or '')
    return set(w.lower() for w in re.split(r'[^A-Za-z0-9]+', t) if w)


def load_db(dbdir=None):
    """({addr: (name, confidence)} best entry per address from db/*.json, {name: {addr}} of every entry)"""
    best, byname = {}, collections.defaultdict(set)
    rank = {'high': 3, 'medium': 2, 'low': 1}
    for p in glob.glob(os.path.join(dbdir or os.path.join(ROOT, 'db'), '*.json')):
        try:
            ents = json.load(open(p, encoding='utf-8'))
        except (ValueError, OSError):
            continue
        for e in ents if isinstance(ents, list) else []:
            try:
                a = int(e['address'], 16)
            except (KeyError, ValueError, TypeError):
                continue
            n, c = e.get('name') or '', e.get('confidence', 'unknown')
            if n:
                byname[n].add(a)
            if a not in best or rank.get(c, 0) > rank.get(best[a][1], 0):
                best[a] = (n, c)
    return best, byname


class Facts:
    """what the index knows. Every source is a sqlite connection (a test passes its own); db: (best, byname) from load_db."""

    def __init__(self, index=None, fields=None, vcall=None, seen=None, names=None, units=None, db=None):
        import qseen
        import qunits
        self.ix = index or qnames.ro(qnames.INDEX_DB)
        self.fd = fields or qnames.ro(os.path.join(qnames.IDX, 'fields.s2.db'))
        vf = None if vcall else qseen.vdb_file()
        self.vc = vcall or (qnames.ro(vf) if vf else None)
        self.seen = seen or (qnames.ro(qseen.SEEN_DB) if os.path.exists(qseen.SEEN_DB) else None)
        self.nm = names or qnames.ro(qnames.NAMES_DB)
        self.uc = units or qunits.open_units()
        self.db, dbn = db if db is not None else load_db()
        self.dbnames = set(dbn)
        self.byname = collections.defaultdict(set)
        for n, s in dbn.items():
            self.byname[n] |= s
        self.game = collections.defaultdict(set)
        for f, n in self.nm.execute("select func,name from names where source!='fn_string_shared'"):
            self.game[f].add(n)
            self.byname[n].add(f)
        self.idxname = {}
        for a, n in self.ix.execute("select addr,name from funcs where name is not null and name not like 'sub\\_%' escape '\\'"):
            self.byname[n].add(a)
            self.idxname[a] = n
        self.roots = dict(self.fd.execute('select id,text from roots'))
        self.rtti = set(n for (n,) in self.ix.execute('select name from rtti_class where name is not null')) | \
            set(n for (n,) in self.ix.execute('select base from rtti_base where base is not null'))
        self._c = {}

    # ---- resolution
    def resolve(self, tok):
        """a name token -> {addr} or None (not a function the index or our db knows)"""
        t = CONF_TAG.sub('', tok.strip().strip(',;:"\'')).strip('()').rstrip('.')       # the tag first: "name(m)" -> name
        m = HEX8.match(t)
        if m:
            a = int(m.group(1), 16)
            r = self.ix.execute('select 1 from funcs where addr=?', (a,)).fetchone()
            return {a} if r else None
        if not t or not self.byname.get(t):
            return None
        # a bare word ("table", "flush", "open", "ApronOut") is prose unless our db holds it as a name ("memset", "sin")
        if re.fullmatch(r'[A-Za-z]+', t) and t not in self.dbnames:
            return None
        return self.byname[t]

    # ---- facts of one function (cached)
    def _get(self, key, fn):
        if key not in self._c:
            self._c[key] = fn()
        return self._c[key]

    def direct_callees(self, f):
        return self._get(('dce', f), lambda: set(d for (d,) in self.ix.execute(
            "select dst from edges where src=? and kind in ('call','tail') and dst<>src", (f,)) if not NOISE.match(self.idxname.get(d, ''))))

    def callees(self, f):
        def go():
            s = set(self.direct_callees(f))
            if self.vc is not None:
                s |= set(t for (t,) in self.vc.execute("select t.target from icall i join icall_target t on t.site=i.site where i.func=?"
                                                        " and i.status in ('exact','narrow')", (f,)))
            if self.seen is not None:
                s |= set(t for (t,) in self.seen.execute('select target from seen where func=?', (f,)))
            return s
        return self._get(('ce', f), go)

    def direct_callers(self, f):
        return self._get(('dcr', f), lambda: set(s for (s,) in self.ix.execute(
            "select src from edges where dst=? and kind in ('call','tail') and dst<>src", (f,))))

    def callers(self, f):
        def go():
            s = set(self.direct_callers(f))
            if self.vc is not None:
                s |= set(x for (x,) in self.vc.execute("select i.func from icall_target t join icall i on i.site=t.site where t.target=?"
                                                        " and i.status in ('exact','narrow')", (f,)))
            if self.seen is not None:
                s |= set(x for (x,) in self.seen.execute('select func from seen where target=?', (f,)))
            return s
        return self._get(('cr', f), go)

    def strings(self, f):
        return self._get(('s', f), lambda: [norm(t) for (t,) in self.ix.execute(
            'select s.text from strrefs r join strings s on s.addr=r.addr where r.func=?', (f,)) if t])

    def slot_words(self, f):
        return self._get(('w', f), lambda: set(a for (a,) in self.ix.execute("select at from ptrs where value=? and kind='func'", (f,))))

    def vt_classes(self, vt):
        """the RTTI class of the table at vt and its bases (names); empty when RTTI does not know the table"""
        def go():
            r = self.ix.execute('select c.name from rtti_vtable v join rtti_class c on c.td=v.td where v.vt=?', (vt,)).fetchone()
            if not r or not r[0]:
                return set()
            return {r[0]} | set(b for (b,) in self.ix.execute('select base from rtti_base where cls=?', (r[0],)) if b)
        return self._get(('vt', vt), go)

    def field_rows(self, f):
        return self._get(('f', f), lambda: set((rw, off & 0xFFFFFFFF, self.roots.get(rt, '?')) for rw, off, rt in self.fd.execute(
            'select rw,off,root from rows where func=?', (f,))))

    def unit_members(self, uid):
        return [a for (a,) in self.uc.execute('select func from units where unit_id=? order by func', (uid,))]

    def unit_of(self, f):
        r = self.uc.execute('select unit_id from units where func=?', (f,)).fetchone()
        return r[0] if r else None

    def label(self, a):
        if a in self.db:
            return self.db[a][0]
        if self.game.get(a):
            return sorted(self.game[a])[0]
        return self.idxname.get(a) or 'sub_%08X' % a


def norm(s):
    """a string for comparison: escapes a reader writes as text become what they stand for, whitespace collapsed, lower case"""
    s = (s or '').replace('\\\\', '\\').replace('\\n', ' ').replace('\\t', ' ').replace('\\r', ' ')
    return re.sub(r'\s+', ' ', s).strip().lower()


def hx(xs, k=4):
    xs = sorted(xs)
    return ' '.join('%08X' % x for x in xs[:k]) + (' +%d' % (len(xs) - k) if len(xs) > k else '')


# ---------------------------------------------------------------- claim extraction
def segments(text):
    """the evidence split at ; and | (a claim never crosses them); parenthesised asides blanked (they are about another function)"""
    t = re.sub(r'\([^()]*\)', lambda m: m.group(0) if re.fullmatch(r'\((?:h|m|l|\?)\)', m.group(0)) else ' ', text or '')
    return [s.strip() for s in re.split(r'[;|]', t) if s.strip()]


def own(seg, start):
    """True when the clause before position `start` (back to the last comma) names no other subject"""
    clause = seg[seg.rfind(',', 0, start) + 1:start]
    return not OTHER.search(clause)


def name_like(tok):
    t = CONF_TAG.sub('', tok)
    return bool(HEX8.match(t) or ('_' in t and re.search(r'[A-Za-z]', t)) or '::' in t or ('.' in t.strip('.') and t[0].isalpha()))


def name_list(s, fx=None, commas=True):
    """the function-name tokens at the start of s, joined by / + & and or (and commas when `commas`); ends at the first token that is
    no name. A list followed by `via` / `through` is about another function: none."""
    out, i, prev_hex = [], 0, False
    while True:
        j = (JOIN if commas else JOIN_NC).match(s, i)
        k = j.end() if j else i
        m = TOKEN.match(s, k)
        if not m or (out and k == i):
            break
        tok = m.group(0)
        if not (name_like(tok) or (fx is not None and fx.resolve(tok))):
            break
        label = prev_hex and not s[i:k].strip() and not HEX8.match(CONF_TAG.sub('', tok))
        i = m.end()
        if label:
            prev_hex = False       # "ADDR NAME" (q sheet's own form): the name labels the address; the address is the one claim
            continue
        out.append(tok)
        prev_hex = bool(HEX8.match(CONF_TAG.sub('', tok)))
    return [] if AFTER.match(s, i) else out


# a quote: "..." with \" escapes inside, or \"...\" (a quote inside a quoted sheet line)
QSTR = r'(?:"(?:[^"\\]|\\.)+"|\\"(?:[^"\\]|\\[^"])+\\")'
QSTR_RE = re.compile(QSTR)
STR_RE = re.compile(r'\bstr(?:ings?|s)?\b(?:\s+\d+\s*:)?\s*(' + QSTR + r'(?:\s+(?:and\s+)?' + QSTR + r')*)', re.I)
QANY_RE = re.compile(r'"(?:[^"\\]|\\.)*"')
# a quoted chunk that is a sheet line ("829BDE38 writes +108", "vslot vt82022900[+0]", "this: reads +0*6") or one function name is READ
# (only its quote marks go); any other quoted chunk is a game string and is blanked
SHEETLINE = re.compile(r'^\s*(?:(?:sub_)?[0-9A-Fa-f]{8}\s+)?(?:[\w ]{1,24}:\s*)?(?:vslot|vt[0-9A-Fa-f]{8}|out|calls|called|callers?|obs|writes?|reads?|fld)\b', re.I)
NAMEQ = re.compile(r'(?:sub_)?[0-9A-Fa-f]{8}|[A-Za-z_][\w:.]*(?:\((?:h|m|l|\?)\)|\[(?:g|i)\])?')
# the marks of an abbreviated quote: an inner quote, an elision / range (.. ... U+2026), an alternation between word characters (W/H)
ABBR = re.compile('\\\\?"|\\.\\.\\.?|\u2026|(?<=\\w)/(?=\\w)')
VSLOT_RE = re.compile(r'\bvt([0-9A-Fa-f]{8})\s*\[\s*\+\s*(\d+)\s*\](?:\s+([A-Za-z_][\w:<>]*))?')
VSLOT_OF_RE = re.compile(r'\bvslot\s+\+(\d+)\s+of\s+vt([0-9A-Fa-f]{8})\b')
CALLS_RE = re.compile(r'\b(?:calls|out|callees?)\s+(?!only\b)', re.I)
ONLY_CALLEE_RE = re.compile(r'\b(?:out\s+only|only\s+callee\s+is|calls\s+only)\s+', re.I)
CALLER_RE = re.compile(r'\b(?:called\s+(?:from|by)|obs(?:erved)?\s+callers?|callers?(?:\s+are|\s+is)?)\s+(?!only\b)', re.I)
ONLY_CALLER_RE = re.compile(r'\b(?:called\s+only\s+(?:from|by)|only\s+caller\s+is|only\s+called\s+(?:from|by))\s+', re.I)
NCALL_RE = re.compile(r'(?<![\w+.])(\d+)\s+callers\b(?!\s+(?:through|via|of|in\s+the\s+unit\b))', re.I)
FIELD_RE = re.compile(r'\b(writes?|reads?)\s+(?:(this|param\s*\d+|vtable)\s+)?(\+\d+(?:\*\d+)?(?:\s*(?:/|,|and|&)?\s*\+\d+(?:\*\d+)?)*)', re.I)
GFIELD_RE = re.compile(r'\b(writes?|reads?)\s+(?:global\s+|address\s+|to\s+address\s+)?0x([0-9A-Fa-f]{8})(?:\s*\+(\d+)\b)?(?!\s*(?:at|to|into)\b)', re.I)
QUOTED_RE = re.compile(r'"([^"]+)"')


def extract(text, unit=False, fx=None):
    """-> [(kind, claim text, payload)] the recognised claims of an evidence / because text"""
    out = []
    raw = text or ''
    # parenthesised asides (outside quotes) are about another function: a str claim that starts inside one is not read
    qb = QANY_RE.sub(lambda m: '"' + ' ' * (len(m.group(0)) - 2) + '"', raw)
    asides = [m.span() for m in re.finditer(r'\([^()]*\)', qb) if not re.fullmatch(r'\((?:h|m|l|\?)\)', m.group(0))]
    strq = []
    for m in STR_RE.finditer(raw):
        if any(a <= m.start() < b for a, b in asides):
            continue
        strq.append(m.span(1))
        for qm in QSTR_RE.finditer(m.group(1)):
            q = qm.group(0)[2:-2] if qm.group(0).startswith('\\') else qm.group(0)[1:-1]
            if not CONF_TAG.search(q):                  # "name(m)": a quoted function name, no string
                out.append(('str', 'str "%s"' % q, q))

    def unquote(m):
        if any(a <= m.start() < b for a, b in strq):
            return ' ' * len(m.group(0))                # the text of a str claim is never a name or a claim
        inner = m.group(0)[1:-1]
        item = re.search(r'(?:^|[,;|:(])\s*$', raw[:m.start()]) is not None      # a quoted list item: a copied sheet line
        if (item and SHEETLINE.match(inner)) or (NAMEQ.fullmatch(inner) and name_like(inner)):
            return ' ' + inner + ' '
        return ' ' * len(m.group(0))                    # a game string: not names (lengths kept)
    for seg in segments(QANY_RE.sub(unquote, raw)):
        sq = seg
        for m in VSLOT_RE.finditer(sq):
            if not own(sq, m.start()) or AFTER.match(sq, sq.index(']', m.start()) + 1):
                continue
            vt, off = int(m.group(1), 16), int(m.group(2))
            out.append(('vslot', 'vt%08X[+%d]' % (vt, off), (vt, off)))
            if m.group(3) and m.group(3).lower() not in ('via', 'through', 'from', 'and', 'or', 'in', 'of', 'with'):
                out.append(('vslot_class', 'vt%08X[+%d] %s' % (vt, off, m.group(3)), (vt, m.group(3))))
        for m in VSLOT_OF_RE.finditer(sq):
            if own(sq, m.start()):
                out.append(('vslot', m.group(0), (int(m.group(2), 16), int(m.group(1)))))
        if not unit:
            m = ONLY_CALLEE_RE.search(sq)
            if m and own(sq, m.start()):
                out.append(('only_callee', sq[m.start():m.end() + 60].strip(), name_list(sq[m.end():], fx)))
            for m in CALLS_RE.finditer(sq):
                if own(sq, m.start()):
                    out += [('calls', 'calls ' + t, t) for t in name_list(sq[m.end():], fx)]
            m = ONLY_CALLER_RE.search(sq)
            if m:
                out.append(('only_caller', sq[m.start():m.end() + 60].strip(), name_list(sq[m.end():], fx)))
            for m in CALLER_RE.finditer(sq):
                if own(sq, m.start()):         # "caller X, Y": Y may be a callee; only "callers X, Y" is a list
                    out += [('caller', 'caller ' + t, t) for t in name_list(sq[m.end():], fx, re.search(r'callers\s*$', m.group(0).strip()) is not None)]
            for m in NCALL_RE.finditer(sq):
                if not re.search(r'\bobs(?:erved)?\s*$', sq[:m.start()]) and own(sq, m.start()):
                    out.append(('n_callers', m.group(0), int(m.group(1))))
        for m in FIELD_RE.finditer(sq):
            if not own(sq, m.start()) or AFTER.match(sq, m.end()):
                continue
            rw = 'W' if m.group(1).lower().startswith('w') else 'R'
            gb = re.search(r'\bglobal\s+0x([0-9A-Fa-f]{8})\s*:\s*$', sq[:m.start()], re.I)     # q sheet's "global G: writes +N"
            for o in re.findall(r'\+(\d+)', m.group(3)):
                out.append(('field', '%s%s +%s' % ('global 0x%s: ' % gb.group(1).upper() if gb else '', m.group(1).lower(), o),
                            (rw, int(gb.group(1), 16) if gb else None, int(o), False)))
        for m in GFIELD_RE.finditer(sq):
            if not own(sq, m.start()) or AFTER.match(sq, m.end()) or re.search(r'\bvtables?\s*$', sq[:m.start()]) or sq.startswith('/0x', m.end()):
                continue
            rw = 'W' if m.group(1).lower().startswith('w') else 'R'
            bare = not re.search(r'\b(?:global|address)\s+0x', m.group(0), re.I)      # "writes 0xV" may be a value written (a vtable)
            out.append(('field', re.sub(r'\s+', ' ', m.group(0)), (rw, int(m.group(2), 16), int(m.group(3)) if m.group(3) else None, bare)))
    if unit and fx is not None:
        for q in QUOTED_RE.findall(raw):
            for tok in TOKEN.findall(q):
                if name_like(tok) and not HEX8.match(CONF_TAG.sub('', tok)) and fx.resolve(tok):
                    out.append(('mention', 'names ' + CONF_TAG.sub('', tok), tok))
    return out


# ---------------------------------------------------------------- checks
def check(kind, payload, subj, fx):
    """-> (verdict, fact), or None when the form turns out to be no claim. subj: the functions the claim is about."""
    if kind == 'str':
        q = norm(payload.replace('\\"', '"')).split('*')[0].strip('~.' + chr(0x2026)).strip()   # "CB_Input*": a prefix; a cut ~ / ... / U+2026 dropped
        if len(q) < 3:
            return 'not_checkable', 'quoted text too short'
        have = [t for f in subj for t in fx.strings(f)]
        if any(q in t for t in have):
            return 'checked', ''
        if '\\"' in payload:               # 'str "?333C\" ...': a game string that ENDS in a backslash, not an escaped quote
            q0 = norm(payload[:payload.index('\\"') + 1])
            if len(q0) >= 3 and any(q0 in t for t in have):
                return 'checked', ''
        if ABBR.search(q):
            # an abbreviated quote ("T_PlaneShadowBlurW/H", "Action 2..5", "Setup ... could be split", 'Invalid \"\" cloth'): each literal
            # run (the text between the marks; of an alternation the first alternative) must be in a string the subject references
            runs = [r for r in (re.split(r'(?<=\w)/(?=\w)', p)[0].strip(' ~.') for p in re.split('"|\\.\\.\\.?|\u2026', q)) if len(r) >= 3]
            if not runs:
                return 'not_checkable', 'abbreviated quote with no literal run of 3+ characters'
            miss = [r for r in runs if not any(r in t for t in have)]
            if not miss:
                return 'checked', ''
            return 'refuted', '"%s" is not in the %d string(s) it references%s' % (miss[0][:40], len(have), (': "%s"' % have[0][:60]) if have else '')
        return 'refuted', 'not in the %d string(s) it references%s' % (len(have), (': "%s"' % have[0][:60]) if have else '')
    if kind == 'vslot':
        vt, off = payload
        if any(vt + off in fx.slot_words(f) for f in subj):
            return 'checked', ''
        w = sorted(x for f in subj for x in fx.slot_words(f))
        return 'refuted', ('the word at %08X does not point at it; its data words: %s' % (vt + off, hx(w))) if w else \
            'the word at %08X does not point at it; no data word points at it' % (vt + off)
    if kind == 'vslot_class':
        vt, cls = payload
        if cls not in fx.rtti:
            return None                    # the word after ] is not a class name: no class claim
        have = fx.vt_classes(vt)
        if not have:
            return 'not_checkable', 'RTTI does not know the table %08X' % vt
        return ('checked', '') if cls in have else ('refuted', 'RTTI says table %08X is %s' % (vt, ' / '.join(sorted(have))))
    if kind in ('calls', 'caller', 'mention'):
        r = fx.resolve(payload)
        if r is None:
            return 'not_checkable', '%s resolves to no function' % payload
        if kind == 'mention':
            near = set(subj)
            for f in subj:
                near |= fx.callees(f) | fx.callers(f)
            return ('checked', '') if r & near else ('refuted', '%s (%s) is no member, callee or caller of the unit' % (payload, hx(r, 2)))
        if kind == 'calls':
            if any(r & fx.callees(f) for f in subj):
                return 'checked', ''
            return 'refuted', '%s (%s) is not among its %d callee(s)' % (payload, hx(r, 2), len(set().union(*[fx.callees(f) for f in subj])))
        if any(r & fx.callers(f) for f in subj):
            return 'checked', ''
        return 'refuted', '%s (%s) is not among its %d caller(s)' % (payload, hx(r, 2), len(set().union(*[fx.callers(f) for f in subj])))
    if kind in ('only_callee', 'only_caller'):
        res = [fx.resolve(t) for t in payload]
        named = set().union(*[x for x in res if x]) if any(res) else set()
        if not named:
            return 'not_checkable', 'the names resolve to no function'
        f = subj[0]
        if kind == 'only_callee':
            have = fx.direct_callees(f)
            if have and have <= named:
                return 'checked', ''
            return 'refuted', 'direct callees: %s' % (' '.join(fx.label(x) for x in sorted(have)[:4]) or 'none')
        have = fx.direct_callers(f)
        if (have and have <= named) or (not have and named & fx.callers(f)):
            return 'checked', ''
        return 'refuted', 'direct callers: %d (%s)' % (len(have), ' '.join(fx.label(x) for x in sorted(have - named)[:4]))
    if kind == 'n_callers':
        f = subj[0]
        d = fx.direct_callers(f)
        u = fx.unit_of(f)
        outside = set(x for x in d if u is None or fx.unit_of(x) != u)
        if payload in (len(d), len(outside), len(d) - len(outside)):
            return 'checked', ''
        return 'refuted', 'the index has %d direct caller(s) (%d outside its unit)' % (len(d), len(outside))
    if kind == 'field':
        rw, g, off, bare = payload
        for f in subj:
            for r, o, root in fx.field_rows(f):
                if r != rw:
                    continue
                if g is None and o == off:
                    return 'checked', ''
                if g is not None and off is None and (root == 'g:%08X' % g or (root == 'K' and o == g)):
                    return 'checked', ''            # "writes global G": any field of it
                if g is not None and off is not None and ((root == 'g:%08X' % g and o == off) or (root == 'K' and o == ((g + off) & 0xFFFFFFFF))):
                    return 'checked', ''
        if bare:
            return 'not_checkable', '0x%08X is no %s address of it; a bare "writes 0xV" may name a value written' % (g, 'written' if rw == 'W' else 'read')
        have = sorted(set(o for f in subj for r, o, root in fx.field_rows(f) if r == rw and root != 'K'))
        return 'refuted', 'its %s offsets: %s' % ('write' if rw == 'W' else 'read', ' '.join('+%d' % o for o in have[:8]) or 'none')
    raise ClaimError('unknown kind %s' % kind)


def name_checks(func, name, fx):
    """the proposed name against the function's HIGH db name and the game's names"""
    out = []
    d = fx.db.get(func)
    if d and d[1] == 'high' and d[0]:
        if d[0] == name or words(d[0]) & words(name):
            out.append(('name_vs_db', 'name vs db %s' % d[0], 'checked', ''))
        elif words(d[0]) and words(name):
            out.append(('name_vs_db', 'name vs db %s' % d[0], 'refuted', 'shares no word of 4+ letters with the high db name %s' % d[0]))
    g = sorted(fx.game.get(func, ()))
    if g:
        gw = set().union(*[words(x) for x in g])
        if name in g or gw & words(name):
            out.append(('name_vs_game', 'name vs game %s' % g[0], 'checked', ''))
        elif gw and words(name):
            out.append(('name_vs_game', 'name vs game %s' % g[0], 'refuted', 'shares no word of 4+ letters with the game name(s) %s' % ' | '.join(g[:3])))
    return out


def check_reading(text, subj, fx, unit=False, func=None, name=None):
    """-> [(kind, claim, verdict, fact)]; at least one row"""
    rows, seen = [], set()
    for kind, claim, payload in extract(text, unit, fx):
        if (kind, claim) in seen:
            continue
        seen.add((kind, claim))
        r = check(kind, payload, subj, fx)
        if r is not None:
            rows.append((kind, claim, r[0], r[1]))
    if func is not None and name:
        rows += name_checks(func, name, fx)
    if not rows:
        rows.append(('-', '', 'not_checkable', 'no claim of a recognised form'))
    return rows


# ---------------------------------------------------------------- build / stats / show
def build(path=None, fx=None, out=print):
    fx = fx or Facts()
    c = qread.open_rw(path)
    c.executescript(TABLE)
    now = time.strftime('%Y-%m-%d %H:%M:%S')
    res = []
    members = {}
    for rid, uid, because in c.execute('select id,unit_id,because from unit_read order by id').fetchall():
        if uid not in members:
            members[uid] = fx.unit_members(uid)
        for k, cl, v, fa in check_reading(because, members[uid], fx, unit=True):
            res.append(('unit', rid, uid, k, cl, v, fa, now))
    for rid, f, n, ev in c.execute('select id,func,name,evidence from name_read order by id').fetchall():
        for k, cl, v, fa in check_reading(ev, [f], fx, func=f, name=n):
            res.append(('name', rid, f, k, cl, v, fa, now))
    c.isolation_level = None
    try:
        c.execute('begin immediate')
        c.execute('delete from claim_read')
        c.executemany('insert into claim_read(read_kind,read_id,target,kind,claim,verdict,fact,when_) values (?,?,?,?,?,?,?,?)', res)
        c.execute('commit')
    except sqlite3.OperationalError as e:
        if c.in_transaction:
            c.execute('rollback')
        raise qread.ReadError('q read claims: the store is busy or locked (%s); nothing was stored, run it again' % e)
    finally:
        c.close()
    out('claim_read: %d rows for %d unit readings and %d name readings' % (
        len(res), len(set(r[1] for r in res if r[0] == 'unit')), len(set(r[1] for r in res if r[0] == 'name'))))
    return len(res)


def _ro(path):
    c = qread.open_ro(path)
    if c is None:
        return None
    try:
        c.execute('select 1 from claim_read limit 1')
    except sqlite3.Error:
        c.close()
        return None
    return c


def reading_status(verdicts):
    """the verdicts of one reading -> refuted | all_checked | nothing"""
    vs = set(verdicts)
    if 'refuted' in vs:
        return 'refuted'
    if 'checked' in vs:
        return 'all_checked'
    return 'nothing'


def stats(path=None, newest_only=False):
    """-> ({read_kind: Counter(status)}, Counter((kind, verdict))) or (None, None) before the first --build"""
    c = _ro(path)
    if c is None:
        return None, None
    try:
        per = collections.defaultdict(list)
        kv = collections.Counter()
        for rk, rid, k, v in c.execute('select read_kind,read_id,kind,verdict from claim_read'):
            per[(rk, rid)].append(v)
            if k != '-':
                kv[(k, v)] += 1
        keep = None
        if newest_only:
            keep = set(('unit', r) for (r,) in c.execute('select max(id) from unit_read group by unit_id')) | \
                set(('name', r) for (r,) in c.execute('select max(id) from name_read group by func'))
    finally:
        c.close()
    st = collections.defaultdict(collections.Counter)
    for (rk, rid), vs in per.items():
        if keep is None or (rk, rid) in keep:
            st[rk][reading_status(vs)] += 1
    return st, kv


def stats_lines(path=None):
    out = []
    for label, newest in (('every reading', False), ('newest reading per unit / per function', True)):
        st, kv = stats(path, newest)
        if st is None:
            return ['claims: none checked yet (py tools/q/q.py read claims --build)']
        for rk in ('unit', 'name'):
            s = st.get(rk, collections.Counter())
            out.append('%s, %s readings %d: all checkable claims checked %d, at least one refuted %d, nothing checkable %d' % (
                label, rk, sum(s.values()), s['all_checked'], s['refuted'], s['nothing']))
    out.append('claims by kind (every reading; checked/refuted/not_checkable): ' + ' '.join(
        '%s %d/%d/%d' % (k, kv[(k, 'checked')], kv[(k, 'refuted')], kv[(k, 'not_checkable')]) for k in KINDS if any(kv[(k, v)] for v in VERDICTS)))
    out.append('a refuted claim is a flag for a reader, never a lowered confidence; checked = the index holds the fact, not that the reading is right.')
    return out


def show_lines(uid, path=None):
    c = _ro(path)
    if c is None:
        return ['claims: none checked yet (py tools/q/q.py read claims --build)']
    try:
        rows = c.execute("select c.read_kind,c.read_id,c.target,c.kind,c.claim,c.verdict,c.fact from claim_read c where"
                         " (c.read_kind='unit' and c.read_id in (select id from unit_read where unit_id=?)) or"
                         " (c.read_kind='name' and c.read_id in (select id from name_read where unit_id=?))"
                         " order by c.read_kind desc,c.read_id desc,c.id", (uid, uid)).fetchall()
    finally:
        c.close()
    out = []
    for rk, rid, t, k, cl, v, fa in rows:
        who = 'unit reading %d' % rid if rk == 'unit' else 'name reading %d sub_%08X' % (rid, t)
        out.append('  %s: %s %s%s%s' % (who, v.upper() if v == 'refuted' else v, k, (' ' + cl) if cl else '', (' -- ' + fa) if fa else ''))
    return out or ['unit %d: no claim rows' % uid]


# ---------------------------------------------------------------- promotion (a LIST for the owner; never written to db/)
def promote(path=None, fx=None, pattern=None):
    """-> rows (addr, name, confidence, source A, source B, kind of B). The rule: docs/research/s110/VERIFYMECH.md."""
    import qpattern
    import qreadverify
    fx = fx or Facts()
    c = _ro(path)
    if c is None:
        raise qread.ReadError('q read claims --promote: run --build first')
    try:
        newest, older = {}, collections.defaultdict(list)
        for rid, f, n, ev, cf, uid, b in c.execute('select id,func,name,evidence,confidence,unit_id,batch from name_read order by id'):
            if f in newest:
                older[f].append(newest[f][1])
            newest[f] = (rid, n, ev, cf, uid, b)
        when = dict(((u, b), w) for u, b, w in c.execute('select unit_id,batch,min(when_) from unit_read group by unit_id,batch'))
        cv = collections.defaultdict(list)
        for rid, v in c.execute("select read_id,verdict from claim_read where read_kind='name'"):
            cv[rid].append(v)
        wrong = set(f for (f,) in c.execute('select func from wrong_read'))
    finally:
        c.close()
    verdicts = qreadverify.func_verdicts(0, 0xFFFFFFFF, path)
    vnote = {}
    vc = qreadverify._ro(path)
    if vc is not None:
        try:
            for t, s, n in vc.execute("select target,source,note from verify_read where target_kind='func' and verdict='confirmed' order by id"):
                vnote[t] = (s, n)
        finally:
            vc.close()
    lab = qpattern.labels(pattern)
    rows = []
    for f, (rid, n, ev, cf, uid, b) in sorted(newest.items()):
        if cf not in ('high', 'medium'):
            continue
        d = fx.db.get(f)
        if d and d[1] in ('high', 'medium'):
            continue                       # our db already names it
        if f in wrong or verdicts.get(f) in ('refuted', 'partly'):
            continue
        if fx.game.get(f):
            continue                       # the game names it: that name is the path (names.s2.db), and it was on the reader's sheet; a
            #                                pattern label made from it (read_end_process) is the reader's own input, no second source
        vs = cv.get(rid, [])
        if 'refuted' in vs or 'checked' not in vs:
            continue                       # source A must carry at least one checked claim and no refuted one
        if any(o != n and not (words(o) & words(n)) for o in older[f]):
            continue                       # an earlier reading named it otherwise: the readers disagree
        src_a = 'reader %s [r:%s]: %s' % (b, cf, re.sub(r'\s+', ' ', ev or '')[:80])
        if verdicts.get(f) == 'confirmed':
            s, note = vnote.get(f, ('?', ''))
            rows.append((f, n, cf, src_a, 'probe verdict (%s): %s' % (s, re.sub(r'\s+', ' ', note)[:80]), 'probe'))
            continue
        w = when.get((uid, b)) or ''
        if w and w < PATTERN_ON_SHEET:
            for k in lab.get(f, []):
                if all_words(n) & PATTERN_WORDS.get(k, set()):
                    rows.append((f, n, cf, src_a, 'pattern [p:%s] (a rule; not on the sheet read at %s)' % (k, w), 'pattern'))
                    break
    return rows


def promote_lines(rows):
    by = collections.Counter((r[5], r[2]) for r in rows)
    return ['would promote %d proposal(s) (source B / confidence): %s' % (len(rows), ' '.join('%s/%s %d' % (k[0], k[1], v) for k, v in sorted(by.items()))),
            'a LIST for the owner: nothing is written to db/*.json.']


def main(rest, path=None, fx=None):
    if '--build' in rest:
        build(path, fx)
        return 0
    if '--stats' in rest:
        for x in stats_lines(path):
            print(x)
        return 0
    if '--promote' in rest:
        rows = promote(path, fx)
        for x in promote_lines(rows):
            print(x)
        if '--out' in rest:
            i = rest.index('--out')
            if i + 1 >= len(rest):
                raise qread.ReadError('q read claims --promote --out FILE')
            fn = rest[i + 1]
            os.makedirs(os.path.dirname(os.path.abspath(fn)), exist_ok=True)
            with open(fn, 'w', encoding='utf-8', newline='\n') as fh:
                fh.write('addr\tproposed\tconfidence\tsource_a\tsource_b\tkind_b\n')
                for f, n, cf, a, b, k in rows:
                    fh.write('%08X\t%s\t%s\t%s\t%s\t%s\n' % (f, n, cf, a.replace('\t', ' '), b.replace('\t', ' '), k))
            print('-> %s' % fn)
        return 0
    pos = [x for x in rest if not x.startswith('--')]
    if len(pos) != 1:
        raise qread.ReadError('q read claims --build | --stats | UNIT|ADDR | --promote [--out TSV]')
    uid = int(pos[0]) if re.fullmatch(r'\d{1,6}', pos[0]) else qread.resolve_unit(pos[0])
    for x in show_lines(uid, path):
        print(x)
    return 0
