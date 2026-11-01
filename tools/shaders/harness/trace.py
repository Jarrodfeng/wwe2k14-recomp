#!/usr/bin/env python3
"""Scratch tool: symbolic scalar (SSA) trace of an Xenos pixel-shader disassembly (Xenia-style listing).
Emits GLSL-ish scalar statements for every written register component, dead code removed."""
import re, sys

path = sys.argv[1]
lines = open(path, encoding='utf-8', errors='replace').read().splitlines()

# ---- parse into instructions: (idx, kind, vec_text, sca_text, pred_clause)
instrs = []   # dict(idx, kind, pred, vec, sca, text)
cur = None
pending_idx = None
exec_pred = None
for ln in lines:
    s = ln.strip()
    if not s:
        continue
    m = re.match(r'/\*\s*([\d.]+)\s*\*/\s*(.*)$', s)
    if m:
        num, rest = m.group(1), m.group(2)
        if '.' in num:   # clause header like 3.1
            if rest.startswith('alloc'):
                continue
            em = re.match(r'(\((!?)p0\)\s*)?exec', rest)
            exec_pred = None
            if em and em.group(1):
                exec_pred = ('!' if em.group(2) else '') + 'p0'
            continue
        idx = int(num)
        if rest.startswith('serialize'):
            cur = dict(idx=idx, kind='alu', pred=None, vec=None, sca=None, lines=[])
            instrs.append(cur)
            continue
        fm = re.match(r'(?:\((!?)p0\)\s*)?(tfetch2D|tfetchCube)\s+(.*)$', rest)
        if fm:
            pr = ('!' if fm.group(1) else '') + 'p0' if '(' in rest.split()[0] else None
            instrs.append(dict(idx=idx, kind='fetch', op=fm.group(2), pred=pr, text=fm.group(3)))
            cur = None
            continue
        # alu line with vector op
        cur = dict(idx=idx, kind='alu', pred=None, vec=rest, sca=None, lines=[])
        instrs.append(cur)
        continue
    if s.startswith('+'):
        cur['sca'] = s[1:].strip()
        continue
    # continuation: vector op after serialize
    if cur is not None and cur['vec'] is None:
        cur['vec'] = s
        continue
    print('UNPARSED', s, file=sys.stderr)

# ---- machine state
reg = {}      # (r, c) -> expr string
def getreg(r, c):
    return reg.get((r, c), f'in{r}{"xyzw"[c]}')
cnum = {}     # constants referenced
ps = '0.0'
emitted = []  # (name, expr)
names = set()
uses = {}

def mk(name_base, idx, expr):
    name = f'{name_base}_{idx}'
    n = name; k = 2
    while n in names:
        n = f'{name}b{k}'; k += 1
    names.add(n)
    emitted.append((n, expr))
    return n

def parse_src(tok):
    tok = tok.strip()
    m = re.match(r'(-)?(?:(r|c)(\d+)|r_abs\[(\d+)\])(?:\.([xyzw]+))?$', tok)
    if not m:
        raise Exception('bad operand ' + tok)
    neg = bool(m.group(1))
    absv = m.group(4) is not None
    if m.group(2):
        kind, num = m.group(2), int(m.group(3))
    else:
        kind, num = 'r', int(m.group(4))
    sw = m.group(5) or 'xyzw'
    return neg, absv, kind, num, sw

def comp_expr(neg, absv, kind, num, c):
    if kind == 'r':
        e = getreg(num, c)
    else:
        e = f'c{num}.{"xyzw"[c]}'
    if absv:
        e = f'abs({e})'
    if neg:
        e = f'-{e}'
    return e

def vec_operand(tok, idx_ncomp=4):
    neg, absv, kind, num, sw = parse_src(tok)
    out = []
    for i in range(4):
        ch = sw[i] if i < len(sw) else sw[-1]
        c = 'xyzw'.index(ch)
        out.append(comp_expr(neg, absv, kind, num, c))
    return out

def sca_operand(tok):
    neg, absv, kind, num, sw = parse_src(tok)
    res = []
    for ch in sw:
        res.append(comp_expr(neg, absv, kind, num, 'xyzw'.index(ch)))
    return res

def parse_dst(tok):
    tok = tok.strip()
    if tok.startswith('oC'):
        return ('o', int(tok[2:]), [0, 1, 2, 3])
    m = re.match(r'r(\d+)(?:\.([xyzw_]+))?$', tok)
    r = int(m.group(1))
    mask = m.group(2)
    if mask is None:
        return ('r', r, [0, 1, 2, 3])
    if mask == '_':
        return ('r', r, [])
    comps = []
    for i, ch in enumerate(mask):
        if ch != '_':
            comps.append(i)
    return ('r', r, comps)

def split_args(text):
    return [t.strip() for t in text.split(',')]

def p_clause(pred):
    return {'p0': 'p0', '!p0': '!p0'}.get(pred)

def cond(pred):
    if pred is None: return None
    return 'p0' if pred == 'p0' else '!p0'

pred_now = 'false'  # symbolic predicate expression name 'p0' variable

def wrap_pred(new, old, pred):
    if pred is None:
        return new
    return f'({cond(pred)} ? {new} : {old})'

final_out = None

TEX_DEST = {}

for ins in instrs:
    idx = ins['idx']
    if ins['kind'] == 'fetch':
        args = split_args(re.sub(r',\s*(FetchValidOnly|UseComputedLOD)=\w+', '', ins['text']))
        dst, coord, tf = args[0], args[1], args[2]
        kind, r, comps = parse_dst(dst)
        # dest swizzle letters = source texture channels
        m = re.match(r'r(\d+)(?:\.([xyzw_]+))?$', dst)
        dsw = m.group(2) or 'xyzw'
        cm = re.match(r'r(\d+)\.([xyzw]+)$', coord)
        cr = int(cm.group(1)); csw = cm.group(2)
        carg = ','.join(getreg(cr, 'xyzw'.index(ch)) for ch in csw)
        lod = ' /*lod0*/' if 'UseComputedLOD=false' in ins['text'] else ''
        kindname = 'tex2D' if ins['op'] == 'tfetch2D' else 'texCube'
        for i, ch in enumerate(dsw):
            if ch == '_':
                continue
            expr = f'{kindname}_{tf}_{ch}({carg}){lod}'
            old = getreg(r, i)
            if ins['pred']:
                expr = f'({cond(ins["pred"])} ? {expr} : {old})'
            reg[(r, i)] = mk(f'r{r}{"xyzw"[i]}', idx, expr)
        continue

    vec = ins['vec']; sca = ins['sca']
    # pred
    vpred = None
    pm = re.match(r'\((!?)p0\)\s*(.*)$', vec)
    if pm:
        vpred = ('!' if pm.group(1) else '') + 'p0'; vec = pm.group(2)
    spred = None
    if sca:
        pm = re.match(r'\((!?)p0\)\s*(.*)$', sca)
        if pm:
            spred = ('!' if pm.group(1) else '') + 'p0'; sca = pm.group(2)

    pending = []    # list of (reg, comp, expr, pred) writes committed after both ops
    pending_ps = None
    new_p = None
    # --- vector op
    vop, vargs = vec.split(None, 1)
    vargs = split_args(vargs)
    dkind, dr, dcomps = parse_dst(vargs[0])
    def V(i):
        return vec_operand(vargs[1 + i])
    res = [None] * 4
    if vop in ('mul', 'add', 'max', 'min', 'mad', 'mul_sat', 'mad_sat', 'add_sat'):
        a = V(0)
        b = V(1) if vop != 'mad' and vop != 'mad_sat' else V(1)
        for i in range(4):
            if vop in ('mul', 'mul_sat'):
                e = f'({a[i]} * {b[i]})'
            elif vop in ('add', 'add_sat'):
                e = f'({a[i]} + {b[i]})'
            elif vop == 'max':
                e = f'max({a[i]}, {b[i]})' if a[i] != b[i] else a[i]
            elif vop == 'min':
                e = f'min({a[i]}, {b[i]})'
            else:
                c = V(2)
                e = f'({a[i]} * {b[i]} + {c[i]})'
            if vop in ('mul_sat', 'mad_sat', 'add_sat'):
                e = f'clamp({e}, 0.0, 1.0)'
            res[i] = e
    elif vop in ('dp3', 'dp4', 'dp2add'):
        a = V(0); b = V(1)
        n = 3 if vop == 'dp3' else 4 if vop == 'dp4' else 2
        e = ' + '.join(f'{a[i]} * {b[i]}' for i in range(n))
        if vop == 'dp2add':
            c = V(2)
            e += f' + {c[0]}'
        res = [f'({e})'] * 4
    elif vop == 'cube':
        a = V(0)
        # operand vector = swizzled comps 0,2,3 -> remap: d = (s2, s3, s0)
        dx, dy, dz = a[2], a[3], a[0]
        res = [f'CUBE_TC({dx},{dy},{dz})', f'CUBE_SC({dx},{dy},{dz})', f'CUBE_2MA({dx},{dy},{dz})', f'CUBE_ID({dx},{dy},{dz})']
    else:
        raise Exception('vec op ' + vop)
    for i in dcomps:
        pending.append((dkind, dr, i, res[i], vpred))

    # --- scalar op
    if sca and not sca.startswith('retain_prev'):
        sop, sargs = sca.split(None, 1)
        sargs = split_args(sargs)
        sd = sargs[0]
        skind, sr, scomps = parse_dst(sd)
        ops = sargs[1:]
        def S(i):
            return sca_operand(ops[i])
        e = None
        if sop in ('mulsc', 'addsc', 'subsc'):
            a = S(0)[0]; b = S(1)[0]
            sym = {'mulsc': '*', 'addsc': '+', 'subsc': '-'}[sop]
            e = f'({a} {sym} {b})'
        elif sop in ('adds', 'subs', 'maxs'):
            o = S(0)
            if len(o) == 2:
                a, b = o
            else:
                a, b = o[0], o[0]
            if sop == 'maxs':
                e = f'max({a}, {b})' if a != b else a
            else:
                e = f'({a} {"+" if sop == "adds" else "-"} {b})'
        elif sop == 'muls_prev':
            a = S(0)[0]
            e = f'({a} * {ps})'
        elif sop == 'adds_prev':
            a = S(0)[0]
            e = f'({a} + {ps})'
        elif sop in ('rsq', 'rcp', 'log', 'exp'):
            a = S(0)[0]
            e = f'{sop}({a})'
        elif sop == 'sgts':
            a = S(0)[0]
            e = f'float({a} > 0.0)'
        elif sop in ('setp_ge', 'setp_gt'):
            a = S(0)[0]
            op = '>=' if sop == 'setp_ge' else '>'
            new_p = f'({a} {op} 0.0)'
            e = None
            pending_ps = f'float(!{new_p})'
        else:
            raise Exception('sca op ' + sop)
        if e is not None:
            pending_ps = e
        for i in scomps:
            pending.append((skind, sr, i, f'@PS', spred))
    # commit
    # name ps first
    ps_name = None
    if pending_ps is not None:
        old_ps = ps
        pe = pending_ps
        if spred:
            pe = f'({cond(spred)} ? {pe} : {old_ps})'
        ps_name = mk('ps', idx, pe)
    for (k, r, i, e, pr) in pending:
        if e == '@PS':
            e = ps_name
        if k == 'o':
            final_out = (i, e)
            if i == 3:
                pass
            reg[('o', i)] = mk(f'out{i}', idx, e)
            continue
        old = getreg(r, i)
        if pr:
            e = f'({cond(pr)} ? {e} : {old})'
        reg[(r, i)] = mk(f'r{r}{"xyzw"[i]}', idx, e)
    if ps_name:
        ps = ps_name
    if new_p is not None:
        pname = mk('p0', idx, new_p)
        # predicate becomes a named var; later cond() uses 'p0' textual -> rename by suffix
        emitted[-1] = (f'P0_{idx}', new_p)
        names.add(f'P0_{idx}')
        # rewrite: subsequent cond() should reference this; keep a global
        globals()['CUR_P'] = f'P0_{idx}'

print('# instrs parsed', len(instrs), file=sys.stderr)

# ---- cond uses the *current* predicate var; do a second pass textual replacement:
# emitted entries use literal 'p0' / '!p0'. Resolve by order: replace with latest P0_ var defined before the entry.
resolved = []
cur_p = 'P0_UNSET'
for n, e in emitted:
    if n.startswith('P0_'):
        resolved.append((n, e))
        cur_p = n
        continue
    e2 = e.replace('!p0', f'!{cur_p}').replace('p0 ?', f'{cur_p} ?')
    resolved.append((n, e2))
emitted = resolved

# ---- liveness from out
outs = [n for n, e in emitted if n.startswith('out')]
defs = dict(emitted)
live = set()
stack = list(outs)
tok = re.compile(r'[A-Za-z_][A-Za-z_0-9]*')
while stack:
    n = stack.pop()
    if n in live:
        continue
    live.add(n)
    for t in tok.findall(defs[n]):
        if t in defs and t not in live:
            stack.append(t)

for n, e in emitted:
    if n in live:
        print(f'{n} = {e};')
