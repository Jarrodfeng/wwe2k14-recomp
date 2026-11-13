"""S101 lane R1: a Lua 5.1 bytecode lister for the 30 Years era / match scripts (read-only, prints no script text beyond identifiers).

    py -I tools\\wm30\\lua51_list.py CHUNK.bin [--consts] [--calls] [--dis] [--guards]
    py -I tools\\wm30\\lua51_list.py --era E [--rec N] [--calls] [--guards]   (reads pac\\evtdsc\\evtdsc<E>.pac from game_data\\extracted)
    py -I tools\\wm30\\lua51_list.py --selftest

Reads the compiled chunks (little-endian, 4-byte size_t, 4-byte float numbers, as `1B 4C 75 61 51 00 01 04 04 04 04 00`; the header
fields are honoured, so other sizes parse too). Prints per function: line range, upvalues / params / vararg, instruction count, the
constants and, for every CALL, a symbolic call text rebuilt by a straight-line register walk (`yMatch.CallFunction(1090, 0, 0, 1, 0, 1003)`)
plus the conditions that guard it (the `if` tests whose skipped body contains the call; a heuristic, marked [L] in the docs: loops and
and/or short-circuit chains are shown as the raw tests they are). The era-pac reader reuses tools\\wm30\\pac.py (XCTD unpack, PACH walk).
Nothing from the game is ever written by this tool.
"""

import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

OPS = ['MOVE', 'LOADK', 'LOADBOOL', 'LOADNIL', 'GETUPVAL', 'GETGLOBAL', 'GETTABLE', 'SETGLOBAL', 'SETUPVAL', 'SETTABLE', 'NEWTABLE',
       'SELF', 'ADD', 'SUB', 'MUL', 'DIV', 'MOD', 'POW', 'UNM', 'NOT', 'LEN', 'CONCAT', 'JMP', 'EQ', 'LT', 'LE', 'TEST', 'TESTSET',
       'CALL', 'TAILCALL', 'RETURN', 'FORLOOP', 'FORPREP', 'TFORLOOP', 'SETLIST', 'CLOSE', 'CLOSURE', 'VARARG']
# format: 'ABC', 'ABx', 'AsBx'
FMT = {n: 'ABC' for n in OPS}
FMT.update({'LOADK': 'ABx', 'GETGLOBAL': 'ABx', 'SETGLOBAL': 'ABx', 'CLOSURE': 'ABx', 'JMP': 'AsBx', 'FORLOOP': 'AsBx', 'FORPREP': 'AsBx'})
ARITH = {'ADD': '+', 'SUB': '-', 'MUL': '*', 'DIV': '/', 'MOD': '%', 'POW': '^'}


class LuaError(Exception):
    pass


class Func:
    pass


class Reader:
    def __init__(self, data):
        self.d = data
        self.p = 0

    def take(self, n):
        if self.p + n > len(self.d):
            raise LuaError('truncated chunk at %d (+%d)' % (self.p, n))
        b = self.d[self.p:self.p + n]
        self.p += n
        return b

    def u8(self):
        return self.take(1)[0]

    def uint(self, size):
        return int.from_bytes(self.take(size), self.endian)


def parse(data):
    """A compiled chunk -> its top Func (children in .protos). Raises LuaError on anything outside the Lua 5.1 spec."""
    r = Reader(data)
    if r.take(4) != b'\x1bLua':
        raise LuaError('not a Lua chunk (no ESC Lua signature)')
    if r.u8() != 0x51:
        raise LuaError('not Lua 5.1 (version byte)')
    if r.u8() != 0:
        raise LuaError('unknown format')
    little = r.u8()
    r.endian = 'little' if little == 1 else 'big'
    r.sz_int = r.u8()
    r.sz_size = r.u8()
    r.sz_ins = r.u8()
    r.sz_num = r.u8()
    r.integral = r.u8()
    if r.sz_ins != 4:
        raise LuaError('instruction size %d unsupported' % r.sz_ins)
    if r.sz_num == 8 and not r.integral:
        r.numfmt = ('<' if little == 1 else '>') + 'd'
    elif r.sz_num == 4 and not r.integral:
        r.numfmt = ('<' if little == 1 else '>') + 'f'
    elif r.integral and r.sz_num in (4, 8):
        r.numfmt = None
    else:
        raise LuaError('number size %d unsupported' % r.sz_num)
    return _func(r)


def _string(r):
    n = r.uint(r.sz_size)
    if n == 0:
        return None
    return r.take(n)[:-1].decode('latin-1')


def _func(r):
    f = Func()
    f.source = _string(r)
    f.line = r.uint(r.sz_int)
    f.lastline = r.uint(r.sz_int)
    f.nups = r.u8()
    f.nparams = r.u8()
    f.vararg = r.u8()
    f.maxstack = r.u8()
    n = r.uint(r.sz_int)
    f.code = [int.from_bytes(r.take(4), r.endian) for _ in range(n)]
    f.consts = []
    for _ in range(r.uint(r.sz_int)):
        t = r.u8()
        if t == 0:
            f.consts.append(('nil', None))
        elif t == 1:
            f.consts.append(('bool', r.u8() != 0))
        elif t == 3:
            raw = r.take(r.sz_num)
            if r.numfmt:
                v = struct.unpack(r.numfmt, raw)[0]
            else:
                v = int.from_bytes(raw, r.endian, signed=True)
            if float(v).is_integer():
                v = int(v)
            f.consts.append(('num', v))
        elif t == 4:
            f.consts.append(('str', _string(r) or ''))
        else:
            raise LuaError('bad constant type tag %d' % t)
    f.protos = [_func(r) for _ in range(r.uint(r.sz_int))]
    f.lineinfo = [r.uint(r.sz_int) for _ in range(r.uint(r.sz_int))]
    f.locvars = [(_string(r), r.uint(r.sz_int), r.uint(r.sz_int)) for _ in range(r.uint(r.sz_int))]
    f.upnames = [_string(r) for _ in range(r.uint(r.sz_int))]
    return f


def decode(ins):
    op = ins & 0x3F
    a = (ins >> 6) & 0xFF
    c = (ins >> 14) & 0x1FF
    b = (ins >> 23) & 0x1FF
    bx = (ins >> 14) & 0x3FFFF
    return OPS[op] if op < len(OPS) else 'OP%d' % op, a, b, c, bx, bx - 131071


def kstr(f, i):
    t, v = f.consts[i]
    if t == 'str':
        return v
    if t == 'nil':
        return 'nil'
    if t == 'bool':
        return 'true' if v else 'false'
    return repr(v)


def rk(f, regs, x):
    if x >= 256:
        t, v = f.consts[x - 256]
        return ('"%s"' % v) if t == 'str' else kstr(f, x - 256)
    return regs.get(x, 'R%d' % x)


def analyse(f):
    """Straight-line register walk. Returns (calls, tests): calls = [dict(pc, text, fn, args, guards)], tests = per-pc guard info."""
    code = f.code
    regs = {}
    calls = []
    tests = []   # (pc_of_test, cond_when_body_runs, body_start, body_end)
    last_call = {}
    for pc, ins in enumerate(code):
        op, a, b, c, bx, sbx = decode(ins)
        if op == 'MOVE':
            regs[a] = regs.get(b, 'R%d' % b)
        elif op == 'LOADK':
            t, v = f.consts[bx]
            regs[a] = ('"%s"' % v) if t == 'str' else kstr(f, bx)
        elif op == 'LOADBOOL':
            regs[a] = 'true' if b else 'false'
        elif op == 'LOADNIL':
            for i in range(a, b + 1):
                regs[i] = 'nil'
        elif op == 'GETUPVAL':
            regs[a] = 'U%d' % b
        elif op == 'GETGLOBAL':
            regs[a] = kstr(f, bx)
        elif op == 'GETTABLE':
            k = rk(f, regs, c)
            base = regs.get(b, 'R%d' % b)
            regs[a] = '%s.%s' % (base, k.strip('"')) if k.startswith('"') else '%s[%s]' % (base, k)
        elif op == 'SELF':
            base = regs.get(b, 'R%d' % b)
            k = rk(f, regs, c).strip('"')
            regs[a + 1] = base
            regs[a] = '%s:%s' % (base, k)
        elif op in ARITH:
            regs[a] = '(%s %s %s)' % (rk(f, regs, b), ARITH[op], rk(f, regs, c))
        elif op == 'UNM':
            regs[a] = '(-%s)' % regs.get(b, 'R%d' % b)
        elif op == 'NOT':
            regs[a] = '(not %s)' % regs.get(b, 'R%d' % b)
        elif op == 'LEN':
            regs[a] = '(#%s)' % regs.get(b, 'R%d' % b)
        elif op == 'CONCAT':
            regs[a] = '(' + ' .. '.join(regs.get(i, 'R%d' % i) for i in range(b, c + 1)) + ')'
        elif op == 'NEWTABLE':
            regs[a] = '{}'
        elif op == 'CLOSURE':
            regs[a] = '<function %d>' % bx
        elif op in ('CALL', 'TAILCALL'):
            nargs = b - 1 if b else None
            fn = regs.get(a, 'R%d' % a)
            if nargs is None:
                args = ['...']
            else:
                args = [regs.get(a + 1 + i, 'R%d' % (a + 1 + i)) for i in range(nargs)]
            if ':' in fn:   # method call: R(A+1) is self, already counted in B
                pass
            text = '%s(%s)' % (fn, ', '.join(args))
            calls.append(dict(pc=pc, fn=fn, args=args, text=text, guards=[]))
            if op == 'CALL':
                nret = c - 1
                for i in range(max(nret, 1) if c != 1 else 0):
                    regs[a + i] = text if i == 0 else '%s[%d]' % (text, i)
        elif op in ('EQ', 'LT', 'LE') and pc + 1 < len(code):
            j = decode(code[pc + 1])
            if j[0] == 'JMP':
                tgt = pc + 2 + j[5]
                sym = {'EQ': '==', 'LT': '<', 'LE': '<='}[op]
                cond = '%s %s %s' % (rk(f, regs, b), sym, rk(f, regs, c))
                # JMP taken when (cmp == A); body (fall-through past the jmp) runs when cmp ~= A
                if a == 0:
                    body = cond
                else:
                    body = 'not (%s)' % cond
                tests.append((pc, body, pc + 2, tgt))
        elif op == 'TEST' and pc + 1 < len(code):
            j = decode(code[pc + 1])
            if j[0] == 'JMP':
                tgt = pc + 2 + j[5]
                x = regs.get(a, 'R%d' % a)
                body = x if c == 0 else 'not %s' % x
                tests.append((pc, body, pc + 2, tgt))
        elif op == 'TESTSET' and pc + 1 < len(code):
            j = decode(code[pc + 1])
            if j[0] == 'JMP':
                tgt = pc + 2 + j[5]
                x = regs.get(b, 'R%d' % b)
                body = x if c == 0 else 'not %s' % x
                tests.append((pc, body, pc + 2, tgt))
    for cl in calls:
        for (tpc, body, s, e) in tests:
            if s <= cl['pc'] < e and e > s:
                cl['guards'].append(body)
    return calls, tests


def func_name(path, f):
    return path if path else 'main'


def listing(f, path='', consts=False, calls=True, dis=False, guards=True, out=None):
    p = out or print
    p('function %s: lines %d-%d, %d instr, %d consts, %d protos, params %d%s' % (
        func_name(path, f), f.line, f.lastline, len(f.code), len(f.consts), len(f.protos), f.nparams, ', vararg' if f.vararg else ''))
    if consts:
        for i, (t, v) in enumerate(f.consts):
            p('  K%-4d %-4s %s' % (i, t, v))
    if dis:
        for pc, ins in enumerate(f.code):
            op, a, b, c, bx, sbx = decode(ins)
            fm = FMT.get(op, 'ABC')
            if fm == 'ABC':
                p('  %4d  %-9s %d %d %d' % (pc, op, a, b, c))
            elif fm == 'ABx':
                p('  %4d  %-9s %d %d' % (pc, op, a, bx))
            else:
                p('  %4d  %-9s %d %d' % (pc, op, a, sbx))
    if calls:
        cs, _ = analyse(f)
        for cl in cs:
            line = '  call @%d: %s' % (cl['pc'], cl['text'])
            if guards and cl['guards']:
                line += '   [if %s]' % ' and '.join(cl['guards'])
            p(line)
    for i, sub in enumerate(f.protos):
        listing(sub, '%s/%d' % (path, i) if path else str(i), consts, calls, dis, guards, out)


# ---------------------------------------------------------------- scene table (lane R1): 1003 calls, steps, entry conditions

import re

# Meaning of the CallFunction ids that matter to a scene table. [C] = handler read in the listing (docs\systems\wm30.md "Objective scenes
# (lane R1)"); the word after it is the reading of what the handler computes, [L] where only the shape is confirmed.
IDS = {0: 'setvar', 1: 'var', 105: 'sceneReady', 106: 'scenePreloaded', 201: 'grappleWith', 202: 'statCount', 300: 'dmgLevel', 309: 'actU16',
       310: 'actB32', 311: 'actB36', 312: 'actFloat1044', 314: 'inZone', 1002: 'preload', 1003: 'playScene'}


def split_args(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
            continue
        depth += (ch == '(') - (ch == ')')
        cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def pretty(text):
    """yMatch.CallFunction(a, b, ID) -> name(a, b) (ID last: the game switches on the last argument); var reads: CallFunction(X, 1) -> X
    shown as var[X] and var[267] as step."""
    key = 'yMatch.CallFunction('
    while key in text:
        i = text.rfind(key)            # innermost-last first
        j, depth = i + len(key), 1
        while j < len(text) and depth:
            depth += (text[j] == '(') - (text[j] == ')')
            j += 1
        args = split_args(text[i + len(key):j - 1])
        if not args:
            rep = 'call()'
        else:
            try:
                cid = int(args[-1])
            except ValueError:
                cid = None
            rest = args[:-1]
            if cid == 1 and len(rest) == 1:
                rep = 'step' if rest[0] == '267' else 'var[%s]' % rest[0]
            elif cid is not None:
                rep = '%s(%s)' % (IDS.get(cid, 'id%d' % cid), ', '.join(rest))
            else:
                rep = 'call(%s)' % ', '.join(args)
        text = text[:i] + rep + text[j:]
    return text


def scene_rows(f):
    """For one chunk: [{'bank','flag','args','step','guards','entry'}] for every playScene(1003) call, and the preloaded banks.
    entry = the conditions under which the Lua sets the step variable to this call's step (the transition into it)."""
    allcalls = []

    def walk(fn):
        cs, _ = analyse(fn)
        allcalls.extend(cs)
        for sub in fn.protos:
            walk(sub)
    walk(f)
    setsteps = []
    for c in allcalls:
        if c['fn'] == 'yMatch.CallFunction' and len(c['args']) == 3 and c['args'][0] == '267' and c['args'][2] == '0':
            try:
                setsteps.append((int(c['args'][1]), [pretty(g) for g in c['guards']]))
            except ValueError:
                pass
    rows, pre = [], []
    for c in allcalls:
        if c['fn'] != 'yMatch.CallFunction' or not c['args']:
            continue
        if c['args'][-1] == '1002' and len(c['args']) == 2:
            pre.append(c['args'][0])
        if c['args'][-1] == '1003' and len(c['args']) == 6:
            gs = [pretty(g) for g in c['guards']]
            step = None
            for g in gs:
                m = re.fullmatch(r'step == (\d+)', g)
                if m:
                    step = int(m.group(1))
            entry = []
            for tgt, g in setsteps:
                if tgt == step:
                    entry.append([x for x in g if not re.fullmatch(r'step == \d+', x)])
            rows.append(dict(pc=c['pc'], bank=c['args'][0], flag=c['args'][1], a3=c['args'][2], a4=c['args'][3], a5=c['args'][4],
                             step=step, guards=gs, entry=entry))
    return rows, pre


def print_scenes(rec, f, p=print):
    rows, pre = scene_rows(f)
    p('record %d: preloads %s' % (rec, ', '.join(sorted(set(pre), key=lambda x: (len(x), x))) or 'none'))
    for r in rows:
        p('  play bank %s flag %s step %s  extra-guards %s' % (r['bank'], r['flag'], r['step'],
                                                              [g for g in r['guards'] if not re.fullmatch(r'step == \d+', g)] or '-'))
        for e in r['entry']:
            p('     entered when: %s' % (' and '.join(e) or '(unconditional)'))


# ---------------------------------------------------------------- selftest: a hand-built chunk, no compiler, no game bytes

def _s(text):
    b = text.encode() + b'\x00'
    return struct.pack('<I', len(b)) + b


def _ins_abc(op, a, b, c):
    return struct.pack('<I', op | (a << 6) | (c << 14) | (b << 23))


def _ins_abx(op, a, bx):
    return struct.pack('<I', op | (a << 6) | (bx << 14))


def build_known(bad_tag=False):
    """The chunk of `Foo.Bar(7, "hi")`-style source, written by hand from the spec:
        GETGLOBAL R0 K0("yMatch"); GETTABLE R0 R0 K1("CallFunction"); LOADK R1 K2(1090); LOADK R2 K3(1003);
        CALL R0 3 1; RETURN R0 1
    """
    head = b'\x1bLua\x51\x00\x01\x04\x04\x04\x04\x00'
    code = [_ins_abx(5, 0, 0), _ins_abc(6, 0, 0, 256 + 1), _ins_abx(1, 1, 2), _ins_abx(1, 2, 3), _ins_abc(28, 0, 3, 1), _ins_abc(30, 0, 1, 0)]
    consts = [b'\x04' + _s('yMatch'), b'\x04' + _s('CallFunction'), b'\x03' + struct.pack('<f', 1090.0), b'\x03' + struct.pack('<f', 1003.0)]
    if bad_tag:
        consts[2] = b'\x07' + struct.pack('<f', 1090.0)    # 7 is not a Lua 5.1 constant type
    body = struct.pack('<I', 0)                              # source name: none
    body += struct.pack('<II', 0, 0) + bytes([0, 0, 0, 3])  # lines, nups, params, vararg, maxstack
    body += struct.pack('<I', len(code)) + b''.join(code)
    body += struct.pack('<I', len(consts)) + b''.join(consts)
    body += struct.pack('<I', 0)                             # protos
    body += struct.pack('<I', 0) + struct.pack('<I', 0) + struct.pack('<I', 0)   # lineinfo, locvars, upvalue names
    return head + body


EXPECT = '''function main: lines 0-0, 6 instr, 4 consts, 0 protos, params 0
  K0    str  yMatch
  K1    str  CallFunction
  K2    num  1090
  K3    num  1003
     0  GETGLOBAL 0 0
     1  GETTABLE  0 0 257
     2  LOADK     1 2
     3  LOADK     2 3
     4  CALL      0 3 1
     5  RETURN    0 1 0
  call @4: yMatch.CallFunction(1090, 1003)'''


def selftest():
    import io
    ok = True
    buf = io.StringIO()
    listing(parse(build_known()), consts=True, calls=True, dis=True, out=lambda s: buf.write(s + '\n'))
    got = buf.getvalue().rstrip('\n')
    if got != EXPECT:
        print('SELFTEST FAIL: listing differs from the expected text')
        print(got)
        ok = False
    else:
        print('selftest: hand-built chunk lists exactly as expected')
    # plant: a wrong constant-type tag must fail
    try:
        parse(build_known(bad_tag=True))
        print('SELFTEST FAIL: the planted bad constant tag was accepted')
        ok = False
    except LuaError as e:
        print('selftest: planted bad constant tag rejected (%s)' % e)
    # plant: a wrong signature and a truncated chunk must fail too
    for name, blob in (('bad signature', b'XXXX' + build_known()[4:]), ('truncated', build_known()[:-5])):
        try:
            parse(blob)
            print('SELFTEST FAIL: %s accepted' % name)
            ok = False
        except LuaError:
            print('selftest: %s rejected' % name)
    print('SELFTEST PASS' if ok else 'SELFTEST FAILED')
    return 0 if ok else 1


# ---------------------------------------------------------------- era pac access

def era_chunks(era):
    """{record id: chunk bytes} of evtdsc<era>.pac (the stub slots of 394 bytes are skipped)."""
    import pac
    path = os.path.join(pac.GAME, 'pac', 'evtdsc', 'evtdsc%d.pac' % era)
    if not os.path.isfile(path):
        raise SystemExit('missing %s (game_data\\extracted is the player\'s own files)' % path)
    data = pac.load(path)
    out = {}
    for name, dec in pac.entries(data):
        top, _, eid = name.rpartition('/')
        if top.startswith('p') and top[1:].isdigit() and int(top[1:]) >= 1 and dec[:5] == b'\x1bLuaQ':
            out[era * 100 + int(top[1:]) - 1] = dec
    if not out:
        raise SystemExit('no Lua chunks found in %s' % path)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('chunk', nargs='?')
    ap.add_argument('--era', type=int)
    ap.add_argument('--rec', type=int)
    ap.add_argument('--consts', action='store_true')
    ap.add_argument('--dis', action='store_true')
    ap.add_argument('--calls', action='store_true', default=True)
    ap.add_argument('--no-guards', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--scenes', action='store_true', help='per record: every 1003 (play scene) call, its step and the conditions that enter that step')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.era is not None:
        for rec, blob in sorted(era_chunks(a.era).items()):
            if a.rec is not None and rec != a.rec:
                continue
            if len(blob) <= 394:
                continue
            if a.scenes:
                print_scenes(rec, parse(blob))
                continue
            print('== record M%03d (%d bytes)' % (rec, len(blob)))
            listing(parse(blob), consts=a.consts, dis=a.dis, guards=not a.no_guards)
        return 0
    if not a.chunk:
        ap.error('give a chunk file, --era, or --selftest')
    listing(parse(open(a.chunk, 'rb').read()), consts=a.consts, dis=a.dis, guards=not a.no_guards)
    return 0


if __name__ == '__main__':
    sys.exit(main())
