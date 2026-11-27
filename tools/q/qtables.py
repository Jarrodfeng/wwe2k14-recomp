"""q tables FUNC [--json] [--min-chain N] [--len N]   (S111 JTABLE): every dispatch structure of one function, as index -> target -> what the target does first.

Four forms, read from the generated listing (qdecomp.read_listing, so the `switch` the generator already resolved) and the image:
  jump     a `bctr` with its resolved cases: the selector (the register the `cmplwi` bounds check compares, followed back through `addi rD,rS,-K` / `mr`
           to the load that defines it), the case-value base K (value = index + K, as q callargs reads it), the table address (the `lis`/`addi` + `lwzx`/`lhzx`
           in front of the `bctr`), and one row per entry. When the selector is itself loaded from a byte / half / word table (`lbzx rD,rT,rI`), that table is
           printed too: table index -> loaded value -> case -> target.
  chain    3 or more consecutive `cmpwi/cmplwi rX,K` + `beq` on the same register: K -> target; the arm after the run is the default.
  bdzf     a run of `bdzf 4*crN+eq,T` after `mtctr rD` + `cmpwi crN,rD,0` (the counter test chain): the k-th branch is taken when rD == k, the fall-through
           after the run (past a closing `bne` / `beq`) is rD == 0; the `addi rD,rS,-K` before it gives the case-value base.
  table    an indexed load from a constant table (`lis`/`addi` base, `lbzx` / `lhzx` / `lwzx`, or `add` + `lwz`) whose value then feeds a call through the
           counter (`mtctr` + `bctrl`) or a compare + branch, and is not a jump table's own table: the entries read from the image (index -> word -> what a
           function / in-function address does first). The length is the bound of the index register when a `cmplwi` + `bgt` guards it, else --len.
"What the target does first": the first store of a non-stack slot (constant or register), call (`calls sub_X(r3=K ..)` with the constants known at that
point), indirect call, return (`returns r3=K`) or tail branch on the straight-line walk from the target (a `b` inside the function is followed, a conditional
branch is walked past; at most 40 instructions). Register names in selectors are the registers AT that point of the listing, not the entry arguments.
The unit is tools\\q\\qtables_selftest.py (planted listings for each form + a twin each).
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qdecomp  # noqa: E402

M32 = 0xFFFFFFFF
WALK_MAX = 40
MAX_EVENTS = 3            # a target's summary: up to this many first stores / calls, ending at a return / tail
DUMP_EVENTS = 6           # `q vtable --dump`: a slot is a short function, show more of it
NOISE_CALLS = {0x82C2D8C0, 0x82C2DE28}     # the thread-table get / set pair most functions open with: no information about the target
IMG_LO, IMG_HI = 0x82000000, 0x84000000
COND_RE = re.compile(r'^b(eq|ne|lt|gt|le|ge|nl|ng|so|ns|un|nu)(lr|ctr)?[+-]?$')
CMP_IMM = ('cmpwi', 'cmplwi', 'cmpdi', 'cmpldi')
LOADW = {'lbzx': 1, 'lhzx': 2, 'lwzx': 4}
LOADD = {'lbz': 1, 'lhz': 2, 'lwz': 4}
ALIAS_MN = ('mr', 'rlwinm', 'slwi', 'clrlwi', 'rotlwi', 'extsb', 'extsh', 'extsw')
WIDTH_TXT = {1: 'u8', 2: 'u16', 4: 'u32'}


class TablesError(Exception):
    pass


def num(s):
    try:
        return int(s, 0)
    except (ValueError, TypeError):
        return None


def fk(v):
    v &= M32
    if v >= 0xFFFF0000:
        return str(v - (1 << 32))
    return str(v) if v < 1024 else f'{v} (0x{v:X})'


def reg_off(reg, off):
    return f'{reg}{off:+d}'


def ops_of(ins):
    return qdecomp.split_ops(ins.ops)


def dest(ins):
    if ins.mn.startswith(qdecomp.NO_DEST):
        return None
    o = ops_of(ins)
    return o[0] if o and qdecomp.REG_RE.match(o[0]) else None


def target_of(ins):
    o = ops_of(ins)
    if o and o[-1].startswith('0x'):
        return int(o[-1], 16)
    return None


def is_cond_branch(mn):
    return (bool(COND_RE.match(mn)) and not mn.endswith(('lr', 'ctr'))) or mn.startswith(('bdz', 'bdnz'))


def is_cond_return(mn):
    return bool(COND_RE.match(mn)) and mn.endswith('lr')


def const_update(regs, ins, ops):
    """the constant-register map after `ins` (li / lis / ori / oris / addi / addis / mr); any other write forgets its destination"""
    mn, d = ins.mn, dest(ins)
    if mn == 'bl' or mn == 'bctrl':
        for r in ['r0'] + [f'r{i}' for i in range(3, 13)]:
            regs.pop(r, None)
        return
    if d is None:
        return
    v = None
    if mn == 'li' and len(ops) == 2 and num(ops[1]) is not None:
        v = num(ops[1]) & M32
    elif mn == 'lis' and len(ops) == 2 and num(ops[1]) is not None:
        v = (num(ops[1]) << 16) & M32
    elif mn in ('ori', 'oris', 'addi', 'addis') and len(ops) == 3 and ops[1] in regs and num(ops[2]) is not None:
        k = num(ops[2])
        k = k << 16 if mn.endswith('is') else k
        v = ((regs[ops[1]] | (k & M32)) if mn.startswith('or') else (regs[ops[1]] + k)) & M32
    elif mn in ('mr', 'rotlwi') and len(ops) >= 2 and ops[1] in regs and (mn == 'mr' or num(ops[2]) == 0):
        v = regs[ops[1]]
    if v is None:
        regs.pop(d, None)
    else:
        regs[d] = v


class Ctx:
    def __init__(self, insns, labels, image, name=None, disp=None, min_chain=3, default_len=16):
        self.insns, self.labels, self.image = insns, labels, image
        self.pos = {i.addr: k for k, i in enumerate(insns)}
        self.name = name or (lambda a: None)
        self.disp = disp or (lambda a: f'sub_{a:08X}')
        self.min_chain, self.default_len = min_chain, default_len
        self.start = insns[0].addr
        self.loads = {}          # insn index -> an indexed load from a constant table
        self.effects = {}
        self.max_events = MAX_EVENTS
        self.scan_loads()

    # ---------------------------------------------------------------- image
    def read(self, a, width):
        d = getattr(self.image, 'data', None)
        base = getattr(self.image, 'base', qdecomp.IMAGE_BASE)
        if d is None or a < base or a + width > base + len(d):
            return None
        return int.from_bytes(d[a - base:a - base + width], 'big')

    # ---------------------------------------------------------------- constant tables
    def scan_loads(self):
        regs, tabs = {}, {}
        for k, ins in enumerate(self.insns):
            if k and ins.addr in self.labels:
                regs.clear()
                tabs.clear()
            mn, ops = ins.mn, ops_of(ins)
            if mn in LOADW and len(ops) == 3:
                for b, ix in ((ops[1], ops[2]), (ops[2], ops[1])):
                    t = regs.get(b)
                    if t is not None and IMG_LO <= t < IMG_HI:
                        self.loads[k] = {'table': t, 'width': LOADW[mn], 'idx': ix, 'dest': ops[0], 'mn': mn}
                        break
            elif mn in LOADD and len(ops) == 2:
                m = qdecomp.MEM_RE.match(ops[1])
                if m and m.group(2) in tabs:
                    t, ix = tabs[m.group(2)]
                    self.loads[k] = {'table': t + int(m.group(1), 0), 'width': LOADD[mn], 'idx': ix, 'dest': ops[0], 'mn': mn}
            d = dest(ins)
            if mn == 'add' and len(ops) == 3:
                hit = None
                for b, ix in ((ops[1], ops[2]), (ops[2], ops[1])):
                    if regs.get(b) is not None and IMG_LO <= regs[b] < IMG_HI:
                        hit = (regs[b], ix)
                        break
                regs.pop(ops[0], None)
                tabs.pop(ops[0], None)
                if hit:
                    tabs[ops[0]] = hit
            else:
                const_update(regs, ins, ops)
                if d:
                    tabs.pop(d, None)

    # ---------------------------------------------------------------- selector / bound
    def selector(self, k, reg):
        """walk back from insn k through `addi` / `mr` copies of reg -> (reg at the definition, base, defining insn index or None);
        the value of the defining register = index + base"""
        base = 0
        for j in range(k - 1, max(k - 40, -1), -1):
            ins = self.insns[j]
            if dest(ins) != reg:
                continue
            o = ops_of(ins)
            if ins.mn in ('addi', 'subi') and len(o) == 3 and num(o[2]) is not None:
                base += -num(o[2]) if ins.mn == 'addi' else num(o[2])
                reg = o[1]
                continue
            if ins.mn == 'mr' and len(o) == 2:
                reg = o[1]
                continue
            return reg, base, j
        return reg, base, None

    def sel_text(self, reg, defi):
        if defi is None:
            return reg
        ins = self.insns[defi]
        if defi in self.loads:
            ld = self.loads[defi]
            return f'{WIDTH_TXT[ld["width"]]} table 0x{ld["table"]:08X}[{ld["idx"]}]'
        o = ops_of(ins)
        if ins.mn in LOADD and len(o) == 2:
            m = qdecomp.MEM_RE.match(o[1])
            if m:
                return ('' if ins.mn == 'lwz' else WIDTH_TXT[LOADD[ins.mn]] + ' ') + f'[{reg_off(m.group(2), int(m.group(1), 0))}]'
        return f'{reg} ({ins.mn} {ins.ops})'

    def bound_of(self, k, idx):
        """-> the number of entries the guard `cmplwi rI,M` + `bgt` in front of insn k allows for index register idx, or None"""
        regs = {idx}
        for j in range(k - 1, max(k - 30, -1), -1):
            ins = self.insns[j]
            o = ops_of(ins)
            if ins.mn in CMP_IMM and len(o) == 3 and o[1] in regs and j + 1 < len(self.insns):
                m = self.insns[j + 1].mn.rstrip('+-')
                n = num(o[2])
                if n is None:
                    return None
                if m in ('bgt', 'bgtlr', 'ble'):
                    return n + 1
                if m in ('bge', 'bgelr', 'blt'):
                    return n
                return None
            if ins.mn in ALIAS_MN and len(o) >= 2 and o[0] in regs:
                regs.add(o[1])
        return None

    # ---------------------------------------------------------------- what a target does first
    def first_effect(self, start):
        if start in self.effects:
            return self.effects[start]
        r = self._effect(start)
        self.effects[start] = r
        return r

    def _effect(self, start):
        regs, defs, ev = {}, {}, []
        a, hops, n, past = start, 0, 0, ''

        def done(final=''):
            if final:
                ev.append(final)
            return '; '.join(ev) if ev else 'no store / call / return within the walk'
        while n < WALK_MAX and len(ev) < self.max_events:
            k = self.pos.get(a)
            if k is None:
                return done(f'leaves the function at 0x{a:08X}')
            ins = self.insns[k]
            n += 1
            mn, ops = ins.mn, ops_of(ins)

            def args():
                got = [f'{r}={fk(regs[r])}' for r in ('r3', 'r4', 'r5', 'r6') if r in regs]
                return '(' + ', '.join(got) + ')' if got else ''
            if mn == 'bl':
                t = target_of(ins)
                nm = self.name(t) or ''
                if not nm.startswith('__') and t not in NOISE_CALLS:
                    ev.append(f'calls {self.disp(t)}{args()}{past}')
            elif mn == 'bctrl':
                slot = ''
                c = defs.get('ctr')
                if c and defs.get(c[0]) and defs[c[0]][1] == 0:
                    slot = f' (vcall slot +{c[1]})'
                ev.append(f'indirect call{slot}{past}')
            elif mn == 'blr':
                return done('returns' + (f' r3={fk(regs["r3"])}' if 'r3' in regs else '') + past)
            elif mn == 'bctr':
                return done('jumps through the counter' + past)
            elif mn == 'b':
                t = target_of(ins)
                if t in self.pos:
                    hops += 1
                    if hops > 8:
                        return done()
                    a = t
                    continue
                nm = self.name(t) or ''
                if nm.startswith('__rest'):
                    return done('returns' + (f' r3={fk(regs["r3"])}' if 'r3' in regs else '') + past)
                return done(f'tail {self.disp(t)}{args()}{past}')
            elif is_cond_branch(mn) or is_cond_return(mn):
                past = ' (after a branch)'
            elif mn in qdecomp.STORES and ops:
                m = qdecomp.MEM_RE.match(ops[-1]) if len(ops) == 2 else None
                if m and m.group(2) != 'r1':
                    v = fk(regs[ops[0]]) if ops[0] in regs else ops[0]
                    ev.append(f'{mn} [{reg_off(m.group(2), int(m.group(1), 0))}] = {v}{past}')
                elif not m and len(ops) == 3 and ops[1] != 'r1':
                    ev.append(f'{mn} [{ops[1]}+{ops[2]}] = {ops[0]}{past}')
            elif mn == 'lwz' and len(ops) == 2:
                m = qdecomp.MEM_RE.match(ops[1])
                if m:
                    defs[ops[0]] = (m.group(2), int(m.group(1), 0))
            elif mn == 'mtctr' and ops and ops[0] in defs:
                defs['ctr'] = defs[ops[0]]
            const_update(regs, ins, ops)
            d = dest(ins)
            if d and mn != 'lwz':
                defs.pop(d, None)
            a += 4
        return done()

    def entry_effect(self, t):
        if t in self.pos:
            return self.first_effect(t)
        nm = self.name(t)
        return f'-> {self.disp(t)}' if nm else ''

    # ---------------------------------------------------------------- the four forms
    def rows(self, pairs):
        """pairs = [(label, target)] -> row dicts"""
        return [{'label': lb, 'target': t, 'effect': self.entry_effect(t) if t is not None else ''} for lb, t in pairs]

    def default_row(self, ins):
        if ins is None:
            return None
        t = target_of(ins)
        if ins.mn.rstrip('+-') in ('bgt', 'bge', 'ble', 'blt') and t is not None:
            return {'label': 'default', 'target': t, 'effect': self.entry_effect(t)}
        if ins.mn.endswith('lr') and is_cond_return(ins.mn):
            return {'label': 'default', 'target': None, 'effect': 'returns'}
        return None

    def find_jumps(self, linked):
        out = []
        for k, ins in enumerate(self.insns):
            if ins.mn != 'bctr' or not ins.cases:
                continue
            kc = next((j for j in range(k - 1, max(k - 40, -1), -1) if self.insns[j].mn in ('cmplwi', 'cmpwi') and len(ops_of(self.insns[j])) == 3), None)
            reg, base, defi, sel = None, 0, None, '?'
            default = None
            if kc is not None:
                reg0 = ops_of(self.insns[kc])[1]
                reg, base, defi = self.selector(kc, reg0)
                sel = self.sel_text(reg, defi) if defi is not None else reg
                default = self.default_row(self.insns[kc + 1] if kc + 1 < len(self.insns) else None)
            own = next((j for j in range(k - 1, max(k - 16, -1), -1) if j in self.loads and j != defi), None)
            if own is not None:
                linked.add(own)
            cases = ins.cases
            s = {'kind': 'jump', 'at': ins.addr - self.start, 'selector': sel, 'base': base, 'default': default,
                 'table': f'0x{self.loads[own]["table"]:08X} ({WIDTH_TXT[self.loads[own]["width"]]} x{len(cases)})' if own is not None else None,
                 'entries': len(cases), 'distinct': len(set(cases)),
                 'rows': self.rows([(str(i + base), t) for i, t in enumerate(cases)])}
            if defi is not None and defi in self.loads and defi not in linked:
                linked.add(defi)
                ld = self.loads[defi]
                n = self.bound_of(defi, ld['idx'])
                guess = n is None
                n = n if n is not None else self.default_len
                pre = []
                for i in range(n):
                    v = self.read(ld['table'] + i * ld['width'], ld['width'])
                    if v is None:
                        continue
                    ci = v - base
                    t = cases[ci] if 0 <= ci < len(cases) else (default['target'] if default else None)
                    pre.append((f'[{i}] = {v}  -> case {ci if 0 <= ci < len(cases) else "default"}', t))
                s['pretable'] = {'table': f'0x{ld["table"]:08X} ({WIDTH_TXT[ld["width"]]} x{n}{", length guessed: --len" if guess else ""})',
                                 'index': ld['idx'], 'rows': self.rows(pre)}
            out.append(s)
        return out

    def cmp_beq(self, k):
        """(reg, K, target) when insn k is `cmp(l)wi [crN,]rX,K` and k+1 is `beq [crN,]T`"""
        if k + 1 >= len(self.insns):
            return None
        c, b = self.insns[k], self.insns[k + 1]
        if c.mn not in CMP_IMM or b.mn.rstrip('+-') != 'beq':
            return None
        co, bo = ops_of(c), ops_of(b)
        if len(co) == 3:
            cr, reg, kk = co
        elif len(co) == 2:
            cr, reg, kk = 'cr0', co[0], co[1]
        else:
            return None
        if (bo[0] if len(bo) == 2 else 'cr0') != cr or num(kk) is None or target_of(b) is None:
            return None
        return reg, num(kk), target_of(b)

    def find_chains(self):
        out, i, n = [], 0, len(self.insns)
        while i < n - 1:
            e = self.cmp_beq(i)
            if not e:
                i += 1
                continue
            run, j = [e], i + 2
            while j < n - 1:
                e2 = self.cmp_beq(j)
                if not e2 or e2[0] != e[0]:
                    break
                run.append(e2)
                j += 2
            if len(run) < self.min_chain:
                i += 1
                continue
            reg, base, defi = self.selector(i, e[0])
            nxt = self.insns[j] if j < n else None
            dflt = None
            if nxt is not None:
                t = target_of(nxt) if nxt.mn == 'b' else None
                dflt = {'label': 'otherwise', 'target': t if t is not None else nxt.addr, 'effect': ''}
                dflt['effect'] = self.entry_effect(dflt['target'])
            out.append({'kind': 'chain', 'at': self.insns[i].addr - self.start, 'selector': self.sel_text(reg, defi), 'base': base,
                        'default': dflt, 'table': None, 'entries': len(run), 'distinct': len({t for _r, _k, t in run}),
                        'rows': self.rows([(str(kk + base), t) for _r, kk, t in run])})
            i = j
        return out

    def find_bdzf(self):
        out, k, n = [], 0, len(self.insns)
        while k < n:
            ins = self.insns[k]
            if not (ins.mn == 'bdzf' and target_of(ins) is not None):
                k += 1
                continue
            j = k
            run = []
            while j < n and self.insns[j].mn == 'bdzf' and target_of(self.insns[j]) is not None:
                run.append(target_of(self.insns[j]))
                j += 1
            crn = re.match(r'4\*(cr\d)\+eq', ops_of(ins)[0])
            mt = next((m for m in range(k - 1, max(k - 8, -1), -1) if self.insns[m].mn == 'mtctr'), None)
            reg = ops_of(self.insns[mt])[0] if mt is not None else None
            cmp_ok = crn is not None and reg is not None and any(
                self.insns[m].mn in CMP_IMM and ops_of(self.insns[m])[:3] == [crn.group(1), reg, '0'] for m in range(k - 1, max(k - 8, -1), -1))
            base, sel = 0, reg or '?'
            if reg is not None:
                r2, base, defi = self.selector(mt, reg)
                sel = self.sel_text(r2, defi) if defi is not None else r2
            pairs = [(str(i + 1 + base), t) for i, t in enumerate(run)]
            nxt = self.insns[j] if j < n else None
            zero_t, other = j and (self.insns[j - 1].addr + 4), None
            if nxt is not None and nxt.mn.rstrip('+-') in ('bne', 'beq') and target_of(nxt) is not None:
                if nxt.mn.startswith('bne'):
                    other, zero_t = target_of(nxt), nxt.addr + 4
                else:
                    zero_t, other = target_of(nxt), nxt.addr + 4
            pairs.insert(0, (str(base), zero_t))
            out.append({'kind': 'bdzf', 'at': ins.addr - self.start, 'selector': sel, 'base': base,
                        'default': ({'label': 'other', 'target': other, 'effect': self.entry_effect(other)} if other is not None else None),
                        'table': None, 'entries': len(pairs), 'distinct': len({t for _l, t in pairs}),
                        'note': None if cmp_ok else 'no `cmpwi crN,rD,0` + `mtctr rD` found in front of the run: the shape is the counter chain, the selector is a guess',
                        'rows': self.rows(pairs)})
            k = j
        return out

    def find_tables(self, linked):
        out = []
        for k, ld in sorted(self.loads.items()):
            if k in linked:
                continue
            use, S = None, {ld['dest']}
            for j in range(k + 1, min(k + 14, len(self.insns))):
                ins = self.insns[j]
                o = ops_of(ins)
                if ins.mn == 'mtctr' and o and o[0] in S:
                    use = 'call' if any(self.insns[m].mn == 'bctrl' for m in range(j + 1, min(j + 4, len(self.insns)))) else None
                    break
                if ins.mn.startswith('cmp') and any(x in S for x in o):
                    use = 'compare'
                    break
                if ins.mn in ALIAS_MN + ('addi',) and len(o) >= 2 and o[1] in S:
                    S.add(o[0])
                elif dest(ins) in S:
                    break
            if use is None:
                continue
            n = self.bound_of(k, ld['idx'])
            guess = n is None
            n = n if n is not None else self.default_len
            pairs = []
            for i in range(n):
                v = self.read(ld['table'] + i * ld['width'], ld['width'])
                if v is None:
                    continue
                pairs.append((f'[{i}] = {v}' if ld['width'] < 4 else f'[{i}] = 0x{v:08X}', v if (ld['width'] == 4 and use == 'call') else None))
            rows = [{'label': lb, 'target': t, 'effect': self.entry_effect(t) if t is not None else ''} for lb, t in pairs]
            out.append({'kind': 'table', 'at': self.insns[k].addr - self.start, 'selector': ld['idx'], 'base': 0, 'default': None,
                        'table': f'0x{ld["table"]:08X} ({WIDTH_TXT[ld["width"]]} x{n}{", length guessed: --len" if guess else ""})',
                        'use': use, 'entries': len(rows), 'distinct': len({r['label'].split('= ')[-1] for r in rows}), 'rows': rows})
        return out


def analyse(insns, labels, image, name=None, disp=None, min_chain=3, default_len=16):
    """-> the dispatch structures of one function listing, in address order"""
    ctx = Ctx(insns, labels, image, name, disp, min_chain, default_len)
    linked = set()
    res = ctx.find_jumps(linked) + ctx.find_chains() + ctx.find_bdzf() + ctx.find_tables(linked)
    return sorted(res, key=lambda s: s['at'])


# ------------------------------------------------------------------ printing
def merge_rows(rows):
    """consecutive rows with the same target and effect print as one line `a..b`"""
    out = []
    for r in rows:
        if out and r['target'] == out[-1]['target'] and r['effect'] == out[-1]['effect'] and out[-1]['label'][:1] != '[':
            out[-1]['hi'] = r['label']
        else:
            out.append(dict(r))
    return out


def row_text(r):
    lab = r['label'] + (f'..{r["hi"]}' if r.get('hi') else '')
    tgt = f'0x{r["target"]:08X}' if r['target'] is not None else '-'
    return f'    {lab:>12}  -> {tgt}  {r["effect"]}'.rstrip()


def print_result(res, out=print):
    out(f'q tables {res["func"]}  0x{res["addr"]:08X}: {len(res["structures"])} dispatch structures')
    for n, s in enumerate(res['structures'], 1):
        head = f'[{n}] {s["kind"]} at +0x{s["at"]:X}  selector {s["selector"]}'
        if s['kind'] in ('jump', 'chain', 'bdzf'):
            head += f'  value = index + {s["base"]}' if s['base'] else '  value = index'
        head += f'  {s["entries"]} entries, {s["distinct"]} distinct targets'
        if s.get('use'):
            head += f'  (feeds a {s["use"]})'
        out(head)
        if s.get('table'):
            out(f'    table {s["table"]}')
        if s.get('note'):
            out(f'    note: {s["note"]}')
        for r in merge_rows(s['rows']):
            out(row_text(r))
        if s.get('default'):
            out(row_text(s['default']))
        if s.get('pretable'):
            p = s['pretable']
            out(f'    selector table {p["table"]} indexed by {p["index"]}:')
            for r in merge_rows(p['rows']):
                out(row_text(r))
    if not res['structures']:
        out('  none found (no resolved bctr switch, no 3-compare beq chain, no bdzf run, no indexed constant table feeding a call / compare)')


def to_json(res):
    def cl(r):
        return {k: (f'0x{v:08X}' if k == 'target' and v is not None else v) for k, v in r.items()}
    st = []
    for s in res['structures']:
        d = dict(s)
        d['rows'] = [cl(r) for r in s['rows']]
        d['default'] = cl(s['default']) if s.get('default') else None
        if s.get('pretable'):
            d['pretable'] = dict(s['pretable'], rows=[cl(r) for r in s['pretable']['rows']])
        st.append(d)
    return {'func': res['func'], 'addr': f'0x{res["addr"]:08X}', 'structures': st}


# ------------------------------------------------------------------ the verb
def image():
    if qdecomp._IMG is None:
        qdecomp._IMG = qdecomp.Image()
    return qdecomp._IMG


def main(argv, con):
    import argparse
    ap = argparse.ArgumentParser(prog='q tables', description=__doc__.split('\n')[0])
    ap.add_argument('func')
    ap.add_argument('--min-chain', type=int, default=3, help='fewest `cmpwi rX,K` + `beq` pairs that make a compare chain (default 3)')
    ap.add_argument('--len', type=int, default=16, dest='length', help='entries to read of a constant table with no index guard (default 16)')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    import q as qmod
    try:
        addr = qmod.resolve(con, a.func)[0]
    except Exception as ex:  # noqa: BLE001
        if type(ex).__name__ == 'QError':
            raise TablesError(str(ex))
        raise
    f = con.execute('SELECT * FROM funcs WHERE addr = ?', (addr,)).fetchone()
    if f is None or not f['file']:
        raise TablesError(f'0x{addr:08X} has no generated body')
    insns, labels, _a, _b = qdecomp.read_listing(os.path.join(qdecomp.GEN, f['file']), f['line'], f['name'], addr)
    if not insns:
        raise TablesError(f'{f["name"]}: no instructions parsed')
    try:
        dbi = qdecomp.db_index()
    except Exception:  # noqa: BLE001 - names are a convenience
        dbi = {}

    def disp(x):
        e = dbi.get(x)
        return f'sub_{x:08X}' + (f' ({e["name"]})' if e and e.get('name') else '')
    res = {'func': f['name'], 'addr': addr,
           'structures': analyse(insns, labels, image(), lambda x: qdecomp._lookup(con, x), disp, a.min_chain, a.length)}
    if a.json:
        print(json.dumps(to_json(res), indent=1))
    else:
        print_result(res)
    return 0


# ------------------------------------------------------------------ q vtable --dump (the same first-effect summary, per slot)
def func_effect(con, addr, img, disp=None):
    """what the function at addr does first (the walk `q tables` uses for a target), or a short reason it cannot be read"""
    f = con.execute('SELECT * FROM funcs WHERE addr = ?', (addr,)).fetchone()
    if f is None or not f['file']:
        return '(no generated body)'
    try:
        insns, labels, _a, _b = qdecomp.read_listing(os.path.join(qdecomp.GEN, f['file']), f['line'], f['name'], addr)
    except (qdecomp.DecompError, OSError) as ex:
        return f'({ex})'
    if not insns:
        return '(no instructions)'
    ctx = Ctx(insns, labels, img, lambda x: qdecomp._lookup(con, x), disp)
    ctx.max_events = DUMP_EVENTS
    return ctx.first_effect(addr)


def dump_vtable(con, img, base, limit, names=None):
    """-> {'vtable', 'table_slots', 'slots': [{'slot','offset','target','target_addr','db_name','first'}]} (limit slots)"""
    import qvtable
    tbl = qvtable.read_table(con, img, base)
    if not tbl:
        raise qvtable.VtError(f'0x{base:08X}: not a vtable (the first word is not a function start)')
    names = names or {}

    def disp(x):
        return f'sub_{x:08X}' + (f' ({names[x]})' if x in names else '')
    rows = [{'slot': s, 'offset': s * 4, 'target': qvtable.fname(con, t), 'target_addr': f'0x{t:08X}', 'db_name': names.get(t),
             'first': func_effect(con, t, img, disp)} for s, _at, t in tbl[:limit]]
    return {'vtable': f'0x{base:08X}', 'table_slots': len(tbl), 'slots': rows}
