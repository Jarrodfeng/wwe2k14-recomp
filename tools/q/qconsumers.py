r"""q consumers (S111 PTRORIGIN): every load / store through a pointer whose ORIGIN is a named value, followed forward.

    py tools/q/q.py consumers --from sub_X [--ret | --arg rN] [--off N] [--depth K] [--json]
    py tools/q/q.py consumers --field OFF [--from FUNC ...] [--off N] [--limit N] [--depth K]
    py tools/q/q.py consumers --stores OFF [--from FUNC ...] [--hit 80,81] [--depth K]

The origin
  --ret      the value sub_X RETURNS (r3): at each call site of sub_X (the q index's call edges + the indirect callers the runs observed) the
             value `ret#SITE` of qdecomp's interpreter is the origin.
  --arg rN   the argument rN sub_X receives (`in_rN`).
  --field OFF  a pointer LOADED from field +OFF (a 4-byte load whose address is base+OFF) in the functions named by --from, or, with no
             --from, in every function the field index holds a read of +OFF in (--limit, default 300; the cut is said).
Followed forward over qslice's per-function facts (the one value engine: qdecomp's interpreter, so a register copy keeps the same
expression), through
  * register copies and `base+K` arithmetic (the offset shifts), joins (a phi with a tracked input, marked `phi`),
  * a stack slot: a store of the pointer to a local and a reload after a call (marked `slot`; flow-insensitive: the slot is `slot?` when
    another value is also stored there),
  * an argument of a call into a callee with a body (`--depth K` hops, default 1), the callee's `in_rN` being the origin.
Per offset (from the origin, shifted by every `+K` on the way): the functions that read / write it, with addresses.
Cuts, counted by reason in the coverage line (never silent): depth, indirect call (the target is unknown), no body, returned onward
(the pointer is the function's own result), tail call, stored into memory (the pointer escapes into an object), indexed address (base+reg,
the offset unknown: listed as `+?`, not followed).
--stores OFF: the other direction, for a field: every 4-byte store at +OFF in the functions (--from, else the field index's W rows), its
value as `q slice` walks it (constant, value set, or ?); --hit V,V lists the stores whose value set holds one of them. No offset claim.
Every row is a lead read off the listing by a tool [C tool]; confirm a surprising one with q asm FUNC --at OFF.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import qcallargs  # noqa: E402
import qdecomp  # noqa: E402
import qslice  # noqa: E402

ARGS = [f'r{i}' for i in range(3, 11)]
FIELD_FUNCS = 300
MAX_PATHS_KEPT = 3


class ConsumersError(Exception):
    pass


def hx(a):
    return f'0x{a:08X}'


# ------------------------------------------------------------------ matching an address against the tracked pointers
def strip(e):
    """base expression and the summed constant offset of an address expression"""
    off = 0
    while isinstance(e, tuple) and e and e[0] == 'add':
        off += e[2]
        e = e[1]
    return e, off


def match(F, e, tracked, level=0):
    """-> (offset from the origin, via) | ('?', via) for an indexed address | None. tracked: atom -> (delta, via)."""
    if not isinstance(e, tuple) or not e or level > 4:
        return None
    b, off = strip(e)
    if b and b[0] == 'def' and not str(b[1]).startswith('L') and b not in tracked:
        full = F['defs'].get(f'{b[1]}@{b[2]:X}')
        if full is not None:
            m = match(F, full, tracked, level + 1)
            return None if m is None else ((m[0] + off) if m[0] != '?' else '?', m[1])
    hit = tracked.get(b)
    if hit is not None:
        return off + hit[0], hit[1]
    if b and b[0] == 'op' and b[1] == '+' and len(b) == 4:
        for x in (b[2], b[3]):
            m = match(F, x, tracked, level + 1)
            if m is not None:
                return '?', m[1]
    return None


def close(F, ins, tracked):
    """grow `tracked` with the values that carry the same pointer: long defs, joins, stack-slot reloads"""
    for _ in range(6):
        n = len(tracked)
        for key, full in F['defs'].items():
            r, rel = key.split('@')
            atom = ('def', r, int(rel, 16))
            if atom in tracked:
                continue
            m = match(F, full, tracked)
            if m is not None and m[0] != '?':
                tracked[atom] = (m[0], m[1])
        for j, regs in F['phis'].items():
            for key, inputs in regs.items():
                atom = ('phi', key, j)
                if atom in tracked:
                    continue
                for _p, v in inputs:
                    m = match(F, v, tracked) if v is not None else None
                    if m is not None and m[0] != '?':
                        tracked[atom] = (m[0], (m[1] + ' phi').strip())
                        break
        slots = {}
        for s in F['stores']:
            if s['stk'] is None:
                continue
            m = match(F, s['val'], tracked) if isinstance(s['val'], tuple) else None
            cur = slots.get(s['stk'])
            if m is not None and m[0] != '?':
                slots[s['stk']] = (m[0], (m[1] + ' slot').strip()) if cur is None or cur[1].endswith('slot') else (m[0], 'slot?')
            elif cur is not None:
                slots[s['stk']] = (cur[0], 'slot?')
            else:
                slots.setdefault(s['stk'], None)
        for rel, rec in ins.items():
            a = rec.get('a')
            if a and a[0] == 'def' and str(a[1]).startswith('L') and a not in tracked:
                try:
                    k = int(a[1][1:])
                except ValueError:
                    continue
                sl = slots.get(k)
                if sl is not None:
                    tracked[a] = sl
        if len(tracked) == n:
            break
    return tracked


# ------------------------------------------------------------------ the walk
class Scan:
    def __init__(self, src, depth=1, con=None):
        self.src, self.depth, self.con = src, depth, con
        self.hits = {}                      # (off, rw, func, site) -> hit dict
        self.funcs = set()
        self.cuts = {}                      # reason -> {keys}
        self.paths = 0
        self.via = {}
        self.visited = set()

    def cut(self, reason, key):
        self.cuts.setdefault(reason, set()).add(key)

    def add_hit(self, off, rw, func, rel, width, path):
        k = (off, rw, func, func + rel)
        h = self.hits.get(k)
        if h is None:
            self.hits[k] = {'off': off, 'rw': rw, 'func': self.src.name(func), 'func_addr': func, 'site': func + rel, 'rel': rel,
                            'width': width, 'paths': [path]}
        elif path not in h['paths'] and len(h['paths']) < MAX_PATHS_KEPT:
            h['paths'].append(path)

    def returned_onward(self, addr, o, v, level, trail):
        """the function returns the pointer (+v[0]): its callers get it as the value `ret#SITE`, one more hop"""
        if level >= self.depth:
            self.cut('returned onward (depth)', (addr, o))
            return
        key = ('ret', addr, v[0])
        if key in self.visited:
            return
        self.visited.add(key)
        rows, _known = qcallargs.sites_of(self.src, addr, self.con)
        for caller, site, kind in sorted(rows):
            F = self.src.facts(caller)
            rel = site - caller
            if kind == 'tail' or not site or F is None or rel not in F['calls']:
                self.cut('returned onward (tail / no call record at the caller)', (addr, caller, site))
                continue
            self.scan(caller, {('ret', rel, 'r3'): (v[0], (v[1] + ' ret').strip())}, level + 1,
                      trail + f' > returned by {self.src.name(addr)} to {self.src.name(caller)}+0x{rel:X}')

    def scan(self, addr, tracked, level, trail):
        F = self.src.facts(addr)
        if F is None:
            self.cut('no body', (addr,))
            return
        self.funcs.add(addr)
        ins = self.src.ins(F)
        tracked = close(F, ins, dict(tracked))
        self.paths += 1
        for t in tracked.values():
            for tag in t[1].split():
                self.via[tag] = self.via.get(tag, 0) + 1
        nm = self.src.name(addr)
        for rel, rec in sorted(ins.items()):
            if rec['mn'] not in qdecomp.LOADS:
                continue
            a = rec.get('a')
            if a is None:
                continue
            if a[0] == 'def' and not str(a[1]).startswith('L'):
                a = F['defs'].get(f'{a[1]}@{a[2]:X}', a)
            if a[0] != 'ld':
                continue
            m = match(F, a[2], tracked)
            if m is None:
                continue
            if m[0] == '?':
                self.cut('indexed address', (addr, rel))
            self.add_hit(m[0], 'R', addr, rel, a[1], trail + (f' [{m[1]}]' if m[1] else ''))
        for s in F['stores']:
            m = match(F, s['af'], tracked)
            if m is not None:
                if m[0] == '?':
                    self.cut('indexed address', (addr, s['o']))
                self.add_hit(m[0], 'W', addr, s['o'], s['w'], trail + (f' [{m[1]}]' if m[1] else ''))
            elif s['stk'] is None and isinstance(s['val'], tuple):
                v = match(F, s['val'], tracked)
                if v is not None and v[0] != '?':
                    self.cut('stored into memory', (addr, s['o']))
        for r in F['rets']:
            v = match(F, r['r3'], tracked) if isinstance(r['r3'], tuple) else None
            if v is not None and v[0] != '?':
                self.returned_onward(addr, r['o'], v, level, trail)
        for rel, call in sorted(F['calls'].items()):
            for reg in ARGS:
                e = call['regs'].get(reg)
                m = match(F, e, tracked) if isinstance(e, tuple) else None
                if m is None or m[0] == '?':
                    continue
                kind = call['k']
                if kind in ('V', 'TAILV') or call.get('t') is None:
                    self.cut('indirect call', (addr, rel, reg))
                    continue
                if level >= self.depth:
                    self.cut('depth', (addr, rel, reg))
                    continue
                key = (call['t'], reg, m[0])
                if key in self.visited:
                    continue
                self.visited.add(key)
                self.scan(call['t'], {('in', reg): (m[0], '')},
                          level + 1, trail + f' > {self.src.name(call["t"])}({reg}{("+%d" % m[0]) if m[0] else ""})'
                          + (f' [{m[1]}]' if m[1] else ''))


# ------------------------------------------------------------------ the origins
def run_ret(src, addr, con, depth, indirect=True):
    sc = Scan(src, depth, con if indirect else None)
    rows, known = qcallargs.sites_of(src, addr, con if indirect else None)
    nm = src.name(addr)
    kinds = {'direct': 0, 'tail': 0, 'indirect': 0}
    for caller, site, kind in sorted(rows):
        if kind == 'tail' or not site:
            kinds['tail'] += 1
            sc.cut('tail call (the result goes to our caller)', (caller, site))
            continue
        kinds['indirect' if kind == 'indirect' else 'direct'] += 1
        F = src.facts(caller)
        rel = site - caller
        if F is None or rel not in F['calls']:
            sc.cut('no body', (caller,))
            continue
        sc.scan(caller, {('ret', rel, 'r3'): (0, '')}, 0, f'{src.name(caller)}+0x{rel:X} (result of {nm})')
    return sc, {'origin': f'the value returned by {nm}', 'sites': len(rows), 'kinds': kinds, 'indirect_known': known}


def run_arg(src, addr, reg, depth):
    sc = Scan(src, depth)
    nm = src.name(addr)
    sc.scan(addr, {('in', reg): (0, '')}, 0, f'{nm} argument {reg}')
    return sc, {'origin': f'argument {reg} of {nm}', 'sites': 1, 'kinds': {}, 'indirect_known': True}


def field_funcs(off, limit, rw='R'):
    import qfield
    a = argparse.Namespace(root=None, gen=None, image=None, db=None, no_rebuild=True, rebuild=False)
    db = qfield.open_db(a, quiet=True)
    try:
        rows = [r[0] for r in db.execute('select distinct func from rows where off=? and rw=? and width=4 order by func', (off, rw))]
    finally:
        db.close()
    return rows[:limit], len(rows)


def load_offset(a):
    """the +OFF of a 4-byte load's value expression ('ld', 4, addr, img) off a non-stack base"""
    b, off = strip(a[2])
    return off, b


def run_field(src, off, funcs, depth, total=None, limit=None):
    sc = Scan(src, depth)
    n_atoms = 0
    for f in funcs:
        F = src.facts(f)
        if F is None:
            sc.cut('no body', (f,))
            continue
        ins = src.ins(F)
        atoms = {}
        for rel, rec in sorted(ins.items()):
            a = rec.get('a')
            if rec['mn'] in qdecomp.LOADS and a and a[0] == 'ld' and a[1] == 4:
                o, b = load_offset(a)
                if o == off and b[0] not in ('sp', 'c'):
                    atoms[a] = (0, '')
                    n_atoms += 1
        if atoms:
            sc.scan(f, atoms, 0, f'{src.name(f)} (pointer loaded from +{off})')
    extra = {'origin': f'a pointer loaded from field +{off}', 'sites': n_atoms, 'kinds': {'loads': n_atoms}, 'indirect_known': True}
    if total is not None and limit is not None and total > limit:
        extra['truncated'] = f'{limit} of {total} functions with a read of +{off} in the field index (--limit)'
        sc.cut('functions beyond --limit', (total - limit,))
    return sc, extra


# ------------------------------------------------------------------ output
def result_of(sc, extra, off_filter, mode):
    by = {}
    for h in sc.hits.values():
        if off_filter is not None and h['off'] != off_filter:
            continue
        by.setdefault(h['off'], []).append(h)
    for v in by.values():
        v.sort(key=lambda h: (h['rw'], h['site']))
    cuts = {k: len(v) for k, v in sorted(sc.cuts.items())}
    if 'functions beyond --limit' in sc.cuts:
        cuts['functions beyond --limit'] = sum(x[0] for x in sc.cuts['functions beyond --limit'])
    return {'mode': mode, 'origin': extra['origin'], 'origin_sites': extra['sites'], 'origin_kinds': extra['kinds'],
            'indirect_known': extra['indirect_known'], 'truncated': extra.get('truncated'),
            'offsets': {str(k): v for k, v in sorted(by.items(), key=lambda kv: (kv[0] == '?', kv[0] if kv[0] != '?' else 0))},
            'coverage': {'functions_scanned': len(sc.funcs), 'paths_followed': sc.paths, 'via': dict(sorted(sc.via.items())),
                         'cut': cuts, 'cut_at': {k: [':'.join(hx(x) if isinstance(x, int) and x > 0xFFFF else (f'+0x{x:X}' if isinstance(x, int) else str(x)) for x in key)
                                                      for key in sorted(v, key=str)[:12]] for k, v in sorted(sc.cuts.items())}, 'hits': sum(len(v) for v in by.values())}}


def coverage_line(r):
    c = r['coverage']
    cut = ', '.join(f'{v} {k}' for k, v in c['cut'].items()) or 'none'
    via = ', '.join(f'{v} {k}' for k, v in c['via'].items())
    k = r['origin_kinds']
    kinds = ', '.join(f'{v} {n}' for n, v in k.items() if v) if k else ''
    return (f'coverage: origin {r["origin"]}: {r["origin_sites"]} site(s)' + (f' ({kinds})' if kinds else '') +
            f'; {c["paths_followed"]} consumer paths followed in {c["functions_scanned"]} functions' + (f' (tracked values by how they carry the pointer: {via})' if via else '') +
            f'; cut: {cut}' + ('' if r['indirect_known'] else '; the observed-run index is not here: indirect callers NOT listed') +
            (f'; {r["truncated"]}' if r['truncated'] else ''))


def print_result(r, out=print):
    out(f'q consumers: {r["origin"]}')
    if not r['offsets']:
        out('  no load or store through this pointer was found')
    for off, hs in r['offsets'].items():
        rd = [h for h in hs if h['rw'] == 'R']
        wr = [h for h in hs if h['rw'] == 'W']
        out(f'+{off}: {len(rd)} read(s) in {len({h["func_addr"] for h in rd})} function(s), {len(wr)} write(s) in {len({h["func_addr"] for h in wr})} function(s)')
        for h in hs:
            out(f'    {h["rw"]} {hx(h["site"])} {h["func"]}+0x{h["rel"]:X} w{h["width"]}   {h["paths"][0]}' +
                (f'   (+{len(h["paths"]) - 1} more path(s))' if len(h['paths']) > 1 else ''))
    out(coverage_line(r))


# ------------------------------------------------------------------ --stores
def run_stores(src, off, funcs, depth, budget, hits):
    sl = qslice.Slicer(src, depth=depth, budget=budget)
    rows = []
    for f in funcs:
        F = src.facts(f)
        if F is None:
            continue
        for s in F['stores']:
            if s['w'] != 4 or s['stk'] is not None:
                continue
            b, o = strip(s['af'])
            if o != off or b[0] in ('sp', 'c'):
                continue
            nm = src.name(f)
            v = s['val']
            if isinstance(v, tuple) and v[0] == 'c':
                rows.append({'func': nm, 'site': f + s['o'], 'how': 'immediate', 'kind': 'const', 'values': [v[1]], 'unknown': [], 'def': ''})
                continue
            try:
                r = qcallargs.value_of(sl, F, v, f'{nm}+0x{s["o"]:X}')
            except (qslice.SliceError, qdecomp.DecompError, RecursionError) as ex:
                r = {'kind': 'unknown', 'values': [], 'unknown': [f'error: {ex}'], 'def': ''}
            rows.append({'func': nm, 'site': f + s['o'], 'how': 'register', 'kind': r['kind'], 'values': r['values'],
                         'unknown': r['unknown'][:3], 'def': r['def']})
    want = set(hits)
    for r in rows:
        r['hit'] = sorted(want & set(r['values']))
    return rows


def print_stores(rows, off, hits, out=print):
    n_imm = sum(1 for r in rows if r['how'] == 'immediate')
    reg = [r for r in rows if r['how'] == 'register']
    k = {x: sum(1 for r in reg if r['kind'] == x) for x in ('const', 'set', 'unknown')}
    out(f'q consumers --stores {off}: {len(rows)} 4-byte stores at +{off} in {len({r["func"] for r in rows})} functions: {n_imm} immediate, '
        f'{len(reg)} register-valued ({k["const"]} trace to one constant, {k["set"]} to a value set, {k["unknown"]} stay unknown)')
    if hits:
        hh = [r for r in rows if r['hit']]
        out(f'stores whose value (set) holds {sorted(hits)}: {len(hh)}')
        for r in hh:
            out(f'  {hx(r["site"])} {r["func"]} {r["how"]} {r["kind"]} {r["values"]}' + (f' unknown {r["unknown"]}' if r['unknown'] else ''))
        imm = sorted({v for r in rows if r['how'] == 'immediate' for v in r['values'] if v in hits})
        out(f'immediate stores of {sorted(hits)}: {imm or "none"}')
    cons = {}
    for r in reg:
        if r['kind'] == 'const':
            cons.setdefault(r['values'][0], []).append(r)
    out('register-valued stores that trace to one constant, by value: ' + (', '.join(f'{v} x{len(x)}' for v, x in sorted(cons.items())[:40]) or 'none'))


# ------------------------------------------------------------------ the verb
def main(argv, con):
    ap = argparse.ArgumentParser(prog='q consumers', description=__doc__.split('\n')[0])
    ap.add_argument('--from', dest='frm', action='append', default=[], metavar='FUNC', help='the function (repeat with --field / --stores)')
    ap.add_argument('--ret', action='store_true', help='the origin is the value FUNC returns (default with --from)')
    ap.add_argument('--arg', metavar='rN', help='the origin is the argument rN of FUNC')
    ap.add_argument('--field', type=int, metavar='OFF', help='the origin is a pointer loaded from field +OFF')
    ap.add_argument('--stores', type=int, metavar='OFF', help='list the stores at +OFF with their values (no pointer origin)')
    ap.add_argument('--hit', default='', help='with --stores: values to look for (80,81)')
    ap.add_argument('--off', type=lambda s: int(s, 0), help='report only this offset')
    ap.add_argument('--depth', type=int, default=None, help='callee hops (default 1); with --stores the slice depth (default 3)')
    ap.add_argument('--budget', type=int, default=400)
    ap.add_argument('--limit', type=int, default=FIELD_FUNCS, help=f'functions read from the field index (default {FIELD_FUNCS})')
    ap.add_argument('--no-indirect', action='store_true')
    ap.add_argument('--no-cache', action='store_true')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    import q as qmod

    def res(name):
        try:
            return qmod.resolve(con, name)[0]
        except Exception as ex:  # noqa: BLE001
            if type(ex).__name__ == 'QError':
                raise ConsumersError(str(ex))
            raise
    src = qslice.QSource(con, use_cache=not a.no_cache)
    modes = sum(x is not None and x is not False for x in (a.field, a.stores)) + (1 if a.frm and a.field is None and a.stores is None else 0)
    if modes != 1:
        raise ConsumersError('name one origin: --from FUNC [--ret | --arg rN], or --field OFF [--from FUNC ...], or --stores OFF')
    if a.stores is not None:
        funcs = [res(f) for f in a.frm]
        total = None
        if not funcs:
            funcs, total = field_funcs(a.stores, a.limit, 'W')
        hits = [int(x, 0) for x in a.hit.split(',') if x.strip()]
        rows = run_stores(src, a.stores, funcs, 3 if a.depth is None else a.depth, a.budget, hits)
        if a.json:
            print(json.dumps({'off': a.stores, 'functions': len(funcs), 'functions_total': total or len(funcs), 'rows': rows}, indent=1))
        else:
            print_stores(rows, a.stores, hits)
            if total is not None and total > len(funcs):
                print(f'coverage: {len(funcs)} of {total} functions with a W row at +{a.stores} (--limit)')
            else:
                print(f'coverage: {len(funcs)} functions scanned')
        return 0
    depth = 1 if a.depth is None else a.depth
    if a.field is not None:
        total = None
        funcs = [res(f) for f in a.frm]
        if not funcs:
            funcs, total = field_funcs(a.field, a.limit)
        sc, extra = run_field(src, a.field, funcs, depth, total, a.limit)
        mode = 'field'
    elif a.arg:
        if not qslice.REG_RE.match(a.arg):
            raise ConsumersError(f'{a.arg!r} is not a register (r3 ... r10)')
        sc, extra = run_arg(src, res(a.frm[0]), a.arg, depth)
        mode = 'arg'
    else:
        sc, extra = run_ret(src, res(a.frm[0]), con, depth, not a.no_indirect)
        mode = 'ret'
    r = result_of(sc, extra, a.off, mode)
    if a.json:
        print(json.dumps(r, indent=1))
    else:
        print_result(r)
    return 0
