r"""q sheet: the tool-made FACT SHEET of one unit (S110 lane SHEETS). Derived facts only (no listing text, no decomp), one function per
block, densest facts first, so that a model can read a sheet instead of a listing.

    py tools/q/q.py sheet UNIT|ADDR [--max-kb 12]          unit id (decimal) or a function address (0x..., sub_..., 8 hex digits)
    py tools/q/q.py sheet --stats [--json] [--max-kb 12] [--out DIR]     size distribution over every unit of >= 3 functions

Sources: units.s1.db (members), names.s1.db (game-carried names), db/*.json (our entries, systems), index.db (sizes, direct edges,
strings, rtti slots), seen.s1.db (OBSERVED indirect callers / targets), vcall.s*.db (static indirect sites: icall, icall_target),
fields.s2.db (field reads / writes), index.db `tables` + `ptrs` (vtable slots: the table catalogue only), the source hooks and mid-asm hooks (qx6.scan_hooks, as qcover does).
Cap: --max-kb (default 12) drops the least informative lines first (fields, then in/out lists, ...) and SAYS how many.
MAY BE INCOMPLETE: it prints what the index sees; a fact that is not there is not "never": see the footer of every sheet.
"""
import bisect
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
import qunits  # noqa: E402
import qseen  # noqa: E402
import qread  # noqa: E402
import qreadverify  # noqa: E402
import qpattern  # noqa: E402
import qpattern_rules2  # noqa: E402

SRC = os.path.dirname(os.path.dirname(HERE))
RANK = {'high': 3, 'medium': 2, 'low': 1, 'unknown': 0}
CONF = {'high': 'h', 'medium': 'm', 'low': 'l', 'unknown': '?'}
PRIO = {'anc': 80, 'strsig': 78, 'obs': 75, 'ind': 70, 'vslot': 65, 'str': 60, 'out': 50, 'in': 55, 'fld': 30}
WIDE = ('open', 'offset', 'cand')
NOISE = re.compile(r'^__(save|rest)')
MAXKB = 12


class SheetError(Exception):
    pass


def ro(path):
    path = path and qnames.newest(path)      # S110 QREBUILD review: a copy waiting beside a held file is the one to read
    if not path or not os.path.exists(path):
        raise SheetError('no file %s' % path)
    return sqlite3.connect('file:%s?mode=ro' % path.replace('\\', '/'), uri=True)


def rng(con, sql, lo, hi):
    return con.execute(sql, (lo, hi)).fetchall()


def clip(s, n):
    s = re.sub(r'\s+', ' ', s or '').strip()
    return s if len(s) <= n else s[:n - 1] + '~'


def lst(items, k):
    items = list(items)
    return ' '.join(items[:k]) + (' +%d' % (len(items) - k) if len(items) > k else '')


SIGNATURE = re.compile(r'__(cdecl|thiscall|stdcall|fastcall)|\w::\w')


def is_signature(t):
    """a string that is a function signature or a qualified name (A::B): it is a name, never cut"""
    return bool(SIGNATURE.search(t or ''))


def clip_str(t, n=80):
    """a string for a str line: a signature / qualified name whole, any other cut at n characters (a cut ends in ~)"""
    s = re.sub(r'\s+', ' ', t or '').strip()
    return s if is_signature(s) else clip(s, n)


def name_words(name):
    """the words of 4+ letters of the member part of a name (after the last :: or .), camel case split, lower case"""
    tail = re.split(r'::|\.', name or '')[-1]
    tail = re.sub(r'([a-z])([A-Z])', r'\1 \2', tail)
    return set(w.lower() for w in re.findall(r'[A-Za-z]{4,}', tail))


def root_words(text):
    """a field root in words, so that the sheet needs no legend: a0 this, aN param N, g:ADDR global 0xADDR, K absolute address, u unknown base"""
    t = text or '?'
    if t == 'a0':
        return 'this'
    m = re.fullmatch(r'a(\d+)', t)
    if m:
        return 'param %s' % m.group(1)
    m = re.fullmatch(r'g:([0-9A-Fa-f]+)', t)
    if m:
        return 'global 0x%s' % m.group(1).upper()
    if t == 'K':
        return 'absolute address'
    if t == 'u':
        return 'unknown base'
    if t.startswith('p('):
        return 'pointer loaded from a field'
    if t.startswith('r:'):
        return 'call result'
    return t


class Ctx:
    """every source opened once. Paths default to the real index; a test passes its own."""

    def __init__(self, index=None, names=None, units=None, seen=None, vcall=None, fields=None, dbdir=None, hooks=None, pattern=None):
        self.ix = ro(index or qnames.INDEX_DB)
        self.units = ro(units or qunits.UNITS_DB)
        self.names = ro(names or qnames.NAMES_DB)
        self.seen = ro(seen or qseen.SEEN_DB)
        self.nruns = self.seen.execute('select count(*) from seen_runs').fetchone()[0]
        self.vc = ro(vcall or qseen.vdb_file())
        self.fd = ro(fields or os.path.join(qnames.IDX, 'fields.s2.db'))
        self.roots = dict(self.fd.execute('select id,text from roots'))
        self.dbdir = dbdir or os.path.join(SRC, 'db')
        self._hooks = hooks
        self.game = collections.defaultdict(list)
        for f, n in self.names.execute('select func,name from names where source!="fn_string_shared" order by name'):
            self.game[f].append(n)
        self.db = {}
        self.dbsys = {}
        for p in glob.glob(os.path.join(self.dbdir, '*.json')):
            try:
                ents = json.load(open(p, encoding='utf-8'))
            except (ValueError, OSError):
                continue
            for e in ents:
                try:
                    a = int(e['address'], 16)
                except (KeyError, ValueError, TypeError):
                    continue
                c = e.get('confidence', 'unknown')
                if a not in self.db or RANK.get(c, 0) > RANK.get(self.db[a][1], 0):
                    self.db[a] = (e.get('name') or '?', c)
                    self.dbsys[a] = e.get('system') or '?'
        self.named = sorted(set(self.game) | set(self.db))
        self.idxname = {}
        for a, n in self.ix.execute("select addr,name from funcs where name is not null and name not like 'sub\\_%' escape '\\'"):
            self.idxname[a] = n
        self.n_units = self.units.execute('select count(*) from unit').fetchone()[0]
        self.pat = qpattern.labels(pattern) if (pattern or not index) else {}     # a test (own index) gets no real labels
        self.prel = qpattern_rules2.relations_of(pattern) if (pattern or not index) else {}   # S110 RULES2: clone relations ([~rel ADDR])

    def hooks(self):
        """-> (hook funcs {func: ['file:line']}, midasm [(addr, name)] sorted); the source scan runs once and only when asked"""
        if self._hooks is None:
            import qx6
            src, mid = qx6.scan_hooks(SRC)
            hf = collections.defaultdict(list)
            for h in src:
                if h.get('tools'):
                    continue
                m = re.search(r'([0-9A-Fa-f]{8})', str(h['target']))
                if m:
                    hf[int(m.group(1), 16)].append('%s:%d' % (os.path.basename(h['file']), h['line']))
            mids = sorted((h['addr'], h.get('name', '?')) for h in mid if h.get('addr'))
            self._hooks = (dict(hf), mids)
        return self._hooks

    def label(self, a):
        """the best name of a function, or None: our db (confidence), the game's, the index's"""
        if a in self.db:
            return '%s(%s)' % (self.db[a][0], CONF.get(self.db[a][1], '?'))
        if a in self.game:
            return self.game[a][0] + '[g]'
        if a in self.idxname:
            return self.idxname[a] + '[i]'
        return None

    def unit_of(self, a):
        r = self.units.execute('select unit_id from units where func=?', (a,)).fetchone()
        if r:
            return r[0]
        o = qnames.owner(self.ix, a)
        r = self.units.execute('select unit_id from units where func=?', (o,)).fetchone() if o else None
        return r[0] if r else None


def kind_of(text):
    m = re.match(r'[A-Za-z]+\d*', text or '')
    return m.group(0) if m else (text or '?')[:3]


def build(c, uid, max_kb=MAXKB, earlier=True):
    """-> (text, flags). flags: has_name, has_anchor, has_observed, size_full (bytes before the cap), dropped.
    earlier=False: the sheet a READER is given (q read pick, pick --reread, qlsheet_todo): no earlier model reading of the unit (the `read:`
    line with its confidence), no reader-proposed name `read NAME [r:..]` and no probe verdict tag. S111 FOLD: a reading shown them took part
    of its answer and its confidence from them (the same rule that removed the listing sheets' # NEIGHBOURS part); `q sheet` keeps them."""
    u = c.units.execute('select lo,hi,n_funcs from unit where unit_id=?', (uid,)).fetchone()
    if not u:
        raise SheetError('no unit %s (0..%d)' % (uid, c.n_units - 1))
    lo, hi, nf = u
    members = [r[0] for r in c.units.execute('select func from units where unit_id=? order by func', (uid,))]
    mset = set(members)
    size = {a: (s, i) for a, s, i in rng(c.ix, 'select addr,size,insns from funcs where addr between ? and ?', lo, hi)}
    hf, mids = c.hooks()
    # direct edges
    outs, ins = collections.defaultdict(set), collections.defaultdict(set)
    for s, d in rng(c.ix, "select src,dst from edges where src between ? and ? and kind in ('call','tail')", lo, hi):
        if s != d:
            outs[s].add(d)
    for s, d in rng(c.ix, "select src,dst from edges where dst between ? and ? and kind in ('call','tail')", lo, hi):
        if s != d:
            ins[d].add(s)
    strs = collections.defaultdict(list)
    for f, a, t in rng(c.ix, 'select r.func,r.addr,s.text from strrefs r left join strings s on s.addr=r.addr where r.func between ? and ?', lo, hi):
        strs[f].append((a, t))
    # vtable slots: from the table catalogue only (index.db `tables`: rtti / col / code = a vtable, run = a run of function words nothing loads)
    vslots = collections.defaultdict(list)     # func -> [vtADDR[+OFF] ...]  (a table code or RTTI knows)
    vrun = collections.defaultdict(list)       # func -> ['in run-table ...']
    vnone = set()                              # funcs a data word points at that no catalogued table holds
    has_cat = c.ix.execute("select 1 from sqlite_master where type='table' and name='tables'").fetchone() is not None
    if has_cat:
        for f, at in rng(c.ix, "select value,at from ptrs where value between ? and ? and kind='func'", lo, hi):
            if f not in mset:
                continue
            t = c.ix.execute('select start,rule,len from tables where start<=? order by start desc limit 1', (at,)).fetchone()
            if not t or at >= t[0] + 4 * t[2]:
                vnone.add(f)
            elif t[1] == 'run':
                vrun[f].append('in run-table %08X[+%d] (no code loads it: may not be a vtable)' % (t[0], at - t[0]))
            else:
                cls = c.ix.execute('select c.name from rtti_vtable v join rtti_class c on c.td=v.td where v.vt=?', (t[0],)).fetchone() if t[1] != 'code' else None
                vslots[f].append('vt%08X[+%d]%s' % (t[0], at - t[0], (' ' + cls[0]) if cls else ''))
    fld = collections.defaultdict(collections.Counter)
    for f, rw, off, root in rng(c.fd, 'select func,rw,off,root from rows where func between ? and ?', lo, hi):
        fld[f][(root_words(c.roots.get(root, '?')), rw, off)] += 1
    icalls = collections.defaultdict(list)
    for site, f, status, n in rng(c.vc, 'select site,func,status,n from icall where func between ? and ?', lo, hi):
        icalls[f].append((site, status, n))
    tg = collections.defaultdict(set)
    for site, t in rng(c.vc, 'select t.site,t.target from icall_target t join icall i on i.site=t.site where i.func between ? and ?', lo, hi):
        tg[site].add(t)
    seen_out = collections.defaultdict(list)             # func -> [(site, target, runs)]
    for site, f, t, r in rng(c.seen, 'select site,func,target,runs from seen where func between ? and ?', lo, hi):
        seen_out[f].append((site, t, r))
    seen_in = collections.defaultdict(list)              # target -> [(func, site, runs)]
    for site, f, t, r in rng(c.seen, 'select site,func,target,runs from seen where target between ? and ?', lo, hi):
        seen_in[t].append((f, site, r))
    mid_of = collections.defaultdict(list)
    end = hi + size.get(hi, (4, 0))[0]
    i = bisect.bisect_left(mids, (lo, ''))
    while i < len(mids) and mids[i][0] < end:
        o = members[max(0, bisect.bisect_right(members, mids[i][0]) - 1)]
        mid_of[o].append(mids[i][1])
        i += 1

    def named_list(xs, k):
        cnt = collections.Counter(c.label(x) for x in xs if c.label(x) and not NOISE.match(c.label(x)))
        nm = ['%s x%d' % (n, v) if v > 1 else n for n, v in sorted(cnt.items())]
        return nm[:k], len(nm) - min(k, len(nm)), len([x for x in xs if not c.label(x)])

    def edge_line(tag, others, k=5, inside_ok=True):
        inside = sorted(x for x in others if x in mset) if inside_ok else []
        if not inside_ok:
            others = [x for x in others if x not in mset]
            if not others:
                return None
        outside = [x for x in others if x not in mset]
        names, more, anon = named_list(outside, k)
        parts = []
        if inside:
            parts.append('unit %s' % lst(['%08X' % x for x in inside], 8))
        if outside:
            parts.append('outside %d: %s%s%s' % (len(outside), ' '.join(names) if names else 'none named',
                                                 (' +%d named' % more) if more else '', (' +%d unnamed' % anon) if anon and names else ''))
        return '%s %s' % (tag, '; '.join(parts))

    lines = []     # (kind, text)
    has_name = has_anchor = has_obs = False
    sysc = collections.Counter()
    gnames, dnames = [], []
    wide_n = collections.Counter()
    wide_seen = 0
    sites_total = sites_seen = 0
    n_vslot = 0
    conflicts = []
    fv = qreadverify.func_verdicts(lo, hi) if earlier else {}        # VERIFYMARK: [v:..], the verdict on the newest reading
    rd_names = qread.proposed_names(lo, hi) if earlier else {}      # reader candidates: tagged [r:..], never counted as a name
    for f in members:
        sz, ni = size.get(f, (0, 0))
        gn =c.game.get(f, [])
        nm_parts = []
        if f in c.db:
            nm_parts.append('db %s' % c.label(f))
            dnames.append(c.db[f][0])
            sysc[c.dbsys[f]] += 1
        if gn:
            nm_parts.append('game %s' % ' | '.join(gn[:3]))
            gnames += gn
        if f in c.idxname and f not in c.db and not gn:
            nm_parts.append('idx %s' % c.idxname[f])
        if f in c.db and gn:
            gw = set()
            for x in gn:
                gw |= name_words(x)
            if gw and name_words(c.db[f][0]) and not (gw & name_words(c.db[f][0])):
                conflicts.append('CONFLICT %08X: db name %s and game name %s share no word of 4+ letters (one of them is wrong)' % (f, c.db[f][0], ' | '.join(gn[:3])))
        if nm_parts:
            has_name = True
        if f in rd_names:
            nm_parts.append('read %s [r:%s]%s' % (rd_names[f] + (qreadverify.tag(fv.get(f)),)))
        ptag = qpattern.tag(c.pat, f)            # S110 PATTERN: a shape label, after the names; never a name
        if f in c.prel and c.db.get(f, ('', ''))[1] != 'high':      # S110 RULES2: a clone relation, only where no HIGH db name stands
            ptag = (ptag + ' ' + qpattern_rules2.rel_tag(c.prel, f)).strip()
        hdr = '%08X %dB %di %s' % (f, sz, ni, ' ; '.join(nm_parts) + ((' ' if nm_parts else '') + ptag if ptag else ''))
        if f not in rd_names and f in fv:        # a probe's verdict on the function itself (no read name to hang it on)
            hdr = hdr.rstrip() + qreadverify.tag(fv[f])
        lines.append(('hdr', hdr))
        anc = []
        if f in hf:
            anc.append('hook ' + ','.join(hf[f][:2]))
        if f in mid_of:
            anc.append('mid-asm ' + ','.join(mid_of[f][:2]))
        if f in vslots:
            n_vslot += 1
        sig = False
        if f in strs:
            ss = []
            for a, t in sorted(set(strs[f])):
                if t:
                    ss.append('"%s"' % clip_str(t))
                    sig = sig or is_signature(t)
            if ss:
                lines.append(('strsig' if sig else 'str', ' str %d: %s' % (len(set(strs[f])), lst(ss, 3))))
        if anc or f in strs or f in vslots or f in vrun:
            has_anchor = True
        if anc:
            lines.append(('anc', ' anc ' + ' ; '.join(anc)))
        if f in vslots:
            lines.append(('vslot', ' vslot ' + lst(sorted(set(vslots[f])), 3)))
        for v in sorted(set(vrun.get(f, []))):
            lines.append(('vslot', ' ' + v))
        if f in vnone and f not in vslots and f not in vrun:
            lines.append(('vslot', ' vslot: none known'))
        if ins.get(f):
            el = edge_line('in', ins[f], inside_ok=sz > 16)
            if el:
                lines.append(('in', ' ' + el))
        if outs.get(f):
            o = set(x for x in outs[f] if not (c.label(x) and NOISE.match(c.label(x))))
            if o:
                lines.append(('out', ' ' + edge_line('out', o)))
        # indirect: static
        ic = icalls.get(f, [])
        if ic:
            cnt = collections.Counter(s for _, s, _ in ic)
            seen_sites = set(s for s, _, _ in seen_out.get(f, []) if s)
            ex, nar = set(), set()
            for site, status, _ in ic:
                if status == 'exact':
                    ex |= tg.get(site, set())
                elif status == 'narrow':
                    nar |= tg.get(site, set())
                else:
                    wide_n[status] += 1
                    wide_seen += site in seen_sites
                sites_total += 1
                sites_seen += site in seen_sites
            txt = ' ind %d sites %s' % (len(ic), ' '.join('%s%d' % (k, v) for k, v in sorted(cnt.items())))
            if ex:
                txt += ' ; exact->' + lst([c.label(x) or '%08X' % x for x in sorted(ex)], 4)
            if nar:
                txt += ' ; narrow->%d tgts ' % len(nar) + lst([c.label(x) or '%08X' % x for x in sorted(nar)], 3)
            txt += ' ; run %d/%d' % (len(seen_sites & set(s for s, _, _ in ic)), len(ic))
            lines.append(('ind', txt))
        # indirect: observed
        so, si = seen_out.get(f, []), seen_in.get(f, [])
        if so or si:
            has_obs = True
            parts = []
            if si:
                cl = sorted(set(x[0] for x in si))
                nmd = sorted(set(c.label(x) for x in cl if c.label(x)))
                parts.append('callers %d (%d funcs, <=%d of %d runs) %s' % (len(si), len(cl), max(x[2] for x in si), c.nruns, lst(nmd, 4) if nmd else ''.join('')))
            if so:
                tl = sorted(set(t for _, t, _ in so))
                nmd = sorted(set(c.label(x) for x in tl if c.label(x)))
                parts.append('targets %d %s' % (len(tl), lst(nmd, 4) if nmd else ''))
            lines.append(('obs', ' obs ' + ' ; '.join(p.strip() for p in parts)))
        # fields
        if fld.get(f):
            by = collections.defaultdict(collections.Counter)
            for (k, rw, off), n in fld[f].items():
                by[k][(rw, off)] += n
            ks = sorted(by, key=lambda k: -sum(by[k].values()))
            fs = []
            for k in ks[:5]:
                seg = []
                const = k == 'absolute address'          # a constant-address row: the offset IS the address
                for rw, verb in (('W', 'writes to address' if const else 'writes'), ('R', 'reads address' if const else 'reads'),
                                 ('A', 'takes address of address' if const else 'takes address of')):
                    top = sorted(((n, off) for (r, off), n in by[k].items() if r == rw), key=lambda x: (-x[0], x[1]))[:3]
                    if top:
                        seg.append('%s %s' % (verb, ' '.join(('0x%X' % (off & 0xFFFFFFFF) if const else '+%d' % off) + (('*%d' % n) if n > 1 else '') for n, off in top)))
                fs.append('%s: %s' % (k, '; '.join(seg)))
            lines.append(('fld', ' fld ' + ' | '.join(fs) + (' (+%d more bases)' % (len(ks) - 5) if len(ks) > 5 else '')))
    # header
    allg = sorted(set(gnames))
    head = ['# unit %d  %08X..%08X  %d funcs  %d with a name' % (uid, lo, hi, nf, sum(1 for f in members if f in c.db or f in c.game)),
            'game names: ' + (lst(allg, 8) if allg else 'none'),
            'db names: ' + (lst(sorted(set(dnames)), 8) if dnames else 'none') +
            ('  systems: ' + ' '.join('%s=%d' % kv for kv in sysc.most_common(6)) if sysc else '')] + conflicts
    rl = qread.read_line(uid) if earlier else None
    if rl:
        head.append(rl + qreadverify.tag(qreadverify.unit_verdict(uid)))
    # footer: only the counts that are not zero
    foot = []
    wsum = sum(wide_n.values())
    unobs = sites_total - sites_seen
    bits = []
    if wsum:
        bits.append('%d wide indirect sites (%s), %d of them run in the observed runs' % (wsum, ' '.join('%s %d' % kv for kv in sorted(wide_n.items())), wide_seen))
    if unobs:
        bits.append('%d of %d static indirect sites unobserved (unobserved is not "never")' % (unobs, sites_total))
    if n_vslot:
        bits.append('%d vtable-slot funcs have callers through the slot' % n_vslot)
    if not has_cat:
        bits.append('no table catalogue in this index: no vslot lines')
    if bits:
        foot.append('cannot see: ' + '; '.join(bits) + '; direct edges only join functions of the index.')
    fixed = '\n'.join(head) + '\n' + ('\n'.join(foot) + '\n' if foot else '')
    cap = int(max_kb * 1024) - 160            # room for the DROPPED line
    body = list(lines)
    total = len(fixed) + sum(len(t) + 1 for _, t in body)
    full = total
    dropped = collections.Counter()
    if total > cap:
        order = sorted((i for i, (k, t) in enumerate(body) if k != 'hdr'), key=lambda i: (PRIO[body[i][0]], -len(body[i][1])))
        gone = set()
        for i in order:
            if total <= cap:
                break
            gone.add(i)
            dropped[body[i][0]] += 1
            total -= len(body[i][1]) + 1
        body = [b for i, b in enumerate(body) if i not in gone]
    out = '\n'.join(head) + '\n' + '\n'.join(t for _, t in body) + '\n'
    if dropped:
        out += 'DROPPED %d lines for --max-kb %g (least informative first): %s\n' % (sum(dropped.values()), max_kb, ' '.join('%s %d' % kv for kv in sorted(dropped.items())))
    out += ''.join(x + '\n' for x in foot)
    flags = {'has_name': has_name, 'has_anchor': has_anchor or bool(hf and any(f in hf for f in members)), 'has_observed': has_obs,
             'bytes': len(out.encode('utf-8')), 'full_bytes': full, 'dropped': sum(dropped.values()), 'n_funcs': nf}
    return out, flags


def sheet(uid, max_kb=MAXKB, ctx=None, earlier=True):
    return build(ctx or Ctx(), uid, max_kb, earlier)[0]


def stats(c, max_kb=MAXKB, outdir=None):
    t0 = time.time()
    uids = [r[0] for r in c.units.execute('select unit_id from unit where n_funcs>=3 order by unit_id')]
    c.hooks()
    sizes, full, n_name, n_anc, n_obs, n_drop, lines_dropped = [], [], 0, 0, 0, 0, 0
    if outdir:
        os.makedirs(outdir, exist_ok=True)
    for u in uids:
        t, fl = build(c, u, max_kb)
        sizes.append(fl['bytes'])
        full.append(fl['full_bytes'])
        n_name += fl['has_name']
        n_anc += fl['has_anchor']
        n_obs += fl['has_observed']
        n_drop += fl['dropped'] > 0
        lines_dropped += fl['dropped']
        if outdir:
            with open(os.path.join(outdir, 'sheet_%d.txt' % u), 'w', encoding='utf-8') as fh:
                fh.write(t)
    s, fs = sorted(sizes), sorted(full)
    n = len(s)

    def q(a, p):
        return a[min(n - 1, n * p // 100)] if n else 0
    return {'units': n, 'max_kb': max_kb, 'median_bytes': q(s, 50), 'p90_bytes': q(s, 90), 'max_bytes': s[-1] if n else 0, 'total_mb': round(sum(s) / 1048576.0, 3),
            'uncapped_median_bytes': q(fs, 50), 'uncapped_p90_bytes': q(fs, 90), 'uncapped_max_bytes': fs[-1] if n else 0, 'uncapped_total_mb': round(sum(fs) / 1048576.0, 3),
            'with_name': n_name, 'with_anchor': n_anc, 'with_observed_edge': n_obs, 'sheets_capped': n_drop, 'lines_dropped': lines_dropped,
            'seconds': round(time.time() - t0, 1)}


def main(argv):
    max_kb = float(argv[argv.index('--max-kb') + 1]) if '--max-kb' in argv else MAXKB
    outdir = argv[argv.index('--out') + 1] if '--out' in argv else None
    skip = set()
    for flag in ('--max-kb', '--out'):
        if flag in argv:
            skip.add(argv.index(flag) + 1)
    pos = [x for i, x in enumerate(argv) if not x.startswith('--') and i not in skip]
    if '--stats' in argv:
        r = stats(Ctx(), max_kb, outdir)
        if '--json' in argv:
            print(json.dumps(r, indent=1, sort_keys=True))
        else:
            print('%d sheets (units of >= 3 funcs), cap %g KB: size median %d B, p90 %d B, max %d B, total %.2f MB' %
                  (r['units'], max_kb, r['median_bytes'], r['p90_bytes'], r['max_bytes'], r['total_mb']))
            print('uncapped: median %d B, p90 %d B, max %d B, total %.2f MB; %d sheets capped, %d lines dropped' %
                  (r['uncapped_median_bytes'], r['uncapped_p90_bytes'], r['uncapped_max_bytes'], r['uncapped_total_mb'], r['sheets_capped'], r['lines_dropped']))
            print('with >= 1 name: %d, anchor: %d, observed edge: %d  (%.1f s)' % (r['with_name'], r['with_anchor'], r['with_observed_edge'], r['seconds']))
        return 0
    if not pos:
        print(__doc__)
        return 2
    c = Ctx()
    a = qnames.parse_addr(pos[0])
    if a is not None and not re.fullmatch(r'\d{1,5}', pos[0]):
        uid = c.unit_of(a)
        if uid is None:
            raise SheetError('%s is not inside a function of the index' % pos[0])
    elif re.fullmatch(r'\d+', pos[0]):
        uid = int(pos[0])
    else:
        raise SheetError('q sheet takes a unit id (decimal) or a function address, not %r' % pos[0])
    sys.stdout.write(build(c, uid, max_kb)[0])
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except (SheetError, qnames.NamesError, qseen.SeenError) as e:
        print('q: %s' % e, file=sys.stderr)
        sys.exit(2)
