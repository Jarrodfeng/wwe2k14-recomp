r"""S110 lane FEEDKEY: a STRUCT KEY and the game's WRITERS for every field row of the scene feed's table (tools/q/qfeed.py).

    py -I tools\q\q.py feed                  the summary: per family reads / key resolved / writer known / writer unknown
    py -I tools\q\q.py feed FILE[:LINE]      that file's rows with struct key, route, evidence and writers
    py -I tools\q\q.py feed --unread         per resolved struct: the offsets the GAME writes that our code never reads
    (every form also writes out\feed\feed_keyed.tsv)

A row's base is often only a local name (`obj`, `mgr`), so `q field` could be asked by bare offset only. The key of a base is one
of three claims, tried in this order; the route and its evidence are printed:
  1 hookarg  every assignment of the local (within the function) is `ctx.rN` of a REX_HOOK entry, or the base is ctx.rN itself:
             the index's objtype on (hooked function, aN) must name exactly ONE vtable (or, with no type, the callers' argflow exactly
             ONE global): writers = the rows typed by that vtable / rooted at that global.
  2 global   every assignment is the pointer loaded from ONE global word (`mem.u32(kGlobal)`, `ReadWord(read, kGlobal, v)`): root g:G;
             the index's types of g:G add the constructors that store its vptr.
  3 ctor     the base's assignment line (or the line above) names `constructor sub_X` / `ctor sub_X` and the index knows sub_X as a
             constructor: the vtable it stores.
  K          an absolute address (the table's own K / image status).
Anything else is `unresolved` with the reason: assigned from two different sources, a parameter of a helper (callers not followed), a
hook argument the index has no single type for, a member expression, a mid-asm hook. A key is a claim: a route that would guess
gives `unresolved`.
What this cannot see: bulk copies (hull / words(A, n) / copy_deref / host_ptr) give a span, not fields (counted apart); forwarded and
register rows have no fixed address; an indexed row (+idx) has a base offset, not the element's, so it is never counted as written;
the index does not model indexed accesses (lwzx) nor types passed down to callees.
Rule 1: offsets, addresses and our own file:line only.
"""
import argparse
import os
import re
import sqlite3
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qfeed   # noqa: E402

DRAWN = ['model', 'material', 'particle', 'light', 'arena', 'crowd', 'camera', 'ui']
OUT_TSV = os.path.join(qfeed.ROOT, 'out', 'feed', 'feed_keyed.tsv')
TSV_COLS = ['file', 'line', 'func', 'reader', 'kind', 'addr', 'family', 'base', 'off', 'width', 'old_status', 'route', 'key', 'key_name',
            'evidence', 'writers_n', 'top_writers', 'result', 'reason']
GLOAD = re.compile(r'^(?:\(\s*u?int32_t\s*\)\s*)?[\w:\.>\-]*?(?:u32|Ld32|Rd32|ReadU32|GuestU32|LoadU32|Read32)\s*\((.*)\)$')
CTOR_CMT = re.compile(r'\b(?:constructor|ctor)\s+(sub_[0-9A-Fa-f]{8})')
HOOKARG = re.compile(r'^\(*ctx\.r(\d+)\.u(?:32|64)\)*$')
OUTPARAM_FNS = {'ReadWord': 1, 'ReadWordBE': 1, 'Word': 1}   # name -> address arg index; the destination is the LAST argument
TYPE_NAMES = r'(?:u?int(?:8|16|32|64)_t|auto|size_t|u32|uint)'


def hx(a):
    return '0x%08X' % a


class Index:
    """the read-only questions on the field index (rows / roots / objtype / ctors / argflow)"""

    def __init__(self, con, names=None, rtti=None):
        self.con, self.names, self.rtti, self.cache = con, names or {}, rtti or {}, {}
        self.root_id = {t: i for i, t in con.execute('select id, text from roots where text in (?,?,?,?,?,?,?,?)',
                                                        tuple('a%d' % k for k in range(8)))}
        self.has_flow = bool(con.execute("select 1 from sqlite_master where name='argflow'").fetchone())

    def fname(self, f):
        n = self.names.get(f)
        return 'sub_%08X' % f + (' (%s)' % n if n else '')

    def vname(self, vt):
        return self.rtti.get(vt, '')

    def arg_types(self, func, argi):
        """-> ([(vt, how)], [global root texts]) of a hook's argument aN"""
        rid = self.root_id.get('a%d' % argi)
        if rid is None:
            return [], []
        vts = list(self.con.execute('select distinct vt, how from objtype where root=? and (func=? or func=0)', (rid, func)))
        gl = []
        if self.has_flow:
            gl = [g for (g,) in self.con.execute('select distinct gr.text from argflow af join roots gr on gr.id=af.groot '
                                                  'where af.func=? and af.argroot=? and gr.text like "g:%"', (func, rid))]
        return vts, gl

    def global_types(self, gtext):
        """vtables the index typed the pointer at global gtext with (objtype rows of root g:G, func 0)"""
        r = self.con.execute('select id from roots where text=?', (gtext,)).fetchone()
        if not r:
            return []
        return [v for (v,) in self.con.execute('select distinct vt from objtype where root=?', (r[0],))]

    def has_root(self, text):
        return bool(self.con.execute('select 1 from roots where text=?', (text,)).fetchone())

    def ctor_vt(self, func):
        r = self.con.execute('select vt from ctors where func=?', (func,)).fetchone()
        return r[0] if r else None

    def ctors_of(self, vt):
        return [f for (f,) in self.con.execute('select func from ctors where vt=? limit 3', (vt,))]

    def _cond(self, handle):
        kind, v = handle
        if kind == 'vt':
            return ('exists (select 1 from objtype o where o.vt in (%s) and o.root=r.root and (o.func=r.func or o.func=0))'
                    % ','.join(str(x) for x in v), [], '')
        flow = (' or exists (select 1 from argflow af join roots gr on gr.id=af.groot where af.func=r.func and af.argroot=r.root and gr.text=?)'
                if self.has_flow else '')
        return ('(ro.text=?' + flow + ')', [v] + ([v] if self.has_flow else []), 'join roots ro on ro.id=r.root')

    def writers(self, handles, off):
        """{function: write rows} of +off over every handle of a key"""
        out = defaultdict(int)
        for h in handles:
            k = ('w', h, off)
            if k not in self.cache:
                cond, args, join = self._cond(h)
                self.cache[k] = self.con.execute('select r.func, count(*) from rows r %s where r.off=? and r.rw=\'W\' and %s group by r.func'
                                                 % (join, cond), [off] + args).fetchall()
            for f, n in self.cache[k]:
                out[f] += n
        return out

    def written(self, handles):
        """{offset: (widest width, distinct writer functions)} over every handle"""
        out = {}
        for h in handles:
            cond, args, join = self._cond(h)
            for off, w, nf in self.con.execute('select r.off, max(r.width), count(distinct r.func) from rows r %s where r.rw=\'W\' and %s group by r.off'
                                               % (join, cond), args):
                o = out.get(off, (0, 0))
                out[off] = (max(o[0], w), max(o[1], nf))
        return out


# ---------------------------------------------------------------------------------------------- the source side
def unit_span(code, i):
    """-> (start, end, hook address or None, params): the top-level function (or hook) holding line i"""
    start = 0
    for j in range(i, -1, -1):
        if qfeed.HOOK_RE.search(code[j]):
            start = j
            break
        if code[j][:1] and not code[j][:1].isspace() and qfeed.DEF_RE.match(code[j]) and \
                qfeed.DEF_RE.match(code[j]).group(1) not in ('if', 'for', 'while', 'switch', 'return', 'catch'):
            start = j
            break
    hm = qfeed.HOOK_RE.search(code[start])
    hook = int(hm.group(1)[4:], 16) if hm else None
    dm = None if hm else qfeed.DEF_RE.match(code[start])
    params = qfeed.param_names(dm.group(2)) if dm else set()
    end = len(code)
    for j in range(i + 1, min(len(code), i + 600)):
        if code[j][:1] and not code[j][:1].isspace() and (qfeed.HOOK_RE.search(code[j]) or qfeed.DEF_RE.match(code[j])):
            end = j
            break
    else:
        end = min(len(code), i + 600)
    return start, end, hook, params


def load_source(addr_expr, consts, text, j):
    """a pointer read from memory at addr_expr -> ('global', G) | ('chain', (parent local, offset)) | ('other', text)"""
    b, off, idx = qfeed.parse_addr(re.sub(r'\b\w+::', '', addr_expr), consts)   # constants are keyed by their bare name (qfeed.load_consts)
    if idx or off is None:
        return ('other', text[:40], j)
    if b == 'ABS' and off >= 0x80000000:
        return ('global', off, j)
    if re.fullmatch(r'[A-Za-z_]\w*', b):
        return ('chain', (b, off), j)
    return ('other', text[:40], j)


def sources(code, orig, start, end, var, consts, params):
    """every way the local `var` gets its value inside [start, end): [(kind, detail, line)]; kinds hookarg / global / param / other / mutated"""
    out = []
    v = re.escape(var)
    assign = re.compile(r'(?<![\w\.>])' + v + r'\s*=(?!=)\s*([^;]*?)\s*(?:;|$)')
    mutate = re.compile(r'(?<![\w\.>])' + v + r'\s*(?:\+\+|--|[-+*/|&^]=|<<=|>>=)|(?:\+\+|--)\s*' + v + r'\b')
    ptype = re.compile(r'\b(?:const\s+)?' + TYPE_NAMES + r'\s*&?\s*' + v + r'\s*(?:,|\)|=[^,)]*[,)])')
    ctorinit = re.compile(r'\b' + TYPE_NAMES + r'\s+' + v + r'\s*[\({]')
    if var in params:
        out.append(('param', 'parameter of the enclosing function', start))
    for j in range(start, end):
        ln = code[j]
        if var not in ln:
            continue
        if (qfeed.DEF_RE.match(ln) or qfeed.LAMBDA_RE.search(ln)) and ptype.search(ln):
            out.append(('param', 'parameter of a function / lambda', j))
        for m in assign.finditer(ln):
            rhs = m.group(1)
            a, _e = qfeed.split_args(rhs + ')', 0)     # the first declarator's initialiser only (`g = 0, p = 0` is two variables)
            rhs = a[0] if a else rhs
            c = qfeed.clean(rhs)
            hm = HOOKARG.match(c)
            gm = GLOAD.match(rhs.strip())
            la = qfeed.split_args(gm.group(1) + ')', 0)[0] if gm else None
            if hm:
                out.append(('hookarg', int(hm.group(1)), j))
            elif c in ('0', '0u', '{}', 'nullptr', 'NULL', 'false'):
                continue                                  # a zero initialiser: the value comes from a later read; never a struct
            elif la:
                out.append(load_source(la[-1], consts, rhs, j))
            else:
                out.append(('other', rhs[:40], j))
        if mutate.search(ln):
            out.append(('mutated', 'compound assignment', j))
        if ctorinit.search(ln):
            out.append(('other', 'constructor-style init', j))
        for cm in qfeed.CALL_RE.finditer(ln):
            name = cm.group(2)
            if name in OUTPARAM_FNS or name == 'read':
                args, _e = qfeed.split_args(ln, cm.end())
                if not args:
                    continue
                if name in OUTPARAM_FNS and args[-1] == var and len(args) > OUTPARAM_FNS[name]:
                    out.append(load_source(args[OUTPARAM_FNS[name]], consts, 'read from ' + args[OUTPARAM_FNS[name]][:30], j))
                elif name == 'read' and len(args) >= 2 and args[1] in ('&' + var, '& ' + var):
                    out.append(('other', 'read() into it', j))
    return out


class Keyer:
    def __init__(self, ix, consts):
        self.ix, self.consts = ix, consts

    def _vt_key(self, vts, evid, route):
        vts = tuple(sorted(vts))
        return dict(route=route, id='vt:%08X' % vts[0], handles=[('vt', vts)], evidence=evid, reason='')

    def resolve(self, code, orig, i, base, depth=0):
        """the key of the base used on line i: dict(route, id, handles, evidence, reason). `unresolved` has a reason."""
        def no(reason):
            return dict(route='unresolved', id='', handles=[], evidence='', reason=reason)
        if base in ('?', ''):
            return no('no variable base parsed from the address')
        if '+' in base:
            return no('address is a sum of two variable bases')
        start, end, hook, params = unit_span(code, i)
        hm = re.fullmatch(r'ctx\.r(\d+)\.u(?:32|64)', base)
        if hm:
            srcs = [('hookarg', int(hm.group(1)), i)]
        elif re.fullmatch(r'[A-Za-z_]\w*', base):
            srcs = sources(code, orig, start, end, base, self.consts, params)
        else:
            return no('member / compound expression `%s`' % base[:30])
        if not srcs:
            return no('no assignment of the local found (member, capture or loop state)')
        kinds = {(k, d) for k, d, _j in srcs}
        if any(k == 'param' for k, _d, _j in srcs):
            return no('parameter of a helper (its callers are not followed)' + ('' if len(kinds) == 1 else '; also assigned elsewhere'))
        if any(k == 'mutated' for k, _d, _j in srcs):
            return no('assigned by compound / increment (a walking pointer)')
        # route 1: hook argument
        if all(k == 'hookarg' for k, _d, _j in srcs):
            if len(kinds) > 1:
                return no('assigned from different hook registers: %s' % ', '.join(sorted('r%d' % d for _k, d in kinds)))
            if hook is None:
                return no('ctx.rN outside a REX_HOOK function entry (mid-asm: no function argument)')
            n = srcs[0][1]
            if not 3 <= n <= 10:
                return no('ctx.r%d is not an argument register' % n)
            vts, gl = self.ix.arg_types(hook, n - 3)
            where = '(sub_%08X a%d)' % (hook, n - 3)
            if len({v for v, _h in vts}) == 1:
                vt = vts[0][0]
                return self._vt_key([vt], 'objtype %s %s [%s]' % (where, hx(vt) + (' ' + self.ix.vname(vt) if self.ix.vname(vt) else ''),
                                                                      ','.join(sorted({h for _v, h in vts}))), 'hookarg')
            if len({v for v, _h in vts}) > 1:
                return no('hook argument %s has %d types in the index (ambiguous)' % (where, len({v for v, _h in vts})))
            if len(gl) == 1:
                return dict(route='hookarg', id=gl[0], handles=[('g', gl[0])], evidence='argflow %s: callers pass %s' % (where, gl[0]), reason='')
            if len(gl) > 1:
                return no('hook argument %s: callers pass %d different globals' % (where, len(gl)))
            return no('hook argument %s: the index knows no type and no global for it' % where)
        # route 2 through a field: the pointer at [parent + D] of a base that is itself rooted at a global (the index's root p(g:G,D))
        if all(k == 'chain' for k, _d, _j in srcs):
            if len(kinds) > 1:
                return no('assigned from %d different parent fields' % len(kinds))
            parent, d = srcs[0][1]
            if depth >= 3 or parent == base:
                return no('pointer chain deeper than the index models (3)')
            pk = self.resolve(code, orig, i, parent, depth + 1)
            pg = [v for kind, v in pk['handles'] if kind == 'g']
            if not pg:
                return no('loaded from field +%d of `%s`, which has no global root (%s)' % (d, parent, pk['reason'] or pk['route']))
            text = 'p(%s,%d)' % (pg[0], d)
            if not self.ix.has_root(text):
                return no('the index has no root %s (no access of that pointer by the game)' % text)
            handles = [('g', text)]
            vts = self.ix.global_types(text)
            if len(vts) == 1:
                handles.append(('vt', tuple(vts)))
            return dict(route='chain', id=text, handles=handles, evidence='pointer loaded from +%d of %s [%s]' % (d, pk['id'], pk['evidence']), reason='')
        # route 2: one global
        if all(k == 'global' for k, _d, _j in srcs):
            if len(kinds) > 1:
                return no('assigned from %d different globals' % len(kinds))
            g = srcs[0][1]
            gt = 'g:%08X' % g
            handles = [('g', gt)]
            ev = 'pointer loaded from global %s' % hx(g)
            vts = self.ix.global_types(gt)
            if len(vts) == 1:      # several vptr stores (base class then derived) = no single type claimed
                handles.append(('vt', tuple(sorted(vts))))
                cs = [self.ix.fname(f) for v in vts for f in self.ix.ctors_of(v)]
                ev += '; type %s%s' % (','.join(hx(v) for v in vts), (' ctor ' + ','.join(cs)) if cs else '')
            return dict(route='global', id=gt, handles=handles, evidence=ev, reason='')
        # route 3: the constructor named on the assignment line
        cs = set()
        for k, _d, j in srcs:
            if k in ('hookarg', 'global', 'chain'):
                return no('assigned from mixed sources (%s)' % ', '.join(sorted({kk for kk, _dd, _jj in srcs})))
            t = ' '.join(orig[x] for x in (j - 1, j) if 0 <= x < len(orig))
            m = CTOR_CMT.search(t)
            if not m:
                return no('assigned from `%s` (not a hook argument, a global, or a named constructor)' % _d)
            cs.add(int(m.group(1)[4:], 16))
        if len(cs) != 1:
            return no('assignment lines name %d different constructors' % len(cs))
        c = next(iter(cs))
        vt = self.ix.ctor_vt(c)
        if vt is None:
            return no('the named constructor sub_%08X is not a constructor in the index' % c)
        return self._vt_key([vt], 'constructor sub_%08X stores %s' % (c, hx(vt)), 'ctor')


# ---------------------------------------------------------------------------------------------- the table
def key_rows(rows, files, W, ix):
    """adds route / key / evidence / writers / result / reason to every row (in place); -> the per-struct read lists"""
    consts = qfeed.load_consts(files)
    keyer = Keyer(ix, consts)
    srcs = {}
    cache = {}
    reads = defaultdict(list)    # key id -> [(off, width)]
    handles_of = {}
    for r in rows:
        r.update(route='', key='', key_name='', evidence='', writers_n='', top_writers='', result='', reason='', old_status=r['status'])
        if r['kind'] not in ('field', 'check', 'hull'):
            r['result'] = {'forward': 'n/a forwarded', 'reg': 'n/a hook register', 'host_ptr': 'n/a span', 'copy_deref': 'n/a bulk copy',
                           'phys': 'n/a physical'}.get(r['kind'], 'n/a')
            continue
        base, hasoff, idx = r['base'], r['off'] != '', r['addr'].endswith('+idx')
        if base == 'ABS':
            r['route'], r['key'], r['evidence'] = 'K', 'K', 'absolute address'
            st = r['old_status']
            r['writers_n'] = r['writers'].split('@')[0] if st == 'K' else ''
            r['result'] = {'K': 'known', 'image': 'image', 'named': 'known'}.get(st, 'unknown')
            if idx:
                r['result'] = 'indexed'
            continue
        if r['file'] not in srcs:
            with open(os.path.join(qfeed.ROOT, r['file'].replace('/', os.sep)), encoding='utf-8', errors='replace') as fh:
                o = fh.read().splitlines()
            srcs[r['file']] = (o, qfeed.strip_code(o))
        orig, code = srcs[r['file']]
        i = r['line'] - 1
        ck = (r['file'], unit_span(code, i)[0], base)
        if ck not in cache:
            cache[ck] = keyer.resolve(code, orig, i, base)
        k = cache[ck]
        r['route'] = k['route']
        if k['route'] == 'unresolved':
            r['reason'] = k['reason']
            r['result'] = 'known' if r['old_status'] == 'named' else 'unknown'   # named = our source names a game writer next to the read
            continue
        r['key'], r['evidence'] = k['id'], k['evidence']
        handles_of[k['id']] = k['handles']
        names = []
        for kind, v in k['handles']:
            if kind == 'vt':
                names += [ix.vname(x) for x in v if ix.vname(x)]
            else:
                gm = re.fullmatch(r'g:([0-9A-F]{8})', v)
                names.append(ix.names.get(int(gm.group(1), 16), '') if gm else '')
        r['key_name'] = ','.join(n for n in names if n)
        if not hasoff:
            r['result'], r['reason'] = 'unknown', 'no constant offset'
            continue
        off = int(r['off'])
        try:
            reads[k['id']].append((off, int(r['width']) if r['width'].isdigit() else 4))
        except ValueError:
            pass
        if idx:
            r['result'], r['reason'] = 'indexed', 'indexed row: the element offset is not +%d' % off
            continue
        w = ix.writers(k['handles'], off)
        r['writers_n'] = str(len(w))
        r['top_writers'] = '; '.join('%s x%d' % (ix.fname(f), n) for f, n in sorted(w.items(), key=lambda t: (-t[1], t[0]))[:3])
        r['result'] = 'known' if w else ('known' if r['old_status'] == 'named' else 'unknown')
    return reads, handles_of


def write_tsv(rows, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write('\t'.join(TSV_COLS) + '\n')
        for r in rows:
            fh.write('\t'.join(str(r.get(c, '')).replace('\t', ' ') for c in TSV_COLS) + '\n')


def summary(rows, out=print):
    fields = [r for r in rows if r['kind'] == 'field']
    out('field reads per family: reads / key resolved / writer known / image data / indexed / writer unknown   [old qfeed known = K+G+T+named]')
    fams = DRAWN + sorted({r['family'] for r in fields} - set(DRAWN))
    tot = Counter()
    for f in fams + ['DRAWN']:
        rs = [r for r in fields if (r['family'] in DRAWN if f == 'DRAWN' else r['family'] == f)]
        if not rs:
            continue
        c = Counter(r['result'] for r in rs)
        res = sum(1 for r in rs if r['route'] not in ('', 'unresolved'))
        old = sum(1 for r in rs if r['old_status'] in ('K', 'G', 'T', 'named'))
        out('  %-10s %4d / %4d / %4d / %3d / %3d / %4d   [old %d]' % (f, len(rs), res, c['known'], c['image'], c['indexed'], c['unknown'], old))
        if f == 'DRAWN':
            tot = (len(rs), res, c['known'], c['image'], c['indexed'], c['unknown'], old)
    out('overall (drawn families): %d reads, key resolved %d (%.1f%%), writer known %d (%.1f%%); FEED.md started at 759 reads, 123 known.'
        % (tot[0], tot[1], 100.0 * tot[1] / max(1, tot[0]), tot[2], 100.0 * tot[2] / max(1, tot[0])))
    out('not field reads (a span or an address decided by the caller, never a fixed field): ' +
        str(dict(Counter(r['kind'] for r in rows if r['kind'] not in ('field',)))))
    # bases per route (units: file + function + local name)
    units = {}
    for r in rows:
        if r['kind'] == 'field' and r['family'] in DRAWN and r['route'] not in ('', 'K'):
            units[(r['file'], r['func'], r['base'])] = (r['route'], r['reason'])
    rc = Counter(v[0] for v in units.values())
    out('bases (file, function, local) in drawn families: %d; by route %s' % (len(units), dict(rc)))
    why = Counter(re.sub(r'`[^`]*`|sub_[0-9A-F]{8}|\d+', '#', v[1]) for v in units.values() if v[0] == 'unresolved')
    for t, n in why.most_common(8):
        out('    unresolved x%d: %s' % (n, t))


def unread(rows, reads, handles_of, ix, out=print):
    total, nstruct, lines = 0, 0, []
    for kid in sorted(handles_of):
        wr = ix.written(handles_of[kid])
        rd = reads.get(kid, [])
        un = sorted(o for o, (w, _n) in wr.items() if not any(ro < o + max(w, 1) and o < ro + rw for ro, rw in rd))
        nstruct += 1
        total += len(un)
        nm = ','.join(n for n in {x for r in rows if r['key'] == kid for x in [r['key_name']]} if n)
        lines.append((kid, nm, len(wr), len({o for o, _w in rd}), un))
    for kid, nm, nw, nr, un in sorted(lines, key=lambda t: -len(t[4])):
        out('%-14s %-28s game writes %4d offsets, we read %3d, never read %4d: %s' %
            (kid, nm[:28], nw, nr, len(un), ' '.join('+%d' % o for o in un[:40]) + (' ...' if len(un) > 40 else '')))
    out('--unread total: %d offsets the game writes and our code never reads, over %d structs with a key.' % (total, nstruct))
    out('cannot see: bulk copies (hull / words(A, n) / ReadMatrices) cover spans we do not count as reads of their fields; indexed reads;'
        ' game writes through indexed stores (not modelled); a struct reached by two keys counts under each.')
    return total


def load_index():
    W = qfeed.Writers()
    if not W.ok:
        return None, None
    import qfield
    rtti = qfield.rtti_names(os.path.dirname(qfeed.DB))
    names = {}
    try:
        import q as qmod
        for addr, _f, e in qmod.load_db_entries():
            names[addr] = e.get('name')
    except Exception:
        pass
    return W, Index(W.con, names, rtti)


def run(argv):
    ap = argparse.ArgumentParser(prog='q feed', description='the scene feed table with struct keys and writers (S110 FEEDKEY)')
    ap.add_argument('where', nargs='?', help='FILE[:LINE]: that file\'s rows')
    ap.add_argument('--unread', action='store_true')
    ap.add_argument('--tsv', default=OUT_TSV)
    ap.add_argument('--writers', metavar='FUNC', help='the read sites FUNC was observed writing in the recorded runs')
    ap.add_argument('--also', action='append', default=[], help='an extra run folder with feed_reads.tsv + stores.tsv')
    a = ap.parse_args(argv)
    import qfeedseen
    W, ix = load_index()
    if W is None:
        print('q feed: no field index (py tools\\q\\q.py field build)', file=sys.stderr)
        return 2
    try:
        seen = qfeedseen.open_db(a.also)
    except qfeedseen.FeedSeenError as e:
        seen = None
        print('q feed: observed writers NOT KNOWN (%s)' % e)
    if a.writers:
        m = re.fullmatch(r'(?:sub_|0x)?([0-9A-Fa-f]{8})', a.writers.strip())
        if not m or seen is None:
            print('q feed --writers: needs a function address (sub_XXXXXXXX) and recorded runs')
            return 2
        f = int(m.group(1), 16)
        qfeedseen.print_writers(f, seen, ix.fname)
        return 0
    rows, files, _p, _W = qfeed.collect(W)
    reads, handles_of = key_rows(rows, files, W, ix)
    write_tsv(rows, a.tsv)
    if a.unread:
        unread(rows, reads, handles_of, ix)
    elif a.where:
        fl, _, ln = a.where.partition(':')
        sel = [r for r in rows if r['file'].replace('\\', '/').endswith(fl.replace('\\', '/')) and (not ln or str(r['line']) == ln)]
        for r in sel:
            print('%s:%d %-12s %-10s %-22s %-9s %s %s | %s %s' % (r['file'], r['line'], r['family'], r['reader'], r['addr'], r['result'],
                  r['key'] or r['route'] or '-', r['evidence'] or r['reason'], r['writers_n'] and ('writers %s' % r['writers_n']), r['top_writers']))
            if seen is not None:
                print('      ' + qfeedseen.row_suffix(r['file'], r['line'], seen, ix.fname))
        print('-- %d rows' % len(sel))
    else:
        summary(rows)
        if seen is not None:
            qfeedseen.seen_summary(rows, seen, DRAWN)
    print('-> %s' % os.path.relpath(a.tsv, qfeed.ROOT), file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(run(sys.argv[1:]))
