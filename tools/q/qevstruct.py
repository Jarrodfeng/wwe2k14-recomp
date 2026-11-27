r"""q slice --struct sub_X+OFF rN [--fields OFF,OFF..] [--follow-callers K] [--init sub_Y ..] [--json]   (S111 QEVENT)

The STACK STRUCT whose address is in rN at the call at sub_X+OFF: for each field (a word at struct+OFF) the store that reaches it on each
path (walked backwards over the block graph qslice's facts hold; the nearest store to that slot on a path wins), as a value:
  * a constant, a value set (a phi / a callee's return constants, one leaf each) or ? with its definition (qdecomp's expression);
  * `init Y` when the path first meets a call to an --init function Y (its own writes are the field's first definition; a planted
    default table gives the value, else `?(init)`); a stack store AFTER the init call and before the consumer wins over it;
  * `filled by sub_Z` when a direct call that was given the struct address sits between the last store and the consumer (a callee
    may have written the field: the value is not trusted); a call through a register (a virtual post of the same struct) is walked past;
  * `no store` when a path reaches the function entry without one.
A value that is an argument of the function climbs into every caller (q index call / tail edges, then the indirect callers the q index
resolves exactly), K levels (--follow-callers, default 3), one row per caller site. A struct pointer that is itself an argument
(the call passes in_rM) is followed the same way: each caller's own stack struct is read.
No listing reader of its own: qslice.QSource facts (qdecomp's interpreter) are the only source.
"""
import json
import re
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import qdecomp  # noqa: E402
import qslice  # noqa: E402

DEF_SIZE = 64


class StructError(Exception):
    pass


def sval(v):
    return v - (1 << 32) if v >= 0x80000000 else v


# ------------------------------------------------------------------ values
def callee_returns(src, t, depth=2):
    G = src.facts(t)
    if G is None or depth == 0:
        return ['sub_%08X()' % t]
    out = []
    for r in G['rets']:
        out += [('RET', 'sub_%08X' % t, x) if isinstance(x, tuple) else x for x in expand(src, G, r['r3'], None, depth - 1)]
    for o, c in G['calls'].items():
        if c['k'] == 'TAIL' and c.get('t'):
            out += callee_returns(src, c['t'], depth - 1)
    return out


def expand(src, F, e, seen=None, depth=2):
    """-> leaves: ints, ('ARG', 'rN') (an entry argument), or the text of a leaf that stays unknown"""
    seen = set() if seen is None else seen
    if e is None:
        return ['?']
    k = e[0]
    if k == 'c':
        return [sval(e[1])]
    if k == 'in' and e[1].startswith('r'):
        return [('ARG', e[1])]
    if k == 'ret' and e[2] == 'r3' and F['calls'].get(e[1], {}).get('t') and depth > 0:
        out = []
        for x in callee_returns(src, F['calls'][e[1]]['t'], depth):
            if x not in out:
                out.append(x)
        return out
    if k == 'phi':
        key = (e[1], e[2])
        if key in seen:
            return []
        seen.add(key)
        out = []
        for p, v in F['phis'].get(e[2], {}).get(e[1], []):
            if v is None or (p is not None and p >= e[2]):
                out.append('?loop' if v is not None else '?')
                continue
            for x in expand(src, F, v, seen, depth):
                if x not in out:
                    out.append(x)
        return out
    return [qdecomp.fmt(e)]


# ------------------------------------------------------------------ the reaching stores of one stack struct
def _block_of(F, a):
    for s, b in F['blocks'].items():
        if s <= a < (b['end'] or s):
            return s
    return None


def _touches(v, base, size):
    return v is not None and v[0] != 'undef' and qdecomp.is_stack(v) and base <= qdecomp.stack_off(v) < base + size


def reach_field(F, rel, base, off, size, inits):
    """-> [(kind, payload)] one per path: ('store', store) | ('init', callee) | ('filled', callee) | ('none', None)"""
    slot = base + off
    A = F['addr']
    start = _block_of(F, A + rel)
    ev = {}                                         # block -> events (rel, kind, rec), descending
    for s in F['stores']:
        if s['stk'] is not None and s['stk'] < slot + 4 and slot < s['stk'] + s['w']:
            ev.setdefault(s['blk'], []).append((s['o'], 'S', s))
    for o, c in F['calls'].items():
        if any(_touches(v, base, size) for r, v in c['regs'].items() if r.startswith('r')):
            b = _block_of(F, A + o)
            if b is not None:
                ev.setdefault(b, []).append((o, 'C', c))
    out, seen = [], set()

    def walk(blk, limit):
        key = (blk, limit is None)
        if key in seen:
            return
        seen.add(key)
        for o, k, rec in sorted(ev.get(blk, []), key=lambda x: -x[0]):
            if limit is not None and o >= limit:
                continue
            if k == 'S':
                out.append(('store', rec))
                return
            if rec.get('t') in inits:
                out.append(('init', rec['t']))
                return
            if rec.get('t'):
                out.append(('filled', rec['t']))
                return
            # a call through a register that was given the struct: a virtual post of it; walked past
        pr = F['blocks'][blk]['preds']
        if not pr:
            out.append(('none', None))
            return
        for p, _k in pr:
            walk(p, None)
    if start is not None:
        walk(start, rel)
    return out


def _entry(src, F, kind, payload, inits, off):
    """-> [{'v': leaf, 'at': text}]"""
    nm = lambda a: src.name(a)  # noqa: E731
    if kind == 'store':
        at = f'{F["name"]}+0x{payload["o"]:X}'
        if payload['stk'] is None or payload['w'] != 4:
            return [{'v': '?(partial store)', 'at': at}]
        return [{'v': x, 'at': at} for x in expand(src, F, payload['val'])]
    if kind == 'init':
        d = inits.get(payload) or {}
        return [{'v': d[off] if off in d else '?(init)', 'at': f'init {nm(payload)}'}]
    if kind == 'filled':
        return [{'v': f'filled by {nm(payload)}', 'at': f'call {nm(payload)}'}]
    return [{'v': '?', 'at': 'no store'}]


def all_callers(src, faddr):
    out = list(src.callers(faddr))
    if out:
        return out
    con = getattr(src, 'con', None)
    if con is None:
        return []
    try:
        import q as qmod
        d = qmod.indirect_of(con, faddr)
    except Exception:  # noqa: BLE001 - no icall tables: only the direct callers are known
        return []
    return [(s['func'] if isinstance(s['func'], int) else int(str(s['func'])[4:], 16), int(s['site'], 16) if isinstance(s['site'], str) else s['site'], 'V')
            for s in d.get('sites', []) if s.get('status') == 'exact']


def _dedupe(lst):
    out = []
    for x in lst:
        if x not in out:
            out.append(x)
    return out


def bind(src, faddr, row, K):
    """-> rows: the ARG leaves of row's fields replaced by each caller's argument (one row per caller site)"""
    args = {x['v'][1] for v in row['fields'].values() for x in v if isinstance(x['v'], tuple)}
    if not args or K == 0:
        return [row]
    callers = all_callers(src, faddr)
    if not callers:
        return [dict(row, via=row['via'] + ['(no caller found)'])]
    rows = []
    for csrc, csite, _k in callers:
        G = src.facts(csrc)
        c = G and G['calls'].get(csite - csrc)
        if not c:
            rows.append(dict(row, via=row['via'] + ['sub_%08X+0x%X ?' % (csrc, csite - csrc)]))
            continue
        nf = {}
        for off, v in row['fields'].items():
            nv = []
            for x in v:
                if isinstance(x['v'], tuple):
                    for y in expand(src, G, c['regs'].get(x['v'][1])):
                        nv.append({'v': y, 'at': f'{G["name"]}+0x{csite - csrc:X} {x["v"][1]}'})
                else:
                    nv.append(x)
            nf[off] = _dedupe(nv)
        rows += bind(src, csrc, dict(row, fields=nf, via=row['via'] + [f'{G["name"]}+0x{csite - csrc:X}']), K - 1)
    return rows


def struct_rows(src, faddr, rel, reg, fields=None, K=3, inits=None, size=DEF_SIZE, _via=None):
    """-> [{'via': [...], 'fields': {off: [{'v','at'}]}, 'func','rel', 'base'} | {'err': text, ...}] for the struct in `reg` at the call"""
    inits = inits or {}
    F = src.facts(faddr)
    via = _via or []
    if F is None:
        raise StructError(f'0x{faddr:08X} has no generated body')
    c = F['calls'].get(rel)
    if c is None:
        raise StructError(f'+0x{rel:X} is not a call in {F["name"]} (calls at: ' + ', '.join(f'+0x{r:X}' for r in sorted(F['calls'])) + ')')
    e = c['regs'].get(reg)
    head = {'func': F['name'], 'rel': rel}
    if e is not None and e[0] == 'in' and e[1].startswith('r') and K > 0:
        rows = []
        for csrc, csite, _k in all_callers(src, faddr):
            G = src.facts(csrc)
            cc = G and G['calls'].get(csite - csrc)
            if not cc:
                continue
            rows += struct_rows(src, csrc, csite - csrc, e[1], fields, K - 1, inits, size, via + [f'{F["name"]}+0x{rel:X} <- {G["name"]}+0x{csite - csrc:X}'])
        return rows or [dict(head, err=f'struct pointer is in_{e[1]}; no caller found', via=via)]
    if e is None or not qdecomp.is_stack(e):
        return [dict(head, err='struct not on the stack: %s = %s' % (reg, qdecomp.fmt(e) if e else '?'), via=via)]
    base = qdecomp.stack_off(e)
    if fields is None:
        fields = sorted({s['stk'] - base for s in F['stores'] if s['stk'] is not None and base <= s['stk'] < base + size and (s['stk'] - base) % 4 == 0})
    row = dict(head, base=base, via=list(via), fields={})
    for off in fields:
        ents = []
        for kind, pl in reach_field(F, rel, base, off, size, inits):
            ents += _entry(src, F, kind, pl, inits, off)
        row['fields'][off] = _dedupe(ents)
    return bind(src, faddr, row, K)


# ------------------------------------------------------------------ printing
def leaf_text(v):
    return f'arg {v[1]}' if isinstance(v, tuple) else str(v)


def print_rows(head, rows):
    print(head)
    for r in rows:
        if 'err' in r:
            print(f'  {r["func"]}+0x{r["rel"]:X}: {r["err"]}' + (f'   via {" > ".join(r["via"])}' if r['via'] else ''))
            continue
        print(f'  {r["func"]}+0x{r["rel"]:X} struct sp+{r["base"]}' + (f'   via {" > ".join(r["via"])}' if r['via'] else ''))
        for off, ents in r['fields'].items():
            vs = ' | '.join(f'{leaf_text(x["v"])} [{x["at"]}]' for x in ents)
            print(f'    +{off}: {vs}')


def to_json(rows):
    return [dict(r, fields={str(o): [dict(x, v=leaf_text(x['v']) if isinstance(x['v'], tuple) else x['v']) for x in v] for o, v in r['fields'].items()})
            if 'fields' in r else r for r in rows]


def _num(s):
    return int(s, 0)


def main(argv, con):
    import argparse
    ap = argparse.ArgumentParser(prog='q slice --struct', description=__doc__.split('\n')[0])
    ap.add_argument('--struct', metavar='sub_X+OFF', required=True)
    ap.add_argument('reg', help='the register that holds the struct address at the call (r4)')
    ap.add_argument('--fields', help='offsets, comma separated (default: every word stored in the first 64 bytes)')
    ap.add_argument('--follow-callers', type=int, default=3, metavar='K', help='levels of callers an argument climbs (default 3)')
    ap.add_argument('--init', action='append', default=[], metavar='sub_Y', help='an init function: a path reaching its call has its first definition there')
    ap.add_argument('--size', type=int, default=DEF_SIZE, help='bytes of the struct (a call given its address inside it fills it; default 64)')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--no-cache', action='store_true')
    a = ap.parse_args(argv)
    import q as qmod
    fn_s, rel = qslice.parse_site(a.struct)
    if not qslice.REG_RE.match(a.reg):
        raise StructError(f'{a.reg!r} is not a register')
    try:
        addr = qmod.resolve(con, fn_s)[0]
        inits = {qmod.resolve(con, i)[0]: {} for i in a.init}
    except Exception as ex:  # noqa: BLE001
        if type(ex).__name__ == 'QError':
            raise StructError(str(ex))
        raise
    src = qslice.QSource(con, use_cache=not a.no_cache)
    fields = [_num(x) for x in a.fields.split(',')] if a.fields else None
    rows = struct_rows(src, addr, rel, a.reg, fields, a.follow_callers, inits, a.size)
    if a.json:
        print(json.dumps(to_json(rows), indent=1))
    else:
        print_rows(f'q slice --struct {a.struct} {a.reg}: {len(rows)} row(s) (one per caller site where an argument was followed)', rows)
    return 0
