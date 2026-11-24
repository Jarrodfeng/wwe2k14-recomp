r"""q pattern: MECHANICAL shape labels for functions (S110 lane PATTERN). A label is a KIND ("deleting_dtor", "this_adjust_thunk", ...),
computed from facts the index already holds, at zero model tokens. It is never a name: it is not written to db\*.json, is never a [g]
name and never a `q cover` anchor. q sheet prints it after the names of a function line as ` [p:kind]`.

    py tools/q/q.py pattern FUNC            the labels (kind + evidence) of one function, from pattern.s1.db
    py tools/q/q.py pattern --stats         per kind: count + 5 sample functions (+ the helper names that did not resolve)
    py tools/q/q.py pattern --build         compute every label and write out/harness/index/pattern.s1.db (table pattern_fn)
    py tools/q/q.py pattern --precision     agreement of the labels with HIGH / MEDIUM db names containing dtor / thunk / ctor / getter

Facts used: index.db (size, direct call/tail edges, vtable slots from ptrs + tables), units.s1.db (the unit of a function),
fields.s2.db (field reads / writes / address-takes on this = root a0, and reads of a call result = root r:SITE), seen.s1.db
(OBSERVED callers), names.s2.db + db/*.json (names). The helper functions a rule names (cymem_free_tagged_wrapper,
component_find_by_tag, ...) are resolved BY NAME through db/*.json, names.s2.db and the index; a name that does not resolve makes
the rules that need it report 0 (the stats line says which). Every rule is a SHAPE: a label says "looks like", not "is".
MAY BE INCOMPLETE: a vtable slot or an observed caller the index does not hold is a label that is not given (not "never").
"""
import bisect
import collections
import glob
import json
import os
import re
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qnames  # noqa: E402

SRC = os.path.dirname(os.path.dirname(HERE))
PATTERN_DB = os.path.join(qnames.IDX, 'pattern.s1.db')
RANK = {'high': 3, 'medium': 2, 'low': 1, 'unknown': 0}
KINDS = ('deleting_dtor', 'this_adjust_thunk', 'forward_thunk', 'component_getter', 'sequence_tick', 'render_node_draw',
         'thread_entry', 'read_end_process', 'component_ctor', 'field_accessor')
HELPERS = {                      # rule helper name -> alias used by the rules
    'free': ('cymem_free_tagged_wrapper',),
    'find': ('component_find_by_tag', 'component_find_by_tag_with_cache_at_12'),
    'basector': ('component_base_ctor',),
    'tick': ('sequence_owner_tick_sequence_hooks',),
    'render': ('render_category_walk_enabled_nodes',),
    'thread': ('guest_thread_start_trampoline',),
    'readend': ('nonblock_reader_read_end_trampoline',),
}
READEND_NAME = re.compile(r'::ReadEndProcess\b')


def ro(path):
    path = path and qnames.newest(path)      # S110 QREBUILD review: a copy waiting beside a held file is the one to read
    if not path or not os.path.exists(path):
        raise qnames.NamesError('no file %s' % path)
    return sqlite3.connect('file:%s?mode=ro' % path.replace('\\', '/'), uri=True)


def paths(idx=None, dbdir=None):
    idx = idx or qnames.IDX
    return {'index': os.path.join(idx, 'index.db'), 'units': os.path.join(idx, 'units.s1.db'), 'fields': os.path.join(idx, 'fields.s2.db'),
            'seen': os.path.join(idx, 'seen.s1.db'), 'names': os.path.join(idx, 'names.s2.db'), 'dbdir': dbdir or os.path.join(SRC, 'db'),
            'out': os.path.join(idx, 'pattern.s1.db')}


class Facts:
    """every fact a rule reads, loaded once (production loader: load())"""

    def __init__(self):
        self.size = {}
        self.outs = collections.defaultdict(set)         # func -> {callee} (call + tail edges, not self)
        self.unit = {}
        self.vslot = collections.defaultdict(list)       # func -> [(vt, off)]
        self.this = collections.defaultdict(list)        # func -> [(rw, off)] on this (root a0)
        self.res = collections.defaultdict(list)         # func -> [(call site, rw, off)] on a call result (root r:SITE, the bl address)
        self.nrows = collections.Counter()               # func -> number of field rows of any root
        self.callers = collections.defaultdict(set)      # func -> {observed caller func}, only for callers that are helpers
        self.names = collections.defaultdict(list)       # func -> [(name, source)]  source: db:high | game | idx
        self.helper = {}                                 # alias -> set(addr)
        self.unresolved = []                             # [(alias, [names])]


def load(p=None):
    p = p or paths()
    f = Facts()
    ix = ro(p['index'])
    for a, s in ix.execute('select addr,size from funcs where has_body=1'):
        f.size[a] = s
    idxname = {}
    for a, n in ix.execute("select addr,name from funcs where name is not null and name not like 'sub\\_%' escape '\\'"):
        idxname[a] = n
        f.names[a].append((n, 'idx'))
    for s, d in ix.execute("select src,dst from edges where kind in ('call','tail')"):
        if s != d:
            f.outs[s].add(d)
    # vtable slots: the table catalogue (rtti / col / code rows), not the 'run' tables that nothing loads
    tabs = sorted((s, r, n) for s, r, n in ix.execute('select start,rule,len from tables'))
    starts = [t[0] for t in tabs]
    for fn, at in ix.execute("select value,at from ptrs where kind='func'"):
        i = bisect.bisect_right(starts, at) - 1
        if i < 0:
            continue
        st, rule, n = tabs[i]
        if at >= st + 4 * n or rule == 'run':
            continue
        f.vslot[fn].append((st, at - st))
    un = ro(p['units'])
    f.unit = dict(un.execute('select func,unit_id from units'))
    nm = ro(p['names'])
    for a, n, s in nm.execute('select func,name,source from names where source!="fn_string_shared"'):
        f.names[a].append((n, 'game'))
    dbn = collections.defaultdict(list)                  # name -> [addr] (our db/*.json)
    best = {}
    for fp in glob.glob(os.path.join(p['dbdir'], '*.json')):
        try:
            ents = json.load(open(fp, encoding='utf-8'))
        except (ValueError, OSError):
            continue
        for e in ents:
            try:
                a = int(e['address'], 16)
            except (KeyError, ValueError, TypeError):
                continue
            c = e.get('confidence', 'unknown')
            if a not in best or RANK.get(c, 0) > RANK.get(best[a][1], 0):
                best[a] = (e.get('name') or '?', c)
    for a, (n, c) in best.items():
        f.names[a].append((n, 'db:' + c))
        dbn[n].append(a)
    byname = collections.defaultdict(set)
    for a, lst in f.names.items():
        for n, _s in lst:
            byname[n].add(a)
    for alias, ns in HELPERS.items():
        got = set()
        for n in ns:
            got |= byname.get(n, set())
        f.helper[alias] = got
        if not got:
            f.unresolved.append((alias, list(ns)))
    fd = ro(p['fields'])
    roots = dict(fd.execute('select id,text from roots'))
    for fn, rw, off, root in fd.execute('select func,rw,off,root from rows'):
        f.nrows[fn] += 1
        t = roots.get(root, '')
        if t == 'a0':
            f.this[fn].append((rw, off))
        elif t.startswith('r:'):
            try:
                f.res[fn].append((int(t[2:], 16), rw, off))
            except ValueError:
                pass
    watch = set()
    for k in ('tick', 'render', 'thread', 'readend'):
        watch |= f.helper[k]
    if watch:
        sn = ro(p['seen'])
        for fn, tg in sn.execute('select func,target from seen'):
            if fn in watch:
                f.callers[tg].add(fn)
    return f


def vs_text(f, a):
    return ','.join('vt%08X+%d' % x for x in sorted(set(f.vslot.get(a, [])))[:3])


def classify(f):
    """-> {func: [(kind, evidence)]}; every rule is one block below, each states the facts it reads."""
    out = collections.defaultdict(list)
    free, find, basector = f.helper['free'], f.helper['find'], f.helper['basector']
    for a, sz in f.size.items():
        outs = f.outs.get(a, set())
        offs = sorted(set(o for _, o in f.vslot.get(a, [])))
        this = f.this.get(a, [])
        # deleting_dtor: 80..140 B at vslot +0 / +12; calls cymem_free_tagged_wrapper and exactly one other function, which sits in its unit
        if free and 80 <= sz <= 140 and any(o in (0, 12) for o in offs) and len(outs) == 2 and len(outs & free) == 1:
            other = next(iter(outs - free))
            # the body may sit in another unit (a base class's dtor): the first build required its own unit and missed 59 % of
            # the db's known deleting dtors (S110 hand-back); the slot, the size and "one body + free" are the shape
            out[a].append(('deleting_dtor', '%dB slot %s calls %08X%s + free' % (sz, vs_text(f, a), other,
                                                                            ' (unit)' if f.unit.get(other) is not None and f.unit.get(other) == f.unit.get(a) else '')))
        # this_adjust_thunk: 8 B at a vslot, takes the address of this+-N, one callee, no other field fact
        if sz == 8 and offs and len(outs) == 1 and f.nrows.get(a, 0) == len(this) and this and all(rw == 'A' and o != 0 for rw, o in this):
            out[a].append(('this_adjust_thunk', '8B slot %s this%+d -> %08X' % (vs_text(f, a), this[0][1], next(iter(outs)))))
        # forward_thunk: 4..16 B, one callee, no field facts at all
        elif 4 <= sz <= 16 and len(outs) == 1 and not f.nrows.get(a, 0):
            out[a].append(('forward_thunk', '%dB -> %08X' % (sz, next(iter(outs)))))
        # component_getter: ~80 B, the only callee is component_find_by_tag (or the cached variant), reads +24 of its result
        if find and 72 <= sz <= 96 and len(outs) == 1 and outs <= find and any(rw == 'R' and o == 24 for _site, rw, o in f.res.get(a, [])):
            out[a].append(('component_getter', '%dB find %08X, reads result+24' % (sz, next(iter(outs)))))
        # observed callers: sequence tick, render node draw, thread entry, read-end hook
        cl = f.callers.get(a, set())
        for kind, alias in (('sequence_tick', 'tick'), ('render_node_draw', 'render'), ('thread_entry', 'thread')):
            if cl & f.helper[alias]:
                out[a].append((kind, 'observed caller %s%s' % (HELPERS[alias][0], (' slot ' + vs_text(f, a)) if a in f.vslot else '')))
        rn = [n for n, s in f.names.get(a, []) if s == 'game' and READEND_NAME.search(n)]
        if rn:
            out[a].append(('read_end_process', 'game name %s' % rn[0][-60:]))
        elif cl & f.helper['readend']:
            out[a].append(('read_end_process', 'observed caller %s' % HELPERS['readend'][0]))
        # component_ctor: calls component_base_ctor and writes this+0
        if basector and outs & basector and any(rw == 'W' and o == 0 for rw, o in this):
            out[a].append(('component_ctor', 'calls component_base_ctor, writes this+0 (%dB)' % sz))
        # field_accessor: 8 B, no call, one this+N word read (getter) or written (setter), nothing else
        if sz == 8 and not outs and len(this) == 1 and f.nrows.get(a, 0) == 1 and this[0][0] in ('R', 'W'):
            out[a].append(('field_accessor', '8B %s this+%d' % ('get' if this[0][0] == 'R' else 'set', this[0][1])))
    return out


def write(out, f, p, path=None):
    path = path or p['out']
    tmp = path + '.tmp%d' % os.getpid()
    if os.path.exists(tmp):
        os.remove(tmp)
    w = sqlite3.connect(tmp)
    w.executescript('create table pattern_fn(func integer, kind text, evidence text); create index pf_func on pattern_fn(func);'
                    'create index pf_kind on pattern_fn(kind); create table meta(key text primary key, value text);')
    w.executemany('insert into pattern_fn values (?,?,?)', [(a, k, e) for a in sorted(out) for k, e in out[a]])
    w.executemany('insert into meta values (?,?)', [('unresolved', json.dumps(f.unresolved)), ('kinds', ','.join(KINDS))])
    w.commit()
    w.close()
    os.replace(tmp, path)


def build(p=None, verbose=True):
    p = p or paths()
    f = load(p)
    out = classify(f)
    write(out, f, p)
    if verbose:
        print('pattern.s1.db: %d labels on %d functions -> %s' % (sum(len(v) for v in out.values()), len(out), p['out']))
        for alias, ns in f.unresolved:
            print('  helper %s did not resolve (%s): the rules that need it report 0' % (alias, ' | '.join(ns)))
    return out


def labels(path=None):
    """{func: [kind, ...]} from pattern.s1.db; {} when there is none (q sheet then prints no tag)"""
    path = path or PATTERN_DB
    if not os.path.exists(path):
        return {}
    c = ro(path)
    try:
        d = collections.defaultdict(list)
        for a, k in c.execute('select func,kind from pattern_fn order by func,kind'):
            d[a].append(k)
        return dict(d)
    except sqlite3.Error:
        return {}
    finally:
        c.close()


def tag(lab, a):
    """the sheet tag of a function: ` [p:kind,kind]` or ''. It goes AFTER the names field of the function line."""
    ks = lab.get(a)
    return '[p:%s]' % ','.join(ks) if ks else ''


def stats(path=None):
    c = ro(path or PATTERN_DB)
    rows = collections.defaultdict(list)
    for a, k in c.execute('select func,kind from pattern_fn order by func'):
        rows[k].append(a)
    meta = dict(c.execute('select key,value from meta'))
    lines = ['%-18s %7s  sample' % ('kind', 'count')]
    for k in KINDS:
        v = rows.get(k, [])
        lines.append('%-18s %7d  %s' % (k, len(v), ' '.join('%08X' % a for a in v[:5]) or '-'))
    un = json.loads(meta.get('unresolved', '[]'))
    for alias, ns in un:
        lines.append('helper %s did not resolve (%s): the rules that need it report 0' % (alias, ' | '.join(ns)))
    return '\n'.join(lines)


EXPECT = (('dtor', ('deleting_dtor',)), ('thunk', ('this_adjust_thunk', 'forward_thunk')), ('ctor', ('component_ctor',)),
          ('getter', ('component_getter', 'field_accessor')))


def precision(p=None, show=10):
    """HIGH / MEDIUM db names containing dtor / thunk / ctor / getter against the labels. -> text"""
    p = p or paths()
    lab = labels(p['out'])
    best = {}
    for fp in glob.glob(os.path.join(p['dbdir'], '*.json')):
        try:
            ents = json.load(open(fp, encoding='utf-8'))
        except (ValueError, OSError):
            continue
        for e in ents:
            try:
                a = int(e['address'], 16)
            except (KeyError, ValueError, TypeError):
                continue
            c = e.get('confidence', 'unknown')
            if c in ('high', 'medium') and (a not in best or RANK[c] > RANK[best[a][1]]):
                best[a] = (e.get('name') or '?', c)
    lines, dis = [], []
    for word, kinds in EXPECT:
        # the word as a token: 'destructor' / 'constructor' count as dtor / ctor; 'factory' and 'vector' (which hold 'ctor') do not
        names = [(a, n) for a, (n, _c) in sorted(best.items())
                 if re.search(r'(?<![a-z])%s(?![a-z])' % word, n.lower().replace('destructor', 'dtor').replace('constructor', 'ctor'))]
        agree = [a for a, _n in names if set(lab.get(a, [])) & set(kinds)]
        anyl = [a for a, _n in names if lab.get(a)]
        lines.append('name has %-6s %5d functions: label agrees (%s) %5d (%.1f%%), any label %5d (%.1f%%)' %
                     (word, len(names), '|'.join(kinds), len(agree), 100.0 * len(agree) / max(1, len(names)), len(anyl),
                      100.0 * len(anyl) / max(1, len(names))))
        for a, n in names:
            if a not in set(agree):
                dis.append((word, a, n, lab.get(a, [])))
    # the other direction: a labelled function whose HIGH/MEDIUM name is about something else
    lines.append('disagreements: %d; first %d spread over the words:' % (len(dis), show))
    byw = collections.defaultdict(list)
    for d in dis:
        byw[d[0]].append(d)
    shown, i = [], 0
    while len(shown) < min(show, len(dis)):
        for w in [w for w, _ in EXPECT]:
            if i < len(byw[w]) and len(shown) < show:
                shown.append(byw[w][i])
        i += 1
    for w, a, n, lb in shown:
        lines.append('  %-6s %08X %-50s label: %s' % (w, a, n[:50], ','.join(lb) or 'none'))
    return '\n'.join(lines)


def show_one(q, path=None):
    a = qnames.parse_addr(q)
    if a is None:
        raise qnames.NamesError('q pattern takes a function address (0x826D44A0, sub_826D44A0 or 8 hex digits)')
    if not os.path.exists(path or PATTERN_DB):
        raise qnames.NamesError('no pattern db: run py tools\\q\\q.py pattern --build')
    c = ro(path or PATTERN_DB)
    rows = c.execute('select kind,evidence from pattern_fn where func=? order by kind', (a,)).fetchall()
    if not rows:
        return 'sub_%08X: no pattern label (a shape rule did not match; says nothing else about the function)' % a
    return '\n'.join('sub_%08X [p:%s] %s' % (a, k, e) for k, e in rows)


def main(argv):
    if '--build' in argv:
        build()
        return 0
    if '--stats' in argv:
        print(stats())
        return 0
    if '--precision' in argv:
        print(precision())
        return 0
    pos = [x for x in argv if not x.startswith('--')]
    if not pos:
        print(__doc__)
        return 2
    print(show_one(pos[0]))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except qnames.NamesError as e:
        print('q: %s' % e, file=sys.stderr)
        sys.exit(2)
