"""Validate the function database (db/*.json) against the CLAUDE.md schema. Session 8, task 18.

    py tools/db/validate.py [--db DIR] [--docs DIR] [--index FILE | --no-index] [--gen DIR] [--coverage FILE]
                            [--warn-layout] [--json] [--list N]

Checks (E = error, makes the exit code 1; W = warning, printed separately, exit code unaffected):
  E  a file that does not parse, or whose top level is not a list of entries
  E  an entry that is not an object, misses a schema field, or has an empty/non-string summary, name or source
  E  confidence not in high|medium|low|unknown; system not in the CLAUDE.md enum
  E  evidence not a list of non-empty strings; empty evidence with confidence high or medium
  E  address not one 8-digit hex guest address 0xXXXXXXXX (struct layouts, field offsets, ranges, several
     addresses in one entry). These entries are invisible to `q db` (it skips addresses it cannot parse).
     The summary splits them into: no guest address, one address plus an offset or dereference, several
     addresses or a range. --warn-layout reports them as warnings instead.
  E  an address in the code range that is not a function start (inside a function, or in no function), checked
     against the code index (tools/q) or, without one, the generated function list (*_init.cpp)
  E  the same address twice in one file; the same name twice in one file; one name used for different addresses
  W  name not snake_case; unknown extra fields; lower-case hex digits in an address
  W  the same address in several files under different names (review list), or with a different confidence/system
  W  a data address (outside the code range) outside the loaded image (heap or stack address?)
  W  evidence or summary text that looks like pasted generated code (hard rule: summaries only in db/)
  W  disassembly: an entry whose text quotes 3 or more PPC instructions with operands (lwz r3,8(r31) ...).
     Entries citing 1-2 instructions are only counted (a note), so a single cited store does not flood the report
  W  key-system entries (camera, match state, video/tron, cutscene/motion, file loading) without two independent
     labels (CLAUDE.md). Read from the db's own "labels: ..." evidence item, per labeller A and B: a label (own name
     or reading, any confidence), a mention only (mentions / cites / describes / lists it / covers it inside
     another entry / candidate only / no entry), or none (none / did not list / not listed / did not find / gave
     that name to another function). key_single_label = one labeller only (or the item says "X only");
     key_partial_label = one label and the other labeller only mentions it, or both only mention it;
     key_no_label = neither labeller (e.g. "not studied by either labeller"); key_label_unparsed = a labels item
     the rules cannot read. Entries without a labels item fall back to phrases in source/evidence ("single label",
     "needs second label", "A only"). Free-text parsing: a lead, not proof. A per-file table is printed.
  W  docs/systems/*.md without a db file and db files without a docs/systems page
  W  the code index is stale (codegen, map or image changed since it was built)

Coverage table (always printed): per db file and per `system` value: entries (function / data / layout), confidence
mix, % with live evidence (a live run, run/timeline number, trace, memory watch, breakpoint, mid-asm hook, profile or
coverage scenario named in the evidence: a keyword match, so a lead, not proof; a keyword negated in its own clause
does not count: "not traced", "never ran in coverage", "not peeked live", "never appears in timelines 070" are
missing or negative observations, not evidence of what the code does), % whose only live evidence is coverage (it
ran, nothing about what it did), and % of function entries seen running in tonight's scenario coverage
(out/harness/coverage/coverage.json, if present).

Reads only our own db/docs files plus the (gitignored) index / generated function list for address checks. Prints
addresses and our own names only. Exit code: 0 no errors, 1 errors, 2 usage problem (e.g. no db folder),
3 internal error of the validator itself (a traceback; not a verdict on the db).
"""
import argparse
import collections
import glob
import json
import os
import re
import sqlite3
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
DEFAULT_DB = os.path.join(REPO, 'db')
DEFAULT_DOCS = os.path.join(REPO, 'docs', 'systems')
DEFAULT_INDEX = os.path.join(REPO, 'out', 'harness', 'index', 'index.db')
DEFAULT_GEN = os.path.join(REPO, 'generated', 'default')
DEFAULT_COVERAGE = os.path.join(REPO, 'out', 'harness', 'coverage', 'coverage.json')
IMAGE_BASE = 0x82000000

FIELDS = ('address', 'name', 'summary', 'evidence', 'confidence', 'system', 'source')
CONFIDENCE = ('high', 'medium', 'low', 'unknown')
SYSTEMS = ('camera', 'render', 'match', 'assets', 'audio', 'io', 'ui', 'other')
# One guest address, as `q db` parses it (int(address, 16)); 8 digits so a typo like 0x8201EF4 is caught. Code-range
# addresses must be function starts; others are data (a warning when outside the loaded image).
ADDR_RE = re.compile(r'^0x[0-9A-Fa-f]{8}$')
GUEST_LITERAL_RE = re.compile(r'0x8[23][0-9A-Fa-f]{6}\b')
NAME_RE = re.compile(r'^[a-z][a-z0-9]*(?:_[a-z0-9]+)*$')
# Live evidence: something observed while the game ran (our own run outputs), as opposed to static reading.
_LIVE_NOT_COVERAGE = (r'\blive\b|\bruns? \d{2,3}\b|\brun_?\d{3}\b|wwe2k14_\d{3}|\btimelines? \d{2,3}|'
                      r'\bframes_\d{3}|\btrac(?:e|ed|es|ing)\b|\bwatch(?:ed|es|ing|point)?\b|breakpoint|midasm|'
                      r'mid-asm|\bobserved\b|\blogged\b|\bprofil(?:e|er|ing)\b')
LIVE_NOT_COVERAGE_RE = re.compile(_LIVE_NOT_COVERAGE, re.I)
LIVE_RE = re.compile(_LIVE_NOT_COVERAGE + r'|\bcoverage\b', re.I)
# A live keyword is negated when a negation word stands before it in the same clause (no , ; . ( ) | between).
LIVE_NEGATION_RE = re.compile(r"\b(?:not|never|no|without|until|unconfirmed|nor|neither)\b[^,;.()|]{0,40}$", re.I)
# Fragments of ReXGlue's generated C++ (never belongs in db/): ctx.r3, PPC_LOAD_U32(, r11.u64 =, REX_ ...
GENCODE_RE = re.compile(r'\bctx\.[rfv]\d|\bPPC_[A-Z_]+\(|\.u(?:8|16|32|64)\s*=|\bREX_[A-Z_]+\(|\bf\d+\.f64\b|'
                        r'\bbase\s*\+\s*ctx\b')
# PPC instructions with operands quoted from the disassembly: "lwz r3,8(r31)", "addi r3,r31,-32", "lwz 4(r3)".
_PPC_MNEMONICS = (r'lwz|lwzx|lwzu|lwbrx|stw|stwx|stwu|stwbrx|lis|addi|addis|addic|lbz|lbzx|stb|stbx|lhz|lhzx|lha|'
                  r'sth|sthx|rlwinm|rlwimi|rlwnm|rldicl|rldicr|clrlwi|slwi|srwi|cmpw|cmpwi|cmplw|cmplwi|cmpd|cmpdi|'
                  r'cmpld|cmpldi|mflr|mtlr|mtctr|mfctr|lfs|lfsx|lfd|lfdx|stfs|stfsx|stfd|stfdx|ori|oris|xori|li|lwa|'
                  r'mr|add|addze|subf|subfic|neg|andc|andi\.|orc|xor|slw|srw|sraw|srawi|extsh|extsb|'
                  r'extsw|mullw|mulli|divw|divwu|fmr|fadds|fmuls|fsubs|fmadds|fdivs|fcmpu|frsp|fctiwz|lvx|stvx|ld|'
                  r'ldx|std|stdu|stdx')
DISASM_RE = re.compile(r'(?<![\w.])(?:%s)\s+(?:[rfv]\d{1,3}\s*,|-?(?:0x)?[0-9A-Fa-f]+\(r\d{1,2}\))' % _PPC_MNEMONICS,
                       re.I)
DISASM_WARN_AT = 3
SINGLE_LABEL_RE = re.compile(r'single label|needs? (?:a )?second label|\b[AB] only\b', re.I)
NO_LABEL_RE = re.compile(r'not studied by either labell?er|neither labell?er|added by the reconciler', re.I)
# "labels: ..." evidence items (the db's own convention for key systems).
LABELS_ITEM_RE = re.compile(r'^\s*labels?\b\s*:?\s*', re.I)
LABEL_NONE_RE = re.compile(r'^(?:none\b|no entry\b|did not (?:list|label|find|study)\b|not listed\b|not studied\b|'
                           r'gave the same name to\b|used the same name for\b|found a different\b)', re.I)
LABEL_MENTION_RE = re.compile(r'^(?:mention(?:s|ed)?\b|cite[sd]?\b|name[sd]? (?:it|this|the same)\b|describe[sd]?\b|'
                              r'cover(?:s|ed)?\b|list(?:s|ed)? (?:it|the same|this|the request)\b|note[sd]?\b|'
                              r'only peeked\b|part of\b|'
                              r'\((?:in|inside)\b|\(open question)|\(no entry\)|no separate entry|\bin its\b|'
                              r'\b(?:candidate|init write|mentions) only\b', re.I)
LABEL_ONLY_RE = re.compile(r'^([AB]) only\b', re.I)
LABEL_BOTH_RE = re.compile(r'^(?:A and B\b|both\b|agree\b)', re.I)
LABEL_MENTION_OF_RE = re.compile(r"\b([AB])'s mention\b")
ADDRESS_FORM_LABEL = {'none': 'no guest address', 'base_plus_offset': 'one address plus an offset or dereference',
                      'several_or_range': 'several addresses or a range'}
# CLAUDE.md key systems (camera, match state, tron/video loading, cutscenes, file loading), by db file. The `system`
# field is too broad for this (every AI entry is system 'match'); entries with system 'camera' count too.
KEY_FILES = ('camera', 'match_state', 'video_tron', 'cutscene_motion', 'file_loading')
KEY_SYSTEMS = ('camera',)
SUMMARY_MAX = 800
EVIDENCE_MAX = 600


class Report:
    def __init__(self):
        self.errors = []    # (kind, where, message)
        self.warnings = []
        self.notes = []
        self.disasm_cites = 0          # entries quoting 1..DISASM_WARN_AT-1 instructions (counted, not listed)
        self.labels = collections.OrderedDict()   # key db file -> Counter of label status

    def err(self, kind, where, msg):
        self.errors.append((kind, where, msg))

    def warn(self, kind, where, msg):
        self.warnings.append((kind, where, msg))


def h(a):
    return '0x%08X' % a


def hashable(v):
    """A set/dict key for any JSON value (a list-valued field must not crash the duplicate checks)."""
    return v if isinstance(v, (str, int, float, bool, type(None))) else json.dumps(v, sort_keys=True)


def live_kind(ev):
    """'live' | 'coverage' (live only through coverage) | 'negated' (only negated live keywords) | None."""
    hits = []
    for m in LIVE_RE.finditer(ev):
        before = ev[max(0, m.start() - 60):m.start()]
        if not LIVE_NEGATION_RE.search(before):
            hits.append(m.group(0))
    if not hits:
        return 'negated' if LIVE_RE.search(ev) else None
    if any(LIVE_NOT_COVERAGE_RE.fullmatch(x) for x in hits):
        return 'live'
    return 'coverage'


def address_form_kind(a):
    """Breakdown of a non-single address: 'none' | 'base_plus_offset' | 'several_or_range'."""
    if not isinstance(a, str):
        return 'none'
    lits = GUEST_LITERAL_RE.findall(a)
    if len(lits) >= 2:
        return 'several_or_range'
    if len(lits) == 1:
        return 'base_plus_offset'
    return 'none'


def split_clauses(t):
    """Split a labels text at ';' and '. ' and at ', ' before 'A '/'B ', outside parentheses. (Quotes are not
    tracked: apostrophes as in B's would confuse it, and no quoted reading in the db holds '; ' or ', A '.)"""
    out, cur, depth, i = [], [], 0, 0
    while i < len(t):
        c = t[i]
        if c == '(':
            depth += 1
        elif c == ')':
            depth = max(0, depth - 1)
        if depth == 0:
            if c == ';' or (c == '.' and (i + 1 == len(t) or t[i + 1] == ' ')):
                out.append(''.join(cur))
                cur = []
                i += 1
                continue
            if c == ',' and re.match(r',\s+[AB](?=[\s:(])', t[i:]):
                out.append(''.join(cur))
                cur = []
                i += 1
                continue
        cur.append(c)
        i += 1
    out.append(''.join(cur))
    return [x.strip() for x in out if x.strip()]


def label_status(text):
    """Who labelled a key-system entry, from its 'labels: ...' evidence item.
    -> (status, {'A': state, 'B': state}); status two | partial | single | none | unparsed,
    state label | mention | none | None (not named)."""
    t = LABELS_ITEM_RE.sub('', text, count=1).strip()
    st = {'A': None, 'B': None}
    rank = {None: 0, 'none': 1, 'mention': 2, 'label': 3}
    only = None

    def put(x, v):
        if rank[v] > rank[st[x]]:
            st[x] = v
    clauses = split_clauses(t)
    if clauses and LABEL_ONLY_RE.match(clauses[0]):
        only = LABEL_ONLY_RE.match(clauses[0]).group(1).upper()
    for k, c in enumerate(clauses):
        if k == 0 and LABEL_BOTH_RE.match(c):
            put('A', 'label')
            put('B', 'label')
            continue
        m = re.match(r'^([AB])(?=[\s:(]|$)\s*:?\s*(.*)$', c, re.S)   # 'A ...', 'A: ...', 'A (...)'; not "A's"
        if not m:
            continue
        x, rest = m.group(1), m.group(2).strip()
        if re.match(r'^and B\b', rest) and x == 'A':
            put('A', 'label')
            put('B', 'label')
        elif LABEL_NONE_RE.match(rest):
            put(x, 'none')
        elif LABEL_MENTION_RE.search(rest):
            put(x, 'mention')
        else:
            put(x, 'label')
    for m in LABEL_MENTION_OF_RE.finditer(t):
        put(m.group(1), 'mention')
    if only:
        st[only] = 'label'
        other = 'B' if only == 'A' else 'A'
        if st[other] == 'label':      # "A only" yet B credited with a label: the text contradicts itself
            return 'unparsed', st
        return 'single', st
    labels = sum(1 for v in st.values() if v == 'label')
    seen = sum(1 for v in st.values() if v in ('label', 'mention'))
    if labels == 2:
        return 'two', st
    if seen == 2:
        return 'partial', st
    if seen == 1:
        return 'single', st
    if any(v == 'none' for v in st.values()):
        return 'none', st
    return 'unparsed', st


def load_functions_from_index(path):
    """{addr: (size, has_body)}, code range, image size, staleness warnings. None when the index is missing."""
    if not os.path.exists(path):
        return None
    try:
        con = sqlite3.connect('file:%s?mode=ro' % path.replace('\\', '/'), uri=True)
        try:
            funcs = {a: (s or 0, hb) for a, s, hb in con.execute('SELECT addr, size, has_body FROM funcs')}
            meta = {k: json.loads(v) for k, v in con.execute('SELECT key, value FROM meta')}
        finally:
            con.close()
    except (sqlite3.Error, ValueError) as e:
        print('validate: cannot read the index %s (%s): using the generated function list' % (path, e),
              file=sys.stderr)
        return None
    if not funcs:
        return None
    lo, hi = meta.get('code_range') or (min(funcs), max(funcs))
    image = meta.get('image') or {}
    stale = []
    try:
        sys.path.insert(0, os.path.join(REPO, 'tools', 'q'))
        import q as qmod  # noqa: E402  (tools/q/q.py: same staleness rules as every q call)
        stale = qmod.staleness(meta)
    except Exception as e:  # q not importable: still validate, say so
        stale = ['cannot check index staleness (%s)' % e]
    return dict(source='index ' + os.path.relpath(path, REPO), funcs=funcs, lo=lo, hi=hi,
                image_size=image.get('size'), stale=stale, sized=True)


def load_functions_from_gen(gen):
    """Fallback: function starts from the generated PPCFuncMappings (no sizes, so 'inside which function' is the
    nearest start below, unconfirmed)."""
    paths = glob.glob(os.path.join(gen, '*_init.cpp'))
    if not paths:
        return None
    rx = re.compile(r'^\s*\{ 0x([0-9A-Fa-f]+), (\w+) \},?\s*$', re.M)
    funcs = {}
    for p in paths:
        with open(p, encoding='utf-8', errors='replace') as f:
            for m in rx.finditer(f.read()):
                funcs[int(m.group(1), 16)] = (0, 1)
    if not funcs:
        return None
    return dict(source='generated function list ' + os.path.relpath(paths[0], REPO), funcs=funcs, lo=min(funcs),
                hi=max(funcs) + 4, image_size=None, stale=[], sized=False)


def load_coverage(path):
    try:
        with open(path, encoding='utf-8') as f:
            d = json.load(f)
        fn = d.get('functions')
        if isinstance(fn, dict):
            return {int(k, 16): v for k, v in fn.items()}
    except (OSError, ValueError, AttributeError):
        pass
    return None


def entry_kind(e):
    a = e.get('address') if isinstance(e, dict) else None
    if isinstance(a, str) and ADDR_RE.match(a.strip()):
        return 'single'
    return 'layout'


def validate(args):
    rep = Report()
    files = sorted(glob.glob(os.path.join(args.db, '*.json')))
    if not os.path.isdir(args.db) or not files:
        print('validate: no db files in %s' % args.db, file=sys.stderr)
        return None, 2
    fn_info = None
    if not args.no_index:
        fn_info = load_functions_from_index(args.index)
    if fn_info is None:
        fn_info = load_functions_from_gen(args.gen)
    if fn_info is None:
        rep.notes.append('no code index and no generated function list: address checks skipped')
    else:
        rep.notes.append('function starts from the %s (%d functions)' % (fn_info['source'], len(fn_info['funcs'])))
        for s in fn_info['stale']:
            rep.warn('index_stale', 'index', s)
    starts_sorted = sorted(fn_info['funcs']) if fn_info else []
    coverage = load_coverage(args.coverage)
    if coverage is not None:
        rep.notes.append('scenario coverage from %s (%d functions seen)' % (os.path.relpath(args.coverage, REPO),
                                                                            len(coverage)))

    entries = []   # (file stem, index, entry, kind, addr or None)
    for p in files:
        stem = os.path.splitext(os.path.basename(p))[0]
        rel = os.path.relpath(p, REPO)
        try:
            with open(p, encoding='utf-8') as f:
                raw = f.read()
            data = json.loads(raw)
        except (OSError, ValueError) as e:
            rep.err('parse', rel, 'does not parse: %s' % e)
            continue
        if isinstance(data, dict) and isinstance(data.get('functions'), list):
            rep.warn('top_level', rel, 'top level is an object with "functions"; the other db files use a plain list')
            data = data['functions']
        if not isinstance(data, list):
            rep.err('top_level', rel, 'top level is not a list of entries')
            continue
        if raw and not raw.endswith('\n'):
            rep.warn('format', rel, 'no newline at the end of the file')
        for i, e in enumerate(data):
            where = '%s[%d]' % (rel, i)
            if not isinstance(e, dict):
                rep.err('entry_type', where, 'entry is not an object')
                continue
            name = e.get('name')
            if isinstance(name, str) and name:
                where = '%s %s' % (rel, name)
            check_entry(rep, where, e)
            kind = entry_kind(e)
            addr = int(e['address'].strip(), 16) if kind == 'single' else None
            entries.append((stem, i, e, kind, addr, rel, where))

    check_addresses(rep, entries, fn_info, starts_sorted, args)
    check_duplicates(rep, entries)
    check_docs(rep, files, args.docs)
    cov = coverage_tables(entries, fn_info, coverage)
    forms = collections.Counter(address_form_kind(e.get('address')) for _, _, e, kind, _, _, _ in entries
                                if kind == 'layout' and isinstance(e.get('address'), str) and e['address'].strip())
    return dict(report=rep, coverage=cov, files=len(files), entries=len(entries),
                address_forms=dict(forms)), (1 if rep.errors else 0)


def check_entry(rep, where, e):
    for k in FIELDS:
        if k not in e:
            rep.err('missing_field', where, 'missing field "%s"' % k)
    extra = sorted(set(e) - set(FIELDS))
    if extra:
        rep.warn('extra_field', where, 'fields outside the schema: %s' % ', '.join(extra))
    a = e.get('address')
    if 'address' in e:
        if not isinstance(a, str) or not a.strip():
            rep.err('address_type', where, 'address is not a non-empty string')
        elif not ADDR_RE.match(a.strip()):
            rep.err('address_form', where, 'address %r is not one 8-digit hex guest address (%s)'
                    % (a, ADDRESS_FORM_LABEL[address_form_kind(a)]))
        else:
            if a != a.strip():
                rep.warn('address_style', where, 'address has surrounding spaces')
            if re.search(r'[a-f]', a.strip()[2:]):
                rep.warn('address_style', where, 'address %s uses lower-case hex digits (the db uses upper case)' % a)
    for k in ('name', 'summary', 'source'):
        if k in e and (not isinstance(e[k], str) or not e[k].strip()):
            rep.err('empty_field', where, '%s is empty or not a string' % k)
    name = e.get('name')
    if isinstance(name, str) and name.strip() and not NAME_RE.match(name):
        rep.warn('name_style', where, 'name %r is not snake_case' % name)
    conf = e.get('confidence')
    if 'confidence' in e and conf not in CONFIDENCE:
        rep.err('confidence', where, 'confidence %r not in %s' % (conf, '|'.join(CONFIDENCE)))
    sysv = e.get('system')
    if 'system' in e and sysv not in SYSTEMS:
        rep.err('system', where, 'system %r not in %s' % (sysv, '|'.join(SYSTEMS)))
    ev = e.get('evidence')
    if 'evidence' in e:
        if not isinstance(ev, list):
            rep.err('evidence_type', where, 'evidence is not a list')
        else:
            bad = [i for i, x in enumerate(ev) if not isinstance(x, str) or not x.strip()]
            if bad:
                rep.err('evidence_type', where, 'evidence items %s are empty or not strings' % bad)
            if conf in ('high', 'medium') and not [x for x in ev if isinstance(x, str) and x.strip()]:
                rep.err('evidence_required', where, 'confidence %s with no evidence (CLAUDE.md: evidence is mandatory '
                                                    'above low)' % conf)
            for x in ev:
                if isinstance(x, str) and len(x) > EVIDENCE_MAX:
                    rep.warn('long_text', where, 'evidence item of %d characters (pasted listing?)' % len(x))
    texts = [e.get('summary')] + (ev if isinstance(ev, list) else [])
    for x in texts:
        if isinstance(x, str) and GENCODE_RE.search(x):
            rep.warn('generated_code', where, 'text looks like generated code (%r): db holds summaries only'
                     % GENCODE_RE.search(x).group(0))
            break
    instrs = [m.group(0) for x in texts if isinstance(x, str) for m in DISASM_RE.finditer(x)]
    if len(instrs) >= DISASM_WARN_AT:
        rep.warn('disassembly', where, '%d quoted PPC instructions (%s): db holds summaries, not listings'
                 % (len(instrs), ', '.join(repr(x) for x in instrs[:3]) + (', ...' if len(instrs) > 3 else '')))
    elif instrs:
        rep.disasm_cites += 1
    s = e.get('summary')
    if isinstance(s, str) and len(s) > SUMMARY_MAX:
        rep.warn('long_text', where, 'summary of %d characters' % len(s))


def check_addresses(rep, entries, fn_info, starts_sorted, args):
    import bisect
    for stem, i, e, kind, addr, rel, where in entries:
        if kind == 'layout':
            continue
        if fn_info is None:
            continue
        funcs = fn_info['funcs']
        if fn_info['lo'] <= addr < fn_info['hi']:
            if addr in funcs:
                continue
            k = bisect.bisect_right(starts_sorted, addr) - 1
            if k >= 0:
                start = starts_sorted[k]
                size, _ = funcs[start]
                if fn_info['sized'] and addr < start + size:
                    rep.err('not_function_start', where, '%s is inside sub_%08X (+0x%X), not a function start'
                            % (h(addr), start, addr - start))
                elif fn_info['sized']:
                    rep.err('not_function_start', where, '%s is in the code range but in no function (after '
                            'sub_%08X, size 0x%X)' % (h(addr), start, size))
                else:
                    rep.err('not_function_start', where, '%s is not a function start (nearest start below: '
                            'sub_%08X; no sizes without the index)' % (h(addr), start))
            else:
                rep.err('not_function_start', where, '%s is in the code range but below the first function' % h(addr))
        else:
            size = fn_info.get('image_size')
            if size and not (IMAGE_BASE <= addr < IMAGE_BASE + size):
                rep.warn('outside_image', where, 'data address %s is outside the loaded image 0x%08X..0x%08X'
                         % (h(addr), IMAGE_BASE, IMAGE_BASE + size))
    if args.warn_layout:
        moved = [x for x in rep.errors if x[0] == 'address_form']
        rep.errors = [x for x in rep.errors if x[0] != 'address_form']
        rep.warnings.extend(moved)


def check_duplicates(rep, entries):
    by_file_addr = collections.defaultdict(list)
    by_file_name = collections.defaultdict(list)
    by_addr = collections.defaultdict(list)
    by_name = collections.defaultdict(set)
    for stem, i, e, kind, addr, rel, where in entries:
        name = e.get('name')
        if kind == 'single':
            by_file_addr[(rel, addr)].append(e)
            by_addr[addr].append((rel, e))
        if isinstance(name, str) and name:
            by_file_name[(rel, name)].append(e)
            if kind == 'single':
                by_name[name].add(addr)
    for (rel, addr), es in sorted(by_file_addr.items()):
        if len(es) > 1:
            same = all(json.dumps(x, sort_keys=True) == json.dumps(es[0], sort_keys=True) for x in es)
            rep.err('duplicate_in_file', rel, '%s appears %d times%s: %s' % (
                h(addr), len(es), ' (identical copies)' if same else '', ', '.join(str(x.get('name')) for x in es)))
    for (rel, name), es in sorted(by_file_name.items()):
        if len(es) > 1:
            addrs = sorted({str(x.get('address')) for x in es})
            if len(addrs) > 1 or entry_kind(es[0]) == 'layout':
                rep.err('duplicate_name_in_file', rel, 'name %s used %d times (%s)' % (name, len(es), ', '.join(addrs)))
    for name, addrs in sorted(by_name.items()):
        if len(addrs) > 1:
            rep.err('name_conflict', 'db', 'name %s names different addresses: %s'
                    % (name, ', '.join(h(a) for a in sorted(addrs))))
    for addr, items in sorted(by_addr.items()):
        files = sorted({rel for rel, _ in items})
        if len(files) < 2:
            continue
        names = sorted({str(e.get('name')) for _, e in items})
        desc = '; '.join('%s: %s (%s, %s)' % (os.path.basename(rel), e.get('name'), e.get('confidence'),
                                              e.get('system')) for rel, e in items)
        if len(names) > 1:
            rep.warn('cross_file_names', h(addr), 'different names in %d files: %s' % (len(files), desc))
        else:
            confs = {hashable(e.get('confidence')) for _, e in items}
            systems = {hashable(e.get('system')) for _, e in items}
            if len(confs) > 1 or len(systems) > 1:
                rep.warn('cross_file_fields', h(addr), 'same name, different %s: %s' % (
                    ' and '.join(x for x, s in (('confidence', confs), ('system', systems)) if len(s) > 1), desc))
            else:
                rep.notes.append('shared entry %s %s in %s' % (h(addr), names[0], ', '.join(
                    os.path.basename(f) for f in files)))
    for stem, i, e, kind, addr, rel, where in entries:
        is_key = stem in KEY_FILES or e.get('system') in KEY_SYSTEMS
        if not is_key:
            continue
        ev = [x for x in (e.get('evidence') if isinstance(e.get('evidence'), list) else []) if isinstance(x, str)]
        item = next((x for x in ev if LABELS_ITEM_RE.match(x) and re.match(r'^\s*labels?\b', x, re.I)), None)
        texts = [e.get('source') if isinstance(e.get('source'), str) else ''] + ev
        if item is not None:
            status, st = label_status(item)
            short = item.strip()
            short = short if len(short) <= 110 else short[:107] + '...'
            detail = 'A %s, B %s: %r' % (st['A'] or 'not named', st['B'] or 'not named', short)
        else:
            hit = next((t for t in texts if NO_LABEL_RE.search(t)), None)
            if hit is not None:
                status, detail = 'none', 'no labels item; %r' % NO_LABEL_RE.search(hit).group(0)
            else:
                hit = next((t for t in texts if SINGLE_LABEL_RE.search(t)), None)
                if hit is not None:
                    status, detail = 'single', 'no labels item; %r' % SINGLE_LABEL_RE.search(hit).group(0)
                else:
                    status, detail = 'unparsed', 'no "labels:" evidence item and no single-label phrase'
        rep.labels.setdefault(stem, collections.Counter())[status] += 1
        if status == 'single':
            rep.warn('key_single_label', where, 'key-system entry with one labeller only (%s)' % detail)
        elif status == 'partial':
            rep.warn('key_partial_label', where, 'second labeller only mentions it (%s)' % detail)
        elif status == 'none':
            rep.warn('key_no_label', where, 'key-system entry labelled by neither labeller (%s)' % detail)
        elif status == 'unparsed':
            rep.warn('key_label_unparsed', where, 'cannot tell who labelled it (%s)' % detail)


def check_docs(rep, files, docs):
    db_stems = {os.path.splitext(os.path.basename(p))[0] for p in files}
    doc_stems = {os.path.splitext(os.path.basename(p))[0] for p in glob.glob(os.path.join(docs, '*.md'))}
    for s in sorted(doc_stems - db_stems):
        rep.warn('docs_without_db', 'docs/systems/%s.md' % s, 'no db/%s.json' % s)
    for s in sorted(db_stems - doc_stems):
        rep.warn('db_without_docs', 'db/%s.json' % s, 'no docs/systems/%s.md' % s)


def coverage_tables(entries, fn_info, coverage):
    def blank():
        return dict(entries=0, function=0, data=0, layout=0, conf=collections.Counter(), live=0, cov_only=0,
                    live_negated_only=0, fn_with_cov=0)
    per_file = collections.OrderedDict()
    per_system = collections.OrderedDict()
    total = blank()
    funcs = fn_info['funcs'] if fn_info else {}
    lo = fn_info['lo'] if fn_info else 0
    hi = fn_info['hi'] if fn_info else 0
    for stem, i, e, kind, addr, rel, where in entries:
        sysv = e.get('system') if e.get('system') in SYSTEMS else '(invalid)'
        rows = [per_file.setdefault(stem, blank()), per_system.setdefault(sysv, blank()), total]
        ev = ' | '.join(x for x in (e.get('evidence') or []) if isinstance(x, str)) if isinstance(
            e.get('evidence'), list) else ''
        lk = live_kind(ev)
        live = lk in ('live', 'coverage')
        cov_only = lk == 'coverage'
        negated = lk == 'negated'
        if kind == 'layout':
            k = 'layout'
        elif fn_info and lo <= addr < hi and addr in funcs:
            k = 'function'
        elif fn_info and lo <= addr < hi:
            k = 'function'  # not a start (reported as an error); counted with functions
        else:
            k = 'data' if fn_info else 'function'
        ran = k == 'function' and coverage is not None and addr is not None and addr in coverage
        for r in rows:
            r['entries'] += 1
            r[k] += 1
            r['conf'][e.get('confidence') if e.get('confidence') in CONFIDENCE else '(invalid)'] += 1
            r['live'] += live
            r['cov_only'] += cov_only
            r['live_negated_only'] += negated
            r['fn_with_cov'] += ran
    for d in (per_file, per_system):
        for k in list(d):
            d[k] = finish(d[k], coverage is not None)
    return dict(per_file=per_file, per_system=per_system, total=finish(total, coverage is not None),
                coverage_available=coverage is not None)


def finish(r, have_cov):
    n = r['entries'] or 1
    out = dict(r)
    out['conf'] = dict(r['conf'])
    out['live_pct'] = 100.0 * r['live'] / n
    out['cov_only_pct'] = 100.0 * r['cov_only'] / n
    out['ran_pct'] = (100.0 * r['fn_with_cov'] / r['function']) if (have_cov and r['function']) else None
    return out


def print_text(res, args):
    rep = res['report']
    print('db validate: %d files, %d entries' % (res['files'], res['entries']))
    for n in rep.notes:
        if not n.startswith('shared entry'):
            print('  ' + n)
    shared = [n for n in rep.notes if n.startswith('shared entry')]
    if shared:
        print('  %d addresses are shared by several files with the same name/confidence/system (fine)' % len(shared))

    def grouped(items, title):
        print('\n%s: %d' % (title, len(items)))
        by = collections.OrderedDict()
        for kind, where, msg in items:
            by.setdefault(kind, []).append((where, msg))
        for kind, rows in by.items():
            print('  [%s] %d' % (kind, len(rows)))
            for where, msg in rows[:args.list]:
                print('    %s: %s' % (where, msg))
            if len(rows) > args.list:
                print('    ... %d more (--list N)' % (len(rows) - args.list))
    grouped(rep.errors, 'ERRORS')
    grouped(rep.warnings, 'WARNINGS')
    af = res['address_forms']
    if af:
        print('\naddress_form entries by kind: ' + ', '.join('%s %d' % (ADDRESS_FORM_LABEL[k], af[k])
                                                           for k in ADDRESS_FORM_LABEL if af.get(k)))
    if rep.disasm_cites:
        print('%d more entries quote 1-%d PPC instructions (citations; warned from %d per entry)'
              % (rep.disasm_cites, DISASM_WARN_AT - 1, DISASM_WARN_AT))
    if rep.labels:
        print('\nKEY-SYSTEM LABELS per db file (two = both labellers gave a label; partial = the second only mentions '
              'it; single = one labeller; none = neither; unparsed = review by hand)')
        cols = ('two', 'partial', 'single', 'none', 'unparsed')
        print('%-18s %5s ' % ('', 'n') + ' '.join('%8s' % c for c in cols) + '   two%')
        tot = collections.Counter()
        for stem, c in rep.labels.items():
            tot.update(c)
            n = sum(c.values())
            print('%-18s %5d ' % (stem, n) + ' '.join('%8d' % c.get(k, 0) for k in cols) +
                  '  %4.0f%%' % (100.0 * c.get('two', 0) / max(1, n)))
        n = sum(tot.values())
        print('%-18s %5d ' % ('TOTAL', n) + ' '.join('%8d' % tot.get(k, 0) for k in cols) +
              '  %4.0f%%' % (100.0 * tot.get('two', 0) / max(1, n)))

    cov = res['coverage']
    hdr = ('%-18s %5s %5s %5s %6s | %4s %4s %4s %4s | %6s %6s %6s' %
           ('', 'n', 'func', 'data', 'layout', 'high', 'med', 'low', 'unk', 'live%', 'covonl', 'ran%'))

    def row(label, r):
        c = r['conf']
        ran = '%5.0f%%' % r['ran_pct'] if r['ran_pct'] is not None else '     -'
        return ('%-18s %5d %5d %5d %6d | %4d %4d %4d %4d | %5.0f%% %5.0f%% %s' %
                (label, r['entries'], r['function'], r['data'], r['layout'], c.get('high', 0), c.get('medium', 0),
                 c.get('low', 0), c.get('unknown', 0), r['live_pct'], r['cov_only_pct'], ran))
    print('\nCOVERAGE per db file (live% = evidence names a live run/trace/watch/hook/coverage; covonl = of all '
          'entries, live only through coverage; ran% = function entries seen in scenario coverage)')
    print(hdr)
    for k, r in cov['per_file'].items():
        print(row(k, r))
    print(row('TOTAL', cov['total']))
    print('\nCOVERAGE per system field')
    print(hdr)
    for k, r in cov['per_system'].items():
        print(row(k, r))
    if cov['total'].get('live_negated_only'):
        print('(%d entries mention live keywords only in negated form, e.g. "not traced", "never ran in coverage": '
              'not counted as live)' % cov['total']['live_negated_only'])
    print('\nresult: %d errors, %d warnings -> exit %d' % (len(rep.errors), len(rep.warnings),
                                                          1 if rep.errors else 0))


def main():
    try:
        return run()
    except SystemExit:
        raise
    except Exception:  # an internal error must not look like 'db has errors' (exit 1)
        import traceback
        traceback.print_exc()
        print('validate: internal error (exit 3); this is a bug in tools/db/validate.py, not a db verdict',
              file=sys.stderr)
        return 3


def run():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--db', default=DEFAULT_DB)
    ap.add_argument('--docs', default=DEFAULT_DOCS)
    ap.add_argument('--index', default=DEFAULT_INDEX)
    ap.add_argument('--no-index', action='store_true', help='use the generated function list instead of the index')
    ap.add_argument('--gen', default=DEFAULT_GEN)
    ap.add_argument('--coverage', default=DEFAULT_COVERAGE)
    ap.add_argument('--warn-layout', action='store_true',
                    help='report non-address (layout/field/range) entries as warnings, not errors')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--list', type=int, default=40, help='lines per error/warning kind in text mode')
    args = ap.parse_args()
    res, code = validate(args)
    if res is None:
        return code
    if args.json:
        rep = res['report']
        out = dict(files=res['files'], entries=res['entries'], notes=rep.notes,
                   errors=[dict(kind=k, where=w, message=m) for k, w, m in rep.errors],
                   warnings=[dict(kind=k, where=w, message=m) for k, w, m in rep.warnings],
                   address_forms=res['address_forms'], disassembly_citations=rep.disasm_cites,
                   key_labels={k: dict(v) for k, v in rep.labels.items()},
                   coverage=res['coverage'], exit=code)
        print(json.dumps(out, indent=1))
    else:
        try:
            sys.stdout.reconfigure(errors='replace')
        except Exception:
            pass
        print_text(res, args)
    return code


if __name__ == '__main__':
    sys.exit(main())
