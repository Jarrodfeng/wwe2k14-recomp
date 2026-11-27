r"""qgstate_prune: drop the caller -> callee paths of q gstate --inherit / --leaks that cannot run (S111 GPRUNE; CIRCLES review:
sub_827E7338 calls the blob drawer only when in_r4 == 3 (`cmpwi cr6,r29,3` / `bne`, r29 = its r4), its only caller sub_827E8080
passes r4 = r30 in {0, 1}: the character node's alpha ref 128 reached the blobs through a branch that never runs).

For a call site (fn, site):
  requires(fn, site)   the conditions on fn's OWN argument registers that every run reaching the site satisfies: read up the chain of
                       single-predecessor blocks above the site's block, each conditional branch `cmpwi / cmplwi crN, rX, K` + beq / bne /
                       blt / bge / bgt / ble whose rX holds the entry value of an argument (`in_rN`, qslice's facts) gives (rN, op, K)
  values(c, off, reg)  the constants a caller's argument register can hold at its call at +off: qslice's call-site expression
                       (`c`, or the `phi` of constants, or a long `def` of either); anything else is None = unknown
  prune(fn, site, callers)  -> (kept, [record]): a caller whose known values satisfy no value of a required register is dropped
Unknown never prunes: a caller with an unknown register, an observed indirect caller without a call record, a block with several
predecessors above the site (the chain stops there), a compare of two registers, a jump table (not read: the facts keep no case value).
The condition is conjunctive: each register's values must satisfy ALL its conditions at once, per value.
"""
M32 = 0xFFFFFFFF
END = 1 << 30                   # qgstate_inherit's observed tail call marker (no site)
CHAIN_CAP = 24
RELS = {'beq': '==', 'bne': '!=', 'blt': '<', 'bge': '>=', 'bgt': '>', 'ble': '<='}
NEG = {'==': '!=', '!=': '==', '<': '>=', '>=': '<', '>': '<=', '<=': '>'}
CMPS = {'cmpwi': True, 'cmpw': True, 'cmplwi': False, 'cmplw': False}


def _s32(v):
    v &= M32
    return v - (1 << 32) if v & 0x80000000 else v


def sat(v, cond):
    """does the 32-bit value v satisfy (reg, op, K, signed)"""
    _r, op, k, signed = cond
    a, b = (_s32(v), _s32(k)) if signed else (v & M32, k & M32)
    return {'==': a == b, '!=': a != b, '<': a < b, '>=': a >= b, '>': a > b, '<=': a <= b}[op]


def cond_text(conds):
    return ' and '.join(f'{r} {op} {_s32(k) if signed else k & M32}' for r, op, k, signed in conds)


def val_text(vals):
    return '{' + ', '.join(str(_s32(v)) for v in sorted(vals, key=_s32)) + '}'


class Pruner:
    def __init__(self, src):
        self.src = src
        self._req = {}

    # ---------------------------------------------------------------- what fn needs at a site
    def requires(self, fn, site):
        key = (fn, site)
        if key not in self._req:
            try:
                self._req[key] = self._requires(fn, site)
            except (KeyError, TypeError, ValueError, IndexError):
                self._req[key] = []     # a listing the reader cannot follow prunes nothing
        return self._req[key]

    def _requires(self, fn, site):
        F = self.src.facts(fn)
        if F is None:
            return []
        ins = self.src.ins(F)
        a = F['addr'] + site
        blocks = F['blocks']
        cur = next((s for s, b in blocks.items() if s <= a < b['end']), None)
        out, seen = [], set()
        while cur is not None and cur not in seen and len(seen) < CHAIN_CAP:
            seen.add(cur)
            pr = blocks[cur]['preds']
            if len(pr) != 1:
                break
            p, kind = pr[0]
            pb = blocks.get(p)
            if pb is None:
                break
            t = pb['term']
            if t and t['k'] == 'cond':
                c = self._cond(F, ins, p, pb, kind)
                if c is not None:
                    out.append(c)
            cur = p
        return out

    def _cond(self, F, ins, p, pb, kind):
        rels = [r for r in ins if p <= F['addr'] + r < pb['end']]
        if not rels or kind not in ('br', 'fall'):
            return None
        last = ins[max(rels)]
        op = RELS.get(last['mn'].rstrip('+-'))
        if op is None:
            return None
        if kind == 'fall':
            op = NEG[op]
        bt = [x.strip() for x in last['ops'].split(',')]
        cr = bt[0] if bt[0].startswith('cr') and len(bt) > 1 else 'cr0'
        for r in sorted((r for r in rels), reverse=True):
            c = ins[r]
            if c['mn'] not in CMPS:
                continue
            t = [x.strip() for x in c['ops'].split(',')]
            ccr = t[0] if t[0].startswith('cr') else 'cr0'
            if ccr != cr:
                continue
            t = t[1:] if t[0].startswith('cr') else t
            if len(t) != 2 or not c['mn'].endswith('i'):
                return None
            e = c['b'].get(t[0])
            if not e or e[0] != 'in':
                return None
            return (e[1], op, int(t[1], 0), CMPS[c['mn']])
        return None

    # ---------------------------------------------------------------- what a caller passes
    def consts(self, F, e, d=0):
        if e is None or d > 8:
            return None
        k = e[0]
        if k == 'c':
            return {int(e[1]) & M32}
        if k == 'def' and not e[1].startswith('L'):
            return self.consts(F, F['defs'].get(f'{e[1]}@{e[2]:X}'), d + 1)
        if k == 'phi':
            ins = F['phis'].get(e[2], {}).get(e[1])
            if not ins:
                return None
            out = set()
            for p, v in ins:
                if v is None or (p is not None and p >= e[2]):
                    return None
                s = self.consts(F, v, d + 1)
                if s is None:
                    return None
                out |= s
            return out
        return None

    def values(self, c, off, reg):
        if off >= END:
            return None
        try:
            F = self.src.facts(c)
            call = F['calls'].get(off) if F else None
            if call is None or reg not in call['regs']:
                return None
            return self.consts(F, call['regs'][reg])
        except (KeyError, TypeError, ValueError):
            return None

    # ---------------------------------------------------------------- the filter
    def prune(self, fn, site, callers):
        """callers = [(caller, offset, how)] -> (kept, [record]) (a record: fn, site, caller, off, need, vals)"""
        req = self.requires(fn, site)
        if not req:
            return list(callers), []
        regs = sorted({c[0] for c in req})
        kept, recs = [], []
        for cl in callers:
            bad = None
            for r in regs:
                conds = [c for c in req if c[0] == r]
                vals = self.values(cl[0], cl[1], r)
                if vals is not None and not any(all(sat(v, c) for c in conds) for v in vals):
                    bad = (conds, vals)
                    break
            if bad is None:
                kept.append(cl)
            else:
                recs.append({'fn': fn, 'site': site, 'caller': cl[0], 'off': cl[1], 'need': cond_text(bad[0]), 'vals': sorted(bad[1], key=_s32)})
        return kept, recs


def footer(recs, name=lambda a: f'sub_{a:08X}'):
    """the `pruned:` line: how many caller edges were dropped, and per (function, need) the values its callers pass"""
    if not recs:
        return None
    groups = {}
    for r in recs:
        g = groups.setdefault((r['fn'], r['site'], r['need']), {'vals': set(), 'n': 0})
        g['vals'] |= set(r['vals'])
        g['n'] += 1
    parts = [f'{name(fn)} needs {need} at +0x{site:X}; callers pass {val_text(g["vals"])}' for (fn, site, need), g in groups.items()]
    return f'pruned: {len(recs)} path(s) (' + '; '.join(parts) + ')'
