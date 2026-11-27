"""q callargs FUNC REG [REG...] [--json]   (S111 CALLARGS): the value of each named register at EVERY call site of FUNC.

Per site (the q index's direct call / tail edges, plus the indirect callers the runs OBSERVED, marked `indirect`): the register's value as
`q slice` walks it (qslice.Slicer over qdecomp's interpreter, no listing reader of its own): a constant, a value set (a phi / a caller's
argument / a callee return with every path), or `?` with its definition. Each site is joined to
  * the jump-table CASE the call sits under (the switch's case value: the case index + the base of the `addi rD,rS,-K` that feeds the
    cmplwi bounds check; the selector shown as the decomp renders it), and
  * a SCREEN CODE, stated only where the decomp shows the compare: a guard `[in_r3+4] == N` on the way to the call, or a case whose selector
    renders as `[in_r3+4]` (the controller's screen word; --screen-field OFF changes the offset). A `!=` guard is listed, never turned into a screen.
Guards = the branch conditions on the single-predecessor chain above the call (the same chain `q slice` uses for a phi path).
The caller's block chain stops at the first block with several non-case predecessors: a site below a join has no guards / case, said so.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qdecomp  # noqa: E402
import qslice  # noqa: E402

CHAIN_MAX = 60
NOISE_RE = re.compile(r'bdz|bdnz|--ctr')     # a counter-branch pseudo condition of the decomp (a bdz chain), not a test of any value


class CallArgsError(Exception):
    pass


# ------------------------------------------------------------------ a Node -> a value set
def _num(text):
    try:
        return int(text, 0)
    except (ValueError, TypeError):
        return None


def flatten(n, out=None):
    """-> (values, unknowns): the constants a slice Node can take (ints) and the texts of the leaves that stay unknown"""
    vals, unk = ([], []) if out is None else out
    if n.kind == 'const':
        v = _num(n.text)
        (vals if v is not None else unk).append(v if v is not None else n.text)
    elif n.kids and n.kind in ('phi', 'arg', 'local', 'ret'):
        for k in n.kids:
            flatten(k, (vals, unk))
    elif n.kind == 'cycle':
        pass                                # the same value again: adds nothing
    else:
        unk.append(n.text)
    return vals, unk


def value_of(sl, F, e, label):
    """-> {'kind': const|set|unknown, 'values': [ints], 'unknown': [texts], 'def': text} for expression e of function F"""
    node = sl.value(F, e, 0, None, label)
    vals, unk = flatten(node)
    vals = sorted(set(vals))
    unk = list(dict.fromkeys(unk))
    if vals and not unk:
        kind = 'const' if len(vals) == 1 else 'set'
    elif vals:
        kind = 'set'
    else:
        kind = 'unknown'
    return {'kind': kind, 'values': vals, 'unknown': unk, 'def': node.text if kind != 'const' else '',
            'note': node.note or ''}


# ------------------------------------------------------------------ the case / guard join
def _block_of(F, addr):
    for s, b in F['blocks'].items():
        if s <= addr < (b['end'] or s):
            return s
    return None


def chain_of(F, site_addr):
    """-> (guards [condition text], cases [(switch block, target block)]) up the single-predecessor chain of the block holding site_addr"""
    cur = _block_of(F, site_addr)
    guards, cases = [], []
    seen = set()
    while cur is not None and cur not in seen and len(seen) < CHAIN_MAX:
        seen.add(cur)
        b = F['blocks'][cur]
        pr = b['preds']
        if not pr:
            break
        if all(k == 'case' for _p, k in pr) and len({p for p, _k in pr}) == 1:
            cases.append((pr[0][0], cur))
            cur = pr[0][0]
            continue
        if len(pr) != 1:
            break
        p, kind = pr[0]
        t = F['blocks'][p]['term']
        if t and t['k'] == 'cond':
            if kind == 'br' and t.get('target') == cur and t.get('cond'):
                guards.append(t['cond'])
            elif kind == 'fall' and t.get('neg'):
                guards.append(t['neg'])
        cur = p
    guards.reverse()
    return guards, cases


def case_values(insns, sw_end, target, fn_addr, insmap, F, sl):
    """the case values reaching `target` from the bctr that ends at sw_end: -> (values, selector text, base) or None.
    values = index + K where K comes from the `addi rD,rS,-K` that feeds the `cmplwi crN,rD,M` bounds check above the bctr."""
    pos = {i.addr: k for k, i in enumerate(insns)}
    k = pos.get(sw_end - 4)
    if k is None or insns[k].mn != 'bctr' or not insns[k].cases:
        return None
    idx = [i for i, t in enumerate(insns[k].cases) if t == target]
    if not idx:
        return None
    rd, base, sel, selrel = None, 0, None, None
    for j in range(k - 1, max(k - 30, -1), -1):
        ins = insns[j]
        ops = qdecomp.split_ops(ins.ops)
        if rd is None:
            if ins.mn == 'cmplwi' and len(ops) == 3:
                rd = ops[1]
                sel, selrel = rd, None          # no addi: the selector is the compared register itself
        elif ins.mn in ('addi', 'subi') and len(ops) == 3 and ops[0] == rd:
            m = _num(ops[2])
            if m is not None:
                base = -m if ins.mn == 'addi' else m
                sel, selrel = ops[1], ins.addr - fn_addr
            break
    sel_text = '?'
    if sel is not None:
        row = insmap.get(selrel if selrel is not None else _cmp_rel(insns, k, fn_addr))
        if row and sel in row['b']:
            sel_text = sl.clip(sl.render(F, row['b'][sel], None, 'expr'), 80)
    return [i + base for i in idx], sel_text, base


def _cmp_rel(insns, k, fn_addr):
    for j in range(k - 1, max(k - 30, -1), -1):
        if insns[j].mn == 'cmplwi':
            return insns[j].addr - fn_addr
    return -1


def screen_words(F, field):
    """the texts that name the screen code in this function: the controller field, plus any word the function's own branch compares it
    against (the leaf map opens with `[in_r3+4] == [[0x8392945C]+364]`: the SmartMenu's current screen)"""
    fld = f'[in_r3+{field}]'
    words = {fld}
    for b in F['blocks'].values():
        t = b['term']
        if t and t['k'] == 'cond' and t.get('cond'):
            for c in (t['cond'], t.get('neg') or ''):
                m = re.fullmatch(re.escape(fld) + r' [!=]= (\[.*\])', c)
                if m:
                    words.add(m.group(1))
    return words


def screens_of(guards, words):
    """-> (screen text or '', the codes a `!=` guard rules out). Only the decomp's own compares count."""
    eq, ne = [], []
    for g in guards:
        for w in words:
            m = re.fullmatch(re.escape(w) + r' (==|!=) (-?\d+)', g)
            if m:
                (eq if m.group(1) == '==' else ne).append(int(m.group(2)))
    return (' / '.join(str(v) for v in dict.fromkeys(eq)) if eq else ''), ne


# ------------------------------------------------------------------ the sites
def sites_of(src, addr, con=None):
    """-> [(caller, site, kind)] direct (call / tail) from the index, then the observed indirect ones (kind 'indirect', site 0 = a tail)"""
    rows = [(s, a, k) for s, a, k in src.callers(addr)]
    if con is not None:
        try:
            import qseen
            sc = qseen.open_seen()
            seen = {(s, a) for s, a, _k in rows}
            for func, site, runs in qseen.callers_of(addr, sc):
                if (func, site) not in seen:
                    rows.append((func, site, 'indirect'))
        except Exception:  # noqa: BLE001 - no run index here: the indirect callers are then simply not listed (said in the header)
            return rows, False
    return rows, True


def default_listing(src):
    def listing(addr):
        f = src.func(addr)
        if f is None:
            return None
        return qdecomp.read_listing(os.path.join(qdecomp.GEN, f['file']), f['line'], f['name'], addr)[0]
    return listing


def callargs(src, addr, regs, depth=3, budget=400, screen_field=4, listing=None, con=None, indirect=True):
    """-> {'func','addr','regs','sites': [...], 'indirect_known': bool}; one site dict per call site, in address order"""
    listing = listing or default_listing(src)
    sl = qslice.Slicer(src, depth=depth, budget=budget)
    rows, known = sites_of(src, addr, con if indirect else None)
    out = []
    for caller, site, kind in sorted(rows, key=lambda r: (r[0], r[1])):
        d = {'caller': src.name(caller), 'caller_addr': caller, 'site': site, 'rel': site - caller if site else None,
             'kind': kind, 'args': {}, 'guards': [], 'case': None, 'screen': '', 'not_screen': []}
        F = src.facts(caller)
        call = F['calls'].get(site - caller) if (F and site) else None
        if F is None or call is None:
            d['note'] = 'no body / no call record at the site' if F is None or site else 'a tail call (no site): no arguments known'
            for r in regs:
                d['args'][r] = {'kind': 'unknown', 'values': [], 'unknown': ['?'], 'def': 'no call record', 'note': ''}
            out.append(d)
            continue
        for r in regs:
            e = call['regs'].get(r)
            if e is None:
                d['args'][r] = {'kind': 'unknown', 'values': [], 'unknown': ['?'], 'def': f'{r} not known at the call', 'note': ''}
            else:
                d['args'][r] = value_of(sl, F, e, f'{d["caller"]}+0x{site - caller:X} {r}')
        guards, cases = chain_of(F, site)
        d['guards'] = [g for g in guards if not NOISE_RE.search(g)]
        words = screen_words(F, screen_field)
        if cases:
            sw_block, target = cases[0]
            ins = listing(caller)
            cv = case_values(ins, F['blocks'][sw_block]['end'], target, caller, src.ins(F), F, sl) if ins else None
            if cv:
                vals, seltxt, base = cv
                d['case'] = {'values': vals, 'selector': seltxt, 'base': base}
                if seltxt.replace(' ', '') in {w.replace(' ', '') for w in words}:
                    d['screen'] = ' / '.join(str(v) for v in vals)
        scr, ne = screens_of(guards, words)
        if scr and not d['screen']:
            d['screen'] = scr
        d['not_screen'] = ne
        out.append(d)
    return {'func': src.name(addr), 'addr': addr, 'regs': regs, 'sites': out, 'indirect_known': known}


# ------------------------------------------------------------------ printing
def val_text(a):
    if a['kind'] == 'const':
        return str(a['values'][0])
    if a['kind'] == 'set':
        s = '{' + ', '.join(str(v) for v in a['values']) + (', ?' if a['unknown'] else '') + '}'
        return s
    return '? = ' + (a['unknown'][0] if a['unknown'] else '?') + (f'   [{a["def"]}]' if a['def'] and a['def'] != (a['unknown'] or [''])[0] else '')


def print_result(r, out=print):
    n_ind = sum(1 for s in r['sites'] if s['kind'] == 'indirect')
    funcs = len({s['caller_addr'] for s in r['sites']})
    out(f'q callargs {r["func"]} {" ".join(r["regs"])}: {len(r["sites"]) - n_ind} direct call sites'
        + (f' + {n_ind} observed indirect' if n_ind else '') + f' in {funcs} functions'
        + ('' if r['indirect_known'] else '   (the observed-run index is not here: indirect callers NOT listed)'))
    for s in r['sites']:
        where = f'0x{s["site"]:08X}' if s['site'] else 'tail    '
        head = f'  {where} {s["caller"]}+0x{s["rel"]:X}' if s['rel'] is not None else f'  {where} {s["caller"]}'
        args = '  '.join(f'{k}={val_text(v)}' for k, v in s['args'].items())
        ctx = []
        if s['case']:
            c = s['case']
            ctx.append(f'case {"/".join(str(v) for v in c["values"])} of switch({c["selector"]} - {c["base"]})' if c['base'] else
                       f'case {"/".join(str(v) for v in c["values"])} of switch({c["selector"]})')
        if s['screen']:
            ctx.append(f'screen {s["screen"]}')
        elif s['not_screen']:
            ctx.append('screen not ' + ', '.join(str(v) for v in s['not_screen']))
        if s['kind'] == 'indirect':
            ctx.append('indirect (observed in runs)')
        elif s['kind'] == 'tail':
            ctx.append('tail')
        out(f'{head}  {args}' + (('   | ' + '; '.join(ctx)) if ctx else ''))
        if s['guards']:
            out('      guards: ' + ' && '.join(s['guards']))
        if s.get('note'):
            out('      ' + s['note'])
    if len(r['regs']) >= 1:
        reg = r['regs'][0]
        by = {}
        for s in r['sites']:
            a = s['args'][reg]
            key = val_text(a) if a['kind'] != 'unknown' else '?'
            by.setdefault(key, []).append(s)
        out(f'by {reg}:')
        for k in sorted(by, key=lambda x: (not x.lstrip('-').isdigit(), int(x) if x.lstrip('-').isdigit() else 0, x)):
            ss = by[k]
            out(f'  {reg} = {k}: ' + ', '.join(f'{x["caller"]}' + (f' 0x{x["site"]:08X}' if x['site'] else '') +
                                                 (f' [screen {x["screen"]}]' if x['screen'] else '') for x in ss))


def to_json(r):
    return {'func': r['func'], 'addr': f'0x{r["addr"]:08X}', 'regs': r['regs'], 'indirect_known': r['indirect_known'],
            'sites': [{'caller': s['caller'], 'site': f'0x{s["site"]:08X}' if s['site'] else None, 'kind': s['kind'],
                       'args': s['args'], 'case': s['case'], 'screen': s['screen'], 'not_screen': s['not_screen'],
                       'guards': s['guards']} for s in r['sites']]}


def main(argv, con):
    import argparse
    ap = argparse.ArgumentParser(prog='q callargs', description=__doc__.split('\n')[0])
    ap.add_argument('func')
    ap.add_argument('regs', nargs='+', help='registers (r4, r5, f1 ...)')
    ap.add_argument('--depth', type=int, default=3, help='hops of the slice into callers / callees for a non-constant value (default 3)')
    ap.add_argument('--budget', type=int, default=400, help='most chain nodes per site (default 400)')
    ap.add_argument('--screen-field', type=int, default=4, help='offset of the screen-code word in the controller (default 4: [in_r3+4])')
    ap.add_argument('--no-indirect', action='store_true', help='direct call sites only')
    ap.add_argument('--no-cache', action='store_true')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    for r in a.regs:
        if not qslice.REG_RE.match(r):
            raise CallArgsError(f'{r!r} is not a register (r4, f1 ...)')
    import q as qmod
    try:
        addr = qmod.resolve(con, a.func)[0]
    except Exception as ex:  # noqa: BLE001
        if type(ex).__name__ == 'QError':
            raise CallArgsError(str(ex))
        raise
    src = qslice.QSource(con, use_cache=not a.no_cache)
    res = callargs(src, addr, a.regs, a.depth, a.budget, a.screen_field, con=con, indirect=not a.no_indirect)
    if a.json:
        print(json.dumps(to_json(res), indent=1))
    else:
        print_result(res)
    return 0
