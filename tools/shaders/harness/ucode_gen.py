#!/usr/bin/env python3
"""DEV TOOL ONLY: never used to produce a committed shader (CLAUDE.md rule 1: a transcription of the game's microcode is built from the
player's game files and is not committed). All 66 shaders of src/scene_vs_swap.h are hand-written; this tool made first drafts that were
read, restructured by hand and checked against with tools/shaders/harness/vs_harness.py.
Session 87 (stage 4): a guest vertex shader's microcode listing (out/s87/shader_dump/shader_<hash>.ucode.vert, a local dump, never
committed) -> GLSL, instruction by instruction in program order, with the SDK translator's multiply-rule model: the rule (AvMulX) on every
multiply whose operands differ when the shader has jumps (no tracking), or only where the forward finite/tainted pass says so (--mode track);
dot products and mads fused when no rule applies. Forward jumps become if / else. The output is a first transcription, a reading aid and a reference for the harness: it is never committed.
  ucode_gen.py <ucode.vert> <out.vert> --stride 8 [--fc 95] [--mode all|track] [--finite 0|1] [--packed]
  --packed: float constants addressed by packed index (#define C<n> fc.c[i]) instead of the full 256-register block."""
import re, sys, argparse

ap = argparse.ArgumentParser()
ap.add_argument('src'); ap.add_argument('dst')
ap.add_argument('--stride', type=int, default=8)
ap.add_argument('--fc', type=int, default=95)
ap.add_argument('--mode', default='all')     # all: no tracking (shader has labels) / track: forward taint pass
ap.add_argument('--finite', type=int, default=1)
ap.add_argument('--fma', type=int, default=1)
ap.add_argument('--packed', action='store_true')
args = ap.parse_args()

LANES = 'xyzw'
class Op:
    def __init__(s, text):
        t = text.strip()
        s.neg = t.startswith('-')
        if s.neg: t = t[1:]
        m = re.match(r'(r_abs|c_abs|r|c)(?:\[(\d+)\]|(\d+)|\[76\+a0\]|\[(\d+)\+a0\])?\.?(\w*)$', t)
        m2 = re.match(r'(r_abs|c_abs|r|c)\[(\d+)\+a0\]\.(\w+)$', t)
        if m2:
            s.kind = m2.group(1); s.idx = None; s.base = int(m2.group(2)); s.swz = m2.group(3)
        else:
            m = re.match(r'(r_abs|c_abs|r|c)\[?(\d+)\]?(?:\.(\w+))?$', t)
            if not m: raise ValueError('operand ' + text)
            s.kind = m.group(1); s.idx = int(m.group(2)); s.base = None; s.swz = m.group(3) or 'xyzw'
        s.abs = s.kind.endswith('_abs')
        s.reg = s.kind[0] == 'r'
    def comp(s, i):  # source component letter for position i
        sw = s.swz
        return sw[i] if i < len(sw) else sw[-1]
    def key(s, i):
        return (s.reg, s.idx, s.base, s.comp(i), s.neg, s.abs)
    def vec(s, n):
        sw = ''.join(s.comp(i) for i in range(n))
        if s.reg: e = 'r%d.%s' % (s.idx, sw)
        elif s.idx is not None: e = 'fc.c[%d].%s' % (s.idx, sw)
        else: e = 'fc.c[%d + a0].%s' % (s.base, sw)
        if s.abs: e = 'abs(%s)' % e
        if s.neg: e = '(-%s)' % e
        return e
    def lane(s, i):
        c = s.comp(i)
        if s.reg: e = 'r%d.%s' % (s.idx, c)
        elif s.idx is not None: e = 'fc.c[%d].%s' % (s.idx, c)
        else: e = 'fc.c[%d + a0].%s' % (s.base, c)
        if s.abs: e = 'abs(%s)' % e
        if s.neg: e = '(-%s)' % e
        return e

def parse_dst(t):
    m = re.match(r'(oPos|oDepth|r|o)(\d*)\.?(\S*)$', t.strip())
    kind = m.group(1); n = m.group(2); sw = m.group(3)
    return kind, int(n) if n else 0, sw

class Ins: pass

lines = open(args.src, encoding='utf-8').read().splitlines()
items = []   # ('exec', cond, [Ins]), ('cjmp', cond, label), ('jmp', label), ('label', n), ('alloc')
cur = None
pending = None
def parse_pred(t):
    m = re.match(r'\((!?)p0\)\s*(.*)$', t)
    if m: return ('!p0' if m.group(1) else 'p0'), m.group(2)
    return None, t
for ln in lines:
    t = re.sub(r'^/\*[^*]*\*/\s*', '', ln).strip()
    if not t: continue
    if t.startswith('+'):
        t = t[1:].strip(); pred, t = parse_pred(t)
        op, rest = (t.split(None, 1) + [''])[:2]
        pending.scalar = (op, [x.strip() for x in re.split(r',\s*(?![^\[]*\])', rest)], pred)
        continue
    pred, t = parse_pred(t)
    op, rest = (t.split(None, 1) + [''])[:2]
    if op in ('exec', 'exece', 'cexec', 'alloc', 'serialize', 'cnop', 'cjmp', 'jmp', 'label'):
        if op in ('exec', 'exece'):
            cur = ['exec', pred, []]; items.append(cur)
        elif op == 'cexec':
            neg = rest.startswith('!'); cur = ['exec', ('!b' if neg else 'b') + rest.lstrip('!b'), []]; items.append(cur)
        elif op == 'cjmp':
            m = re.match(r'(!?)b(\d+),\s*L(\d+)', rest); items.append(['cjmp', (m.group(1) == '!', int(m.group(2))), int(m.group(3))])
        elif op == 'jmp': items.append(['jmp', None, int(rest.strip()[1:])])
        elif op == 'label': items.append(['label', None, int(rest.strip()[1:])])
        continue
    ins = Ins(); ins.pred = pred; ins.scalar = None; ins.exec_cond = cur[1]
    if op.startswith('vfetch') or op.startswith('tfetch'):
        ins.kind = 'fetch'; ins.op = op; ins.rest = rest
        cur[2].append(ins); pending = ins; continue
    ins.kind = 'alu'
    parts = [x.strip() for x in re.split(r',\s*(?![^\[]*\])', rest)]
    ins.vop = op; ins.vargs = parts
    cur[2].append(ins); pending = ins
    # scalar-only instructions are in the table below
SCALAR_ONLY = {'truncs', 'sgts', 'maxs', 'mins', 'muls', 'muls_prev', 'adds', 'adds_prev', 'subs', 'subs_prev', 'mulsc', 'addsc', 'subsc', 'rcp', 'rsq', 'sqrt',
               'setp_eq', 'setp_ne', 'setp_gt', 'setp_ge', 'maxas', 'seqs', 'sges', 'snes', 'exp', 'log', 'truncs', 'floors', 'frcs', 'rcpc', 'rsqc', 'mulsc0', 'mulsc1', 'sets'}


taint = [0] * 128
state = {'ps_taint': False}
def operand_tainted(op):
    if not op.reg: return False
    if op.idx >= 128: raise Exception('reg >= 128')
    for i in range(len(op.swz) if len(op.swz) > 0 else 1):
        c = op.comp(i)
        if c in 'xyzw' and (taint[op.idx] >> 'xyzw'.index(c)) & 1: return True
    return False

def mulx(a, b, identical, action):
    if action == 2 or identical: return '(%s * %s)' % (a, b)
    return 'AvMulX(%s, %s)' % (a, b)

def note_result(dkind, didx, dsw, tainted, weak, clamped=False):
    if dkind != 'r': return
    for i, ch in enumerate(dsw):
        if ch == '_': continue
        lt = tainted and not clamped and ch in 'xyzw'
        if weak: taint[didx] |= (1 << i) if lt else 0
        else: taint[didx] = (taint[didx] & ~(1 << i)) | ((1 << i) if lt else 0)


def fetch_code(ins):
    op = ins.op
    m = re.match(r'(r\d+)\.?(\w*)\s*,\s*(.*)$', ins.rest)
    dreg = int(re.match(r'r(\d+)', ins.rest).group(1))
    dsw = re.match(r'r\d+\.?(\w*)', ins.rest).group(1) or 'xyzw'
    if op.startswith('tfetch2D'):
        parts = [x.strip() for x in ins.rest.split(',')]
        co = Op(parts[1]); tf = int(re.match(r'tf(\d+)', parts[2]).group(1))
        wide = 'w' in dsw and tf == 16
        lines_ = [('{ vec4 t = XeVertexTexture%dRgba(vec2(%s, %s));' if wide else '{ vec3 t = XeVertexTexture%d(vec2(%s, %s));') % (tf, co.lane(0), co.lane(1))]
        for i, ch in enumerate(dsw):
            if ch == '_': continue
            lines_.append('  r%d.%s = %s;' % (dreg, LANES[i], ('t.' + ch) if (ch in 'xyz' or (wide and ch == 'w')) else '0.0'))
        lines_.append('}')
        return lines_, dreg, dsw, (not args.finite)
    off = 0
    mo = re.search(r'Offset=(\d+)', ins.rest)
    if mo: off = int(mo.group(1))
    fmt = re.search(r'DataFormat=(\w+)', ins.rest).group(1)
    lines_ = []
    if op == 'vfetch_full':
        lines_.append('at = XeVertexAddress(uint(r0.x), %d, %d);' % (args.stride, args.fc))
    addr = 'at + %d' % off
    if all(ch in '_01' for ch in dsw):
        for i, ch in enumerate(dsw):
            if ch != '_': lines_.append('r%d.%s = %s.0;' % (dreg, LANES[i], ch))
        return lines_, dreg, dsw, False
    if fmt == 'FMT_32_32_32_32_FLOAT':
        v = 'vec4(XeFetchFloat(%s), XeFetchFloat(%s + 1), XeFetchFloat(%s + 2), XeFetchFloat(%s + 3))' % (addr, addr, addr, addr)
    elif fmt == 'FMT_32_32_32_FLOAT':
        v = 'vec4(XeFetchFloat(%s), XeFetchFloat(%s + 1), XeFetchFloat(%s + 2), 1.0)' % (addr, addr, addr)
    elif fmt == 'FMT_32_32_FLOAT':
        v = 'vec4(XeFetchFloat(%s), XeFetchFloat(%s + 1), 0.0, 1.0)' % (addr, addr)
    elif fmt == 'FMT_32_FLOAT':
        v = 'vec4(XeFetchFloat(%s), 0.0, 0.0, 1.0)' % addr
    elif fmt == 'FMT_8_8_8_8':
        if 'NumFormat=integer' in ins.rest:
            v = 'vec4(uvec4(bitfieldExtract(XeFetchDword(%s), 0, 8), bitfieldExtract(XeFetchDword(%s), 8, 8), bitfieldExtract(XeFetchDword(%s), 16, 8), bitfieldExtract(XeFetchDword(%s), 24, 8)))' % ((addr,) * 4)
        else:
            v = 'AvFetchColorAt(%s).zyxw' % addr
    elif fmt == 'FMT_11_11_10':
        v = 'vec4(max(vec3(ivec3(bitfieldExtract(int(XeFetchDword(%s)), 0, 10), bitfieldExtract(int(XeFetchDword(%s)), 10, 11), bitfieldExtract(int(XeFetchDword(%s)), 21, 11))) * vec3(0.00195694715, 0.000977517106, 0.000977517106), vec3(-1.0)), 1.0)' % ((addr,) * 3)
    elif fmt == 'FMT_16_16_FLOAT':
        v = 'vec4(unpackHalf2x16(XeFetchDword(%s)), 0.0, 1.0)' % addr
    else:
        raise Exception('fetch format ' + fmt)
    lines_.append('{ vec4 f = %s;' % v)
    for i, ch in enumerate(dsw):
        if ch == '_': continue
        if ch in '01': val = ch + '.0'
        else: val = 'f.' + ch
        lines_.append('  r%d.%s = %s;' % (dreg, LANES[i], val))
    lines_.append('}')
    taintable = (not args.finite) and fmt in ('FMT_16_16_FLOAT', 'FMT_32_32_32_32_FLOAT', 'FMT_32_32_32_FLOAT', 'FMT_32_32_FLOAT', 'FMT_32_FLOAT')
    return lines_, dreg, dsw, taintable

def addr_expr(x):  # float expression x -> address register int (round, NaN -> -256)
    return 'XeAddress(%s)' % x

def alu(ins, weak_exec):
    """returns code lines for one vector (+ scalar) ALU instruction"""
    code = []
    # operands
    vop = ins.vop
    vsat = False
    if vop and vop.endswith('_sat'):
        vsat = True; vop = vop[:-4]
    track = args.mode == 'track'
    pre_taint = list(taint)
    vector_tainted = False
    def vact():
        return 0
    res_vec = None   # (dkind, didx, dsw, lanes exprs)
    # decide whether this line is scalar-only
    scalar = ins.scalar
    if vop in SCALAR_ONLY and not scalar:
        scalar = (ins.vop, ins.vargs, ins.pred); vop = None
    vec_lines = []; sc_lines = []
    vdst = None
    if vop:
        a = ins.vargs
        dk, di, dsw = parse_dst(a[0])
        srcs = [Op(x) for x in a[1:]]
        vector_tainted = any(operand_tainted(o) for o in srcs)
        action = 2 if (track and not vector_tainted) else 0
        wl = [i for i, ch in enumerate(dsw if dsw else 'xyzw') if ch != '_']
        const_lanes = {i: ('0.0' if ch == '0' else '1.0') for i, ch in enumerate(dsw) if ch in '01'}
        if not dsw: wl = [0, 1, 2, 3]
        exprs = {}
        if vop in ('add', 'sub'):
            for l in wl: exprs[l] = '(%s + %s)' % (srcs[0].lane(l), srcs[1].lane(l))
        elif vop == 'mul':
            for l in wl:
                ident = srcs[0].key(l) == srcs[1].key(l)
                exprs[l] = mulx(srcs[0].lane(l), srcs[1].lane(l), ident, action)
        elif vop == 'mad':
            diff = [l for l in wl if srcs[0].key(l) != srcs[1].key(l)]
            for l in wl:
                a_, b_, c_ = srcs[0].lane(l), srcs[1].lane(l), srcs[2].lane(l)
                if args.fma and (action == 2 or not diff):
                    exprs[l] = 'fma(%s, %s, %s)' % (a_, b_, c_)
                else:
                    ident = srcs[0].key(l) == srcs[1].key(l)
                    exprs[l] = '(%s + %s)' % (mulx(a_, b_, ident, action), c_)
        elif vop == 'max':
            for l in wl:
                if srcs[0].key(l) == srcs[1].key(l): exprs[l] = srcs[0].lane(l)
                else: exprs[l] = '(%s >= %s ? %s : %s)' % (srcs[0].lane(l), srcs[1].lane(l), srcs[0].lane(l), srcs[1].lane(l))
        elif vop == 'min':
            for l in wl: exprs[l] = '(%s < %s ? %s : %s)' % (srcs[0].lane(l), srcs[1].lane(l), srcs[0].lane(l), srcs[1].lane(l))
        elif vop in ('trunc', 'floor', 'frc'):
            fn = {'trunc': 'trunc', 'floor': 'floor', 'frc': 'fract'}[vop]
            for l in wl: exprs[l] = '%s(%s)' % (fn, srcs[0].lane(l))
        elif vop in ('sgt', 'sge', 'seq', 'sne'):
            sym = {'sgt': '>', 'sge': '>=', 'seq': '==', 'sne': '!='}[vop]
            for l in wl: exprs[l] = '((%s %s %s) ? 1.0 : 0.0)' % (srcs[0].lane(l), sym, srcs[1].lane(l))
        elif vop in ('cndeq', 'cndge', 'cndgt'):
            sym = {'cndeq': '==', 'cndge': '>=', 'cndgt': '>'}[vop]
            for l in wl: exprs[l] = '(%s %s 0.0 ? %s : %s)' % (srcs[0].lane(l), sym, srcs[1].lane(l), srcs[2].lane(l))
        elif vop in ('dp4', 'dp3', 'dp2add'):
            n = {'dp4': 4, 'dp3': 3, 'dp2add': 2}[vop]
            diff = [i for i in range(n) if srcs[0].key(i) != srcs[1].key(i)]
            fused = args.fma and (action == 2 or not diff)
            if vop == 'dp2add':
                e = None
                for i in range(2):
                    pa, pb = srcs[0].lane(i), srcs[1].lane(i)
                    ident = srcs[0].key(i) == srcs[1].key(i)
                    if fused and i:
                        e = 'fma(%s, %s, %s)' % (pa, pb, e)
                    else:
                        pr = mulx(pa, pb, ident, action)
                        e = pr if e is None else '(%s + %s)' % (e, pr)
                e = '(%s + %s)' % (e, srcs[2].lane(0))
            else:
                fn = {'dp4': 'AvDot4', 'dp3': 'AvDot3'}[vop] + ('' if fused else 'R')
                if action == 2 and not fused: fn = None
                if fn is None:
                    e = None
                    for i in range(n):
                        pr = '(%s * %s)' % (srcs[0].lane(i), srcs[1].lane(i))
                        e = pr if e is None else '(%s + %s)' % (e, pr)
                else:
                    e = '%s(%s, %s)' % (fn, srcs[0].vec(n), srcs[1].vec(n))
            for l in wl: exprs[l] = e
        elif vop == 'max4':
            e = 'max(max(%s, %s), max(%s, %s))' % tuple(srcs[0].lane(i) for i in range(4))
            for l in wl: exprs[l] = e
        elif vop == 'maxa':
            for l in wl: exprs[l] = '(%s >= %s ? %s : %s)' % (srcs[0].lane(l), srcs[1].lane(l), srcs[0].lane(l), srcs[1].lane(l))
            vec_lines.append('a0 = %s;' % addr_expr('(%s + 0.0)' % srcs[0].lane(3)))
        elif vop == 'setp_eq_push' or vop.startswith('setp'):
            raise Exception('vector ' + vop)
        else:
            raise Exception('vector op ' + vop)
        if vsat:
            for l_ in wl: exprs[l_] = 'AvSat(%s)' % exprs[l_]
        for l_, c_ in const_lanes.items(): exprs[l_] = c_
        vdst = (dk, di, dsw if dsw else 'xyzw', wl, exprs)
    # scalar
    sdst = None
    if scalar:
        sop_full, sargs, spred = scalar
        sop = sop_full[:-4] if sop_full.endswith('_sat') else sop_full
        stainted = False
        sres = None
        sk, si, ssw = parse_dst(sargs[0])
        ss = [Op(x) for x in sargs[1:]]
        stainted = any(operand_tainted(o) for o in ss)
        if sop in ('adds_prev', 'muls_prev', 'subs_prev'): stainted = stainted or state['ps_taint']
        saction = 2 if (track and not stainted) else 0
        def ab(o):  # a, b scalar pair of a one-operand two-component scalar operand
            return o.lane(0), o.lane(1)
        if sop == 'adds': a_, b_ = ab(ss[0]); sres = '(%s + %s)' % (a_, b_)
        elif sop == 'subs': a_, b_ = ab(ss[0]); sres = '(%s - %s)' % (a_, b_)
        elif sop == 'adds_prev': sres = '(%s + ps)' % ss[0].lane(0)
        elif sop == 'subs_prev': sres = '(%s - ps)' % ss[0].lane(0)
        elif sop == 'muls':
            a_, b_ = ab(ss[0]); sres = mulx(a_, b_, ss[0].key(0) == ss[0].key(1), saction)
        elif sop == 'muls_prev':
            sres = mulx(ss[0].lane(0), 'ps', False, saction)
        elif sop in ('mulsc', 'mulsc0', 'mulsc1'):
            sres = mulx(ss[0].lane(0), ss[1].lane(0), False, saction)
        elif sop == 'addsc': sres = '(%s + %s)' % (ss[0].lane(0), ss[1].lane(0))
        elif sop == 'subsc': sres = '(%s - %s)' % (ss[0].lane(0), ss[1].lane(0))
        elif sop in ('maxs', 'maxas'):
            a_, b_ = ab(ss[0]); sres = a_ if ss[0].key(0) == ss[0].key(1) else '(%s >= %s ? %s : %s)' % (a_, b_, a_, b_)
            if sop == 'maxas':
                sc_lines.append('a0 = XeAddress(%s);' % a_)
        elif sop == 'mins':
            a_, b_ = ab(ss[0]); sres = '(%s < %s ? %s : %s)' % (a_, b_, a_, b_)
        elif sop == 'sgts': a_, b_ = ab(ss[0]); sres = '((%s > %s) ? 1.0 : 0.0)' % (a_, b_)
        elif sop == 'sges': a_, b_ = ab(ss[0]); sres = '((%s >= %s) ? 1.0 : 0.0)' % (a_, b_)
        elif sop == 'rcp': sres = '(1.0 / %s)' % ss[0].lane(0)
        elif sop == 'rsq': sres = 'inversesqrt(%s)' % ss[0].lane(0)
        elif sop == 'truncs': sres = 'trunc(%s)' % ss[0].lane(0)
        elif sop == 'floors': sres = 'floor(%s)' % ss[0].lane(0)
        elif sop == 'frcs': sres = 'fract(%s)' % ss[0].lane(0)
        elif sop == 'exp': sres = 'exp2(%s)' % ss[0].lane(0)
        elif sop == 'log': sres = 'log2(%s)' % ss[0].lane(0)
        elif sop == 'sqrt': sres = 'sqrt(%s)' % ss[0].lane(0)
        elif sop in ('setp_eq', 'setp_ne', 'setp_gt', 'setp_ge'):
            sym = {'setp_eq': '==', 'setp_ne': '!=', 'setp_gt': '>', 'setp_ge': '>='}[sop]
            a_ = ss[0].lane(0)
            sc_lines.append('p0 = (%s %s 0.0);' % (a_, sym))
            sres = '(p0 ? 0.0 : 1.0)'
        else:
            raise Exception('scalar op ' + sop)
        sdst = (sk, si, ssw, sres, stainted, sop_full, spred)
    return vec_lines, vdst, sc_lines, sdst, vector_tainted
# ---------------------------------------------------------------------------------------------------------------------------
# emitter: structured (forward jumps become if / else), registers are precise locals
label_idx = {it[2]: idx for idx, it in enumerate(items) if it[0] == 'label'}
used_outs = set()
for it in items:
    if it[0] == 'exec':
        for ins in it[2]:
            if ins.kind == 'alu':
                ds = [ins.vargs[0]] + ([ins.scalar[1][0]] if ins.scalar else [])
                for d in ds:
                    m = re.match(r'o(\d+)(?:\.|$)', d.strip())
                    if m: used_outs.add(int(m.group(1)))
code = []
def pcond(p): return 'p0' if p == 'p0' else '!p0'

def emit_exec(it, ind):
    cond = it[1]
    exec_uncond = cond is None
    pre = ind
    close = False
    if cond in ('p0', '!p0'):
        code.append(ind + 'if (%s) {' % cond); close = True
    elif cond and cond.startswith('b'):
        code.append(ind + 'if (XeBool(%s)) {' % cond[1:]); close = True
    elif cond and cond.startswith('!b'):
        code.append(ind + 'if (!XeBool(%s)) {' % cond[2:]); close = True
    ind2 = ind + ('  ' if close else '')
    for ins in it[2]:
        weak = ins.pred is not None or not exec_uncond
        if ins.kind == 'fetch':
            ls, dreg, dsw, tnt = fetch_code(ins)
            for l in ls: code.append(ind2 + l)
            if args.mode == 'track': note_result('r', dreg, dsw, tnt, weak)
            continue
        vl, vdst, sl, sdst, vt = alu(ins, weak)
        pr_v = ins.pred
        # hazard / pairing => compute into temps first
        need_tmp = bool(sdst)
        if vdst and vdst[0] == 'r':
            dst_reg = vdst[1]
            need_tmp = need_tmp or any(re.search(r'\br%d\.' % dst_reg, e) for e in vdst[4].values())
        if sdst and not vdst:
            need_tmp = True
        def tgt_of(kind, idx):
            return ('r%d' % idx) if kind == 'r' else ('out_%d' % idx if kind == 'o' else 'pos')
        lines_ = []
        if vdst:
            dk, di, dsw, wl, ex = vdst
            if need_tmp:
                for l in wl: lines_.append('precise float v%d = %s;' % (l, ex[l]))
        if sdst:
            sk, si, ssw, sres, st, sop, spred = sdst
            lines_.append('precise float s = %s;' % sres)
        stores = []
        if vdst:
            dk, di, dsw, wl, ex = vdst
            for l in wl:
                stores.append((pr_v, '%s.%s = %s;' % (tgt_of(dk, di), LANES[l], ('v%d' % l) if need_tmp else ex[l])))
            for x in vl: stores.append((pr_v, x))
        if sdst:
            sk, si, ssw, sres, st, sop, spred = sdst
            pr = spred or ins.pred
            for x in sl: stores.append((pr, x))
            for i2, ch in enumerate(ssw):
                if ch == '_': continue
                stores.append((pr, '%s.%s = %s;' % (tgt_of(sk, si), LANES[i2], 'AvSat(s)' if sop.endswith('_sat') else 's')))
            if sop != 'retain_prev': stores.append((pr, 'ps = s;'))
        # group consecutive stores with the same predicate
        if all(p is None for p, _ in stores) and not lines_:
            for _, s_ in stores: code.append(ind2 + s_)
        else:
            if lines_ or any(p is not None for p, _ in stores):
                code.append(ind2 + '{')
                for l in lines_: code.append(ind2 + '  ' + l)
                for p, s_ in stores:
                    code.append(ind2 + '  ' + (('if (%s) ' % pcond(p)) if p else '') + s_)
                code.append(ind2 + '}')
        if args.mode == 'track':
            if vdst: note_result(vdst[0], vdst[1], vdst[2], vt, weak)
            if sdst:
                sk, si, ssw, sres, st, sop, spred = sdst
                if sop in ('rcp', 'rsq', 'sqrt', 'exp', 'log'): st = True
                note_result(sk, si, ssw, st, weak or spred is not None)
                state['ps_taint'] = (state['ps_taint'] or st) if (weak or spred) else st
    if close: code.append(pre + '}')

def emit(i, j, ind):
    k = i
    while k < j:
        it = items[k]
        if it[0] == 'exec':
            emit_exec(it, ind); k += 1
        elif it[0] == 'label':
            k += 1
        elif it[0] == 'jmp':
            raise Exception('unstructured jmp at item %d' % k)
        elif it[0] == 'cjmp':
            neg, bn = it[1]
            m = label_idx[it[2]]
            fall = ('XeBool(%d)' % bn) if neg else ('!XeBool(%d)' % bn)
            last = items[m - 1]
            if last[0] == 'jmp':
                lm = label_idx[last[2]]
                code.append(ind + 'if (%s) {' % fall)
                emit(k + 1, m - 1, ind + '  ')
                code.append(ind + '} else {')
                emit(m + 1, lm, ind + '  ')
                code.append(ind + '}')
                k = lm + 1
            else:
                code.append(ind + 'if (%s) {' % fall)
                emit(k + 1, m, ind + '  ')
                code.append(ind + '}')
                k = m
        else:
            k += 1

emit(0, len(items), '  ')
body = '\n'.join(code)
maxr = max([int(x) for x in re.findall(r'\br(\d+)\b', body)] + [0])
header = ['#version 450', '#extension GL_GOOGLE_include_directive : require', '#include "xenos_vs.glsl"']
if 'XeVertexTexture17' in body: header.append('#include "xenos_vs_tex17.glsl"')
if 'XeVertexTexture16' in body: header.append('#include "xenos_vs_tex.glsl"')
fregs = sorted(set(int(x) for x in re.findall(r'fc\.c\[(\d+)\]', body)))
if args.packed:
    body = re.sub(r'fc\.c\[(\d+)\]', lambda m: 'C' + m.group(1), body)
    header += ['', 'layout(set = 1, binding = 1, std140) uniform XeFloatConstants { vec4 c[%d]; } fc;' % len(fregs)]
    header += ['#define C%d fc.c[%d]' % (r, i) for i, r in enumerate(fregs)]
    header += ['', '#include "xenos_vs_math.glsl"', '']
    print('float registers:', ' '.join(str(r) for r in fregs))
else:
    header += ['#include "xenos_vs_math.glsl"', '#include "xenos_vs_skin.glsl"', '']
for n in sorted(used_outs):
    header.append('layout(location = %d) invariant out vec4 out_%d;' % (n, n))
header += ['', 'void main() {', '  uint guest_index = XeGuestVertexIndex();']
header.append('  precise vec4 ' + ', '.join('r%d = vec4(0.0)' % i for i in range(maxr + 1)) + ';')
header.append('  r0.x = float(guest_index);')
if re.search(r'\bps\b', body): header.append('  float ps = 0.0;')
if re.search(r'\ba0\b', body): header.append('  int a0 = 0;')
if re.search(r'\bp0\b', body): header.append('  bool p0 = false;')
if re.search(r'\bat\b', body): header.append('  int at = 0;')
for n in sorted(used_outs):
    header.append('  out_%d = vec4(0.0);' % n)
header.append('  precise vec4 pos = vec4(0.0);')
text = '\n'.join(header) + '\n' + body + '\n  XeOutputPosition(pos);\n}\n'
open(args.dst, 'w', encoding='utf-8').write(text)
print('wrote', args.dst, text.count('\n'), 'lines')
