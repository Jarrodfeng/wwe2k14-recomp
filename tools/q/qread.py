r"""q read (S110 lane READSTORE): what a model reader settled about a cluster of game functions goes into the shared index.

    py tools/q/q.py read pick --feed [--n 30] [--out DIR]     units to read next for the scene feed (sheets + batch.txt into DIR)
    py tools/q/q.py read store ANSWER_FILE [--batch NAME] [--model NAME]     parse a sheet-reader answer into read.s1.db
    py tools/q/q.py read show [UNIT|ADDR]                      the stored answers of a unit (newest first); no argument: every unit read
    py tools/q/q.py read verify unit UNIT|func ADDR --verdict V --source PATH --note T | --file TSV | --stats   (tools/q/qreadverify.py)
    py tools/q/q.py read stats                               totals (and units per system)
    py tools/q/q.py read pick --reread [--n N] --out DIR       sheets of units answered but with no `system` yet (to be re-asked)
    py tools/q/q.py read system NAME [--out FILE]              the bundle a system-writer reads: every stored reading of that system
    py tools/q/q.py read systems                               systems, unit counts, written explanation stored?, stale?
    py tools/q/q.py read store-system ANSWER_FILE [--model NAME]   parse a system-writer's answer (tools/q/qreadsys.py)

Table file: out/harness/index/read.s2.db (not rebuilt from anything: it IS the store; delete it to forget; made from read.s1.db on first use).
  unit_read(id, unit_id, purpose, confidence, because, missing, listing_needed, batch, model, when_, system, fun)   one row per answer, never overwritten
  name_read(id, func, name, evidence, confidence, unit_id, batch, model)      reader-proposed names
  pattern_read(id, text, batch, model)
  system_read(id, system, explain, model, when_)                              a system-writer's explanation of one system
  fact_read(id, text, unit_id, confidence, system, source, when_)             one-line curiosities; source = reader | writer

SOUNDNESS: a reader-proposed name is a CANDIDATE with its evidence, never the game's own ([g]) or our database's, never an anchor
for `q cover`, never written into db/*.json: a person or a probe confirms it.
Input format: the `Answer format` block of .claude/agents/sheet-reader.md. A block that cannot be parsed is reported with its
label and SKIPPED, never guessed.
"""
import collections
import os
import re
import sqlite3
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qnames  # noqa: E402
import qunits  # noqa: E402

READ_DB = os.path.join(qnames.IDX, 'read.s2.db')
SYSTEMS = ('pose', 'model', 'material', 'arena', 'crowd', 'light', 'particle', 'camera', 'screens', 'liquids', 'ui', 'audio',
           'menus', 'creation', 'match', 'ai', 'engine', 'other', 'unknown')
CANDIDATE = ('reader-proposed names are candidates with evidence, not facts: never [g], never our db, never a `q cover` anchor, '
             'never in db/*.json until a person or a probe confirms them.')
# Functions that only copy or fill memory write the bytes of every read site and say nothing about the scene.
# tools/q/qcopier.py supersedes this list when it exists.
COPIER_WORDS = ('memset', 'memcpy', 'memmove')
CONFS = ('high', 'medium', 'low')


class ReadError(Exception):
    pass


# ---------------------------------------------------------------- parser
def conf_of(s):
    """'high' | 'medium' | 'low' | None. `high-ish (medium)` -> medium (a parenthesised word wins), else the first word."""
    s = (s or '').lower()
    m = re.search(r'\((high|medium|low)\)', s) or re.search(r'\b(high|medium|low)\b', s)
    return m.group(1) if m else None


def clean(line):
    s = line.strip()
    s = re.sub(r'^[-*•]\s+', '', s)
    s = s.replace('**', '').replace('`', '')
    return s.strip()


KEY = re.compile(r'^(unit|purpose|system|fun|missing|listing needed|names|wrong)\s*:\s*(.*)$', re.I)
WRONG = re.compile(r'^(?:sub_|0x)?([0-9A-Fa-f]{8})\s+(\S+)\s*\|\s*(.+)$')   # wrong: sub_ADDR existing_name | why it does not fit
ENTRY = re.compile(r'^(?:sub_|0x)?([0-9A-Fa-f]{8})\s*(?:->|=>|→)\s*(\S+)\s*(.*)$')
NONE = re.compile(r'^\(?\s*none\b', re.I)


def parse_entry(text):
    """-> (func, name, evidence, conf) or None"""
    m = ENTRY.match(text)
    if not m:
        return None
    name = m.group(2).strip().strip('.,;')
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', name):
        return None
    parts = [p.strip() for p in m.group(3).split('|')]
    parts = [p for p in parts if p]
    conf = conf_of(parts[-1]) if parts else None
    if conf is None:
        return None
    ev = parts[:-1]
    if ev and ev[0].lower().startswith('evidence:'):
        ev[0] = ev[0][9:].strip()
    return int(m.group(1), 16), name, ' | '.join(ev), conf


def parse_answer(text):
    """-> (blocks, patterns, skipped). blocks: dicts with label, unit_id, purpose, confidence, because, missing, listing, names, bad_names.
    skipped: [(label, reason)]."""
    raw, cur, pat = [], None, None
    for line in text.splitlines():
        h = re.match(r'^\s*#{2,3}\s*(.*?)\s*$', line)
        if h:
            if h.group(1).strip().lower() == 'patterns':
                cur, pat = None, []
                continue
            cur = {'label': h.group(1).strip(), 'lines': []}
            raw.append(cur)
            pat = None
            continue
        if pat is not None:
            s = clean(line)
            if s and not NONE.match(s):
                pat.append(s)
        elif cur is not None:
            cur['lines'].append(line)
    blocks, skipped = [], []
    for b in raw:
        d = {'label': b['label'], 'unit_id': None, 'purpose': None, 'confidence': None, 'because': '', 'missing': '',
             'listing': None, 'names': [], 'bad_names': [], 'system': '', 'fun': '', 'wrong': []}
        in_names = False
        for line in b['lines']:
            s = clean(line)
            if not s:
                continue
            k = KEY.match(s)
            if k:
                key, rest = k.group(1).lower(), k.group(2).strip()
                in_names = False
                if key == 'unit':
                    m = re.match(r'(\d+)\b', rest)
                    d['unit_id'] = int(m.group(1)) if m else d['unit_id']
                elif key == 'purpose':
                    m = re.match(r'^(.*?)\s*\|\s*confidence\s*:\s*(.*?)\s*(?:\|\s*because\s*:\s*(.*))?$', rest, re.I)
                    if m:
                        d['purpose'], d['confidence'], d['because'] = m.group(1).strip(), conf_of(m.group(2)), (m.group(3) or '').strip()
                elif key == 'system':       # absent or not one of SYSTEMS -> '' (never guessed)
                    w = re.match(r'[a-z]+', rest.lower())
                    d['system'] = w.group(0) if w and w.group(0) in SYSTEMS else ''
                elif key == 'fun':
                    d['fun'] = '' if re.fullmatch(r'"?none"?\.?', rest.strip(), re.I) else rest.strip().strip('"').strip()
                elif key == 'wrong':
                    w = WRONG.match(rest)
                    if w:
                        d['wrong'].append((int(w.group(1), 16), w.group(2), w.group(3).strip()))
                elif key == 'missing':
                    d['missing'] = rest
                elif key == 'listing needed':
                    d['listing'] = rest.lower().startswith('yes')
                elif key == 'names':
                    if not rest:
                        in_names = True
                    elif not NONE.match(rest):
                        e = parse_entry(rest)
                        (d['names'].append(e) if e else d['bad_names'].append(rest))
                continue
            if in_names and not NONE.match(s):
                e = parse_entry(s)
                (d['names'].append(e) if e else d['bad_names'].append(s))
        if d['unit_id'] is None and d['purpose'] is None:
            skipped.append((d['label'], 'no unit / purpose line (a back-reference such as "same unit as X, answered above"?)'))
        elif d['unit_id'] is None:
            skipped.append((d['label'], 'no unit: line'))
        elif d['purpose'] is None:
            skipped.append((d['label'], 'no parsable purpose line'))
        elif d['confidence'] is None:
            skipped.append((d['label'], 'no confidence high|medium|low'))
        else:
            blocks.append(d)
    return blocks, pat or [], skipped


# ---------------------------------------------------------------- store
SCHEMA = ('create table if not exists unit_read(id integer primary key autoincrement, unit_id integer, purpose text, confidence text, because text,'
          ' missing text, listing_needed integer, batch text, model text, when_ text, system text default \'\', fun text default \'\');'
          'create table if not exists system_read(id integer primary key autoincrement, system text, explain text, model text, when_ text);'
          'create table if not exists fact_read(id integer primary key autoincrement, text text, unit_id integer, confidence text, system text,'
          ' source text, when_ text);'
          'create table if not exists name_read(id integer primary key autoincrement, func integer, name text, evidence text, confidence text,'
          ' unit_id integer, batch text, model text);'
          'create table if not exists pattern_read(id integer primary key autoincrement, text text, batch text, model text);'
          'create table if not exists wrong_read(id integer primary key autoincrement, func integer, name text, why text, unit_id integer,'
          ' batch text, model text);'
          'create index if not exists unit_read_u on unit_read(unit_id);'
          'create index if not exists name_read_f on name_read(func);')


def migrate(path):
    """read.s2.db does not exist but its sibling read.s1.db does: build s2 from it (system and fun empty). Once."""
    old = os.path.join(os.path.dirname(path), 'read.s1.db')
    if os.path.basename(path) != 'read.s2.db' or os.path.exists(path) or not os.path.exists(old):
        return
    c = sqlite3.connect(path)
    c.executescript(SCHEMA)
    c.execute('attach database ? as old', (old,))
    c.execute("insert into unit_read(id,unit_id,purpose,confidence,because,missing,listing_needed,batch,model,when_,system,fun) "
              "select id,unit_id,purpose,confidence,because,missing,listing_needed,batch,model,when_,'','' from old.unit_read")
    c.execute('insert into name_read select * from old.name_read')
    c.execute('insert into pattern_read select * from old.pattern_read')
    c.commit()
    c.execute('detach database old')
    c.close()


def open_rw(path=None):
    path = path or READ_DB
    os.makedirs(os.path.dirname(path), exist_ok=True)
    migrate(path)
    c = sqlite3.connect(path)
    c.executescript(SCHEMA)
    return c


def open_ro(path=None):
    """read-only connection, or None when nothing has been stored yet"""
    path = path or READ_DB
    migrate(path)
    if not os.path.exists(path):
        return None
    c = sqlite3.connect('file:%s?mode=ro' % path.replace('\\', '/'), uri=True)
    try:
        c.execute('select 1 from unit_read limit 1')
    except sqlite3.Error:
        c.close()
        return None
    return c


def store(text, batch='', model='', path=None):
    """-> dict(parsed, skipped [(label, why)], names, bad_names, listing [unit ids], patterns)"""
    blocks, pats, skipped = parse_answer(text)
    c = open_rw(path)
    now = time.strftime('%Y-%m-%d %H:%M:%S')
    nn = nb = 0
    listing = []
    for d in blocks:
        c.execute('insert into unit_read(unit_id,purpose,confidence,because,missing,listing_needed,batch,model,when_,system,fun) values (?,?,?,?,?,?,?,?,?,?,?)',
                  (d['unit_id'], d['purpose'], d['confidence'], d['because'], d['missing'], 1 if d['listing'] else 0, batch, model, now,
                   d['system'], d['fun']))
        if d['fun']:
            c.execute('insert into fact_read(text,unit_id,confidence,system,source,when_) values (?,?,?,?,?,?)',
                      (d['fun'], d['unit_id'], d['confidence'], d['system'] or 'other', 'reader', now))
        for f, n, ev, cf in d['names']:
            c.execute('insert into name_read(func,name,evidence,confidence,unit_id,batch,model) values (?,?,?,?,?,?,?)', (f, n, ev, cf, d['unit_id'], batch, model))
            nn += 1
        nb += len(d['bad_names'])
        for f, n, why in d['wrong']:
            c.execute('insert into wrong_read(func,name,why,unit_id,batch,model) values (?,?,?,?,?,?)', (f, n, why, d['unit_id'], batch, model))
        if d['listing']:
            listing.append(d['unit_id'])
    for p in pats:
        c.execute('insert into pattern_read(text,batch,model) values (?,?,?)', (p, batch, model))
    c.commit()
    c.close()
    return {'parsed': len(blocks), 'skipped': skipped, 'names': nn, 'bad_names': nb, 'listing': listing, 'patterns': len(pats)}


def unit_answer(uid, path=None):
    """newest stored answer of a unit as a dict, or None"""
    c = open_ro(path)
    if c is None:
        return None
    try:
        r = c.execute('select purpose,confidence,model,batch from unit_read where unit_id=? order by id desc limit 1', (uid,)).fetchone()
    finally:
        c.close()
    return dict(zip(('purpose', 'confidence', 'model', 'batch'), r)) if r else None


def read_line(uid, path=None):
    a = unit_answer(uid, path)
    if not a:
        return None
    return 'read: %s (%s, %s, %s)' % (a['purpose'], a['confidence'], a['model'] or '?', a['batch'] or '?')


def proposed_names(lo, hi, path=None):
    """{func: (name, confidence)} newest proposal per function between lo and hi"""
    c = open_ro(path)
    if c is None:
        return {}
    try:
        out = {}
        for f, n, cf in c.execute('select func,name,confidence from name_read where func between ? and ? order by id', (lo, hi)):
            out[f] = (n, cf)
        return out
    finally:
        c.close()


# ---------------------------------------------------------------- pick
def is_copier(label):
    s = (label or '').lower()
    return any(w in s for w in COPIER_WORDS)


def pick_feed(n, outdir, ctx=None, feed=None, units=None, path=None):
    """-> [(unit_id, sites, [(func, name_or_None, sites)], bytes)] ranked; writes sheets + batch.txt when outdir"""
    import qfeedseen
    import qsheet
    fc = feed or qfeedseen.open_db()
    uc = units or qunits.open_units()
    ctx = ctx or qsheet.Ctx()
    answered = set()
    rc = open_ro(path)
    if rc is not None:
        answered = {r[0] for r in rc.execute('select distinct unit_id from unit_read')}
        rc.close()
    big = {r[0] for r in uc.execute('select unit_id from unit where n_funcs>=3')}
    uof = dict(uc.execute('select func,unit_id from units'))
    per = collections.defaultdict(lambda: collections.defaultdict(set))     # unit -> func -> {(file,line)}
    for f, fl, ln in fc.execute('select func,file,line from writers where func<>0'):
        u = uof.get(f)
        if u is None or u not in big or u in answered or is_copier(ctx.label(f)):
            continue
        per[u][f].add((fl, ln))
    rank = []
    for u, fs in per.items():
        sites = set()
        for v in fs.values():
            sites |= v
        rank.append((u, len(sites), sorted(((f, ctx.label(f), len(v)) for f, v in fs.items()), key=lambda x: (-x[2], x[0]))))
    rank.sort(key=lambda r: (-r[1], r[0]))
    rank = rank[:n]
    res = []
    if outdir:
        os.makedirs(outdir, exist_ok=True)
    if outdir:
        ctx.hooks()
    for u, sites, fs in rank:
        nbytes = 0
        if outdir:
            text, fl = qsheet.build(ctx, u)
            nbytes = fl['bytes']
            with open(os.path.join(outdir, '%d.txt' % u), 'w', encoding='utf-8') as fh:
                fh.write(text)
        res.append((u, sites, fs, nbytes))
    if outdir:
        with open(os.path.join(outdir, 'batch.txt'), 'w', encoding='utf-8') as fh:
            for u, sites, fs, b in res:
                top = ' '.join('%s(%d)' % (nm or 'sub_%08X' % f, k) for f, nm, k in fs[:4])
                fh.write('%d.txt\tunit %d\t%d read sites\t%d B\t%s\n' % (u, u, sites, b, top))
    return res


# ---------------------------------------------------------------- show / stats
def resolve_unit(arg):
    uc = qunits.open_units()
    if re.fullmatch(r'\d{1,6}', arg):
        return int(arg)
    a = qnames.parse_addr(arg)
    if a is None:
        raise ReadError('q read show takes a unit id (decimal) or a function address, not %r' % arg)
    r = uc.execute('select unit_id from units where func=?', (a,)).fetchone()
    if not r:
        o = qnames.owner(qnames.ro(qnames.INDEX_DB), a)
        r = uc.execute('select unit_id from units where func=?', (o,)).fetchone() if o else None
    if not r:
        raise ReadError('%s is not inside a unit' % arg)
    return r[0]


def show(arg, path=None, out=print):
    c = open_ro(path)
    if c is None:
        out('nothing stored yet (%s)' % (path or READ_DB))
        return
    if not arg:
        for u, n, cf, b in c.execute('select unit_id,count(*),(select confidence from unit_read x where x.unit_id=y.unit_id order by id desc limit 1),'
                                    '(select purpose from unit_read x where x.unit_id=y.unit_id order by id desc limit 1) from unit_read y group by unit_id order by unit_id'):
            out('unit %d  %d answer(s)  newest: %s  %s' % (u, n, cf, (b or '')[:100]))
        return
    uid = int(arg) if isinstance(arg, int) else resolve_unit(arg)
    rows = c.execute('select id,purpose,confidence,because,missing,listing_needed,batch,model,when_,system,fun from unit_read where unit_id=? order by id desc', (uid,)).fetchall()
    if not rows:
        out('unit %d: no stored answer' % uid)
        import qreadverify
        for x in qreadverify.show_lines(uid, path):
            out(x)
        return
    for i, p, cf, bc, ms, ln, b, m, w, sy, fu in rows:
        out('unit %d  %s  model %s  batch %s' % (uid, w, m or '?', b or '?'))
        out('  purpose: %s (%s)' % (p, cf))
        out('  system: %s   fun: %s' % (sy or '(not stated)', fu or '(none)'))
        out('  because: %s' % bc)
        out('  missing: %s   listing needed: %s' % (ms, 'yes' if ln else 'no'))
    for f, n, ev, cf, m, b in c.execute('select func,name,evidence,confidence,model,batch from name_read where unit_id=? order by id desc', (uid,)):
        out('  name sub_%08X -> %s [r:%s] | %s | %s %s' % (f, n, cf, ev, m or '?', b or '?'))
    import qreadverify      # VERIFYMARK: what a probe confirmed or refuted
    for x in qreadverify.show_lines(uid, path):
        out(x)
    out(CANDIDATE)


def stats(path=None, units=None, out=print):
    c = open_ro(path)
    uc = units or qunits.open_units()
    big = {r[0] for r in uc.execute('select unit_id from unit where n_funcs>=3')}
    if c is None:
        out('nothing stored yet')
        return
    latest = {}
    for u, cf, ln in c.execute('select unit_id,confidence,listing_needed from unit_read order by id'):
        latest[u] = (cf, ln)
    mine = {u: v for u, v in latest.items() if u in big}
    by = collections.Counter(v[0] for v in mine.values())
    out('units read: %d of %d with >= 3 functions (%d answered in all, %d answers stored); by confidence: %s' %
        (len(mine), len(big), len(latest), c.execute('select count(*) from unit_read').fetchone()[0],
         ' '.join('%s %d' % (k, by.get(k, 0)) for k in CONFS)))
    lsys = {}
    for u, sy in c.execute('select unit_id,system from unit_read order by id'):
        lsys[u] = sy or ''
    bys = collections.Counter(lsys[u] or '(no system yet)' for u in mine)
    out('units per system (newest answer, units with >= 3 functions): %s' %
        (' '.join('%s %d' % (k, bys[k]) for k in list(SYSTEMS) + ['(no system yet)'] if bys.get(k)) or 'none'))
    nb = collections.Counter(r[0] for r in c.execute('select confidence from name_read'))
    out('names proposed: %d (%s)' % (sum(nb.values()), ' '.join('%s %d' % (k, nb.get(k, 0)) for k in CONFS)))
    out('needing the listing: %d of %d answered units (%.0f%%)' % (sum(v[1] for v in latest.values()), len(latest),
                                                                  100.0 * sum(v[1] for v in latest.values()) / max(1, len(latest))))
    pats = collections.Counter()
    first = {}
    for (t,) in c.execute('select text from pattern_read order by id'):
        k = re.sub(r'\s+', ' ', t.lower()).strip()
        pats[k] += 1
        first.setdefault(k, t)
    out('patterns (deduped by text, %d): ' % len(pats))
    for k, n in pats.most_common():
        out('  x%d %s' % (n, first[k]))
    out(CANDIDATE)


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    cmd, rest = argv[0], argv[1:]

    def opt(flag, default=None):
        return rest[rest.index(flag) + 1] if flag in rest and rest.index(flag) + 1 < len(rest) else default
    if cmd in ('system', 'systems', 'store-system') or (cmd == 'pick' and '--reread' in rest):
        import qreadsys
        return qreadsys.main(cmd, rest)
    if cmd == 'pick' and '--all' in rest:
        import qreadpick
        try:
            return qreadpick.main(rest)
        except ValueError as e:
            raise ReadError(str(e))
    if cmd == 'pick':
        if '--feed' not in rest:
            raise ReadError('q read pick needs --feed, --all or --reread')
        res = pick_feed(int(opt('--n', 30)), opt('--out'))
        for u, sites, fs, b in res:
            print('unit %d  %d read sites  %d B  %s' % (u, sites, b, ' '.join('%s(%d)' % (nm or 'sub_%08X' % f, k) for f, nm, k in fs[:4])))
        print('batch: %d units, %d bytes total%s' % (len(res), sum(r[3] for r in res), ' -> ' + opt('--out') if opt('--out') else ''))
        return 0
    if cmd == 'store':
        pos = [x for i, x in enumerate(rest) if not x.startswith('--') and (i == 0 or not rest[i - 1].startswith('--'))]
        if not pos:
            raise ReadError('q read store ANSWER_FILE [--batch NAME] [--model NAME]')
        with open(pos[0], encoding='utf-8', errors='replace') as fh:
            r = store(fh.read(), opt('--batch', ''), opt('--model', ''))
        print('blocks parsed %d, skipped %d; names stored %d (%d name lines unparsed); patterns %d' %
              (r['parsed'], len(r['skipped']), r['names'], r['bad_names'], r['patterns']))
        for lb, why in r['skipped']:
            print('  SKIPPED "%s": %s' % (lb, why))
        print('listing needed: yes for %d unit(s)%s' % (len(r['listing']), ': ' + ' '.join(map(str, r['listing'])) if r['listing'] else ''))
        return 0
    if cmd == 'verify':
        import qreadverify
        return qreadverify.main(rest)
    if cmd == 'show':
        show(rest[0] if rest else None)
        return 0
    if cmd == 'stats':
        stats()
        return 0
    raise ReadError('q read: pick | store | verify | show | stats | system | systems | store-system')


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except (ReadError, qnames.NamesError) as e:
        print('q: %s' % e, file=sys.stderr)
        sys.exit(2)
