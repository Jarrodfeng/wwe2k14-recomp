"""Self-test of `q decomp` (S103 lane QD): `py -I tools/q/decomp_selftest.py`   (0 game runs)

A. Synthetic units: tiny listings written in a TEMP folder (our own invented instruction sequences, not game code)
   with the exact event / text each construct must give: if-skip, if/else, do-while loop, lis+addi / lis+lwz base
   folding, spill and prologue-save handling, call arguments and results, a vtable call, a switch, rlwinm decode,
   image float decode, the cache (hit, rebuild when the source's hash changes).
B. The real fixture (tools/q/decomp_fixture.json; field offsets, call targets and constants that were checked by hand
   against the listing; they are our annotations, not game code): four real functions, assertions over the freshly
   built output. A machine without the generated code / the q index SKIPs this part.
C. Planted twins: the same checks against a WRONG expectation (an offset, a call target, a constant, a float) must
   FAIL, in both the synthetic and the real part: a checker that cannot fail is not a checker.
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import json
import os
import re
import shutil
import sqlite3
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qdecomp  # noqa: E402
import qdecomp_phi  # noqa: E402

FIXTURE = os.path.join(HERE, 'decomp_fixture.json')
FAILS = []


def check(name, cond, detail=''):
    if not cond:
        FAILS.append(f'{name}: {detail}')
        print(f'FAIL {name} {detail}')
    else:
        print(f'ok   {name}')


# ------------------------------------------------------------------ synthetic listing builder
def build_listing(base, items, name=None):
    """items: ['mn ops', ('label', 'L1'), ('goto-case', ...)]. Returns (text, addr_of_label)."""
    name = name or f'sub_{base:08X}'
    addr = base
    labels = {}
    for it in items:
        if isinstance(it, tuple) and it[0] == 'label':
            labels[it[1]] = addr
        elif isinstance(it, str):
            addr += 4
    out = [f'DEFINE_REX_FUNC({name}) {{', '\tREX_FUNC_PROLOGUE();']
    addr = base
    for it in items:
        if isinstance(it, tuple) and it[0] == 'label':
            out.append(f'loc_{addr:08X}:')
            continue
        mn, _, ops = it.partition(' ')
        for k, v in labels.items():          # branch targets by label name
            ops = ops.replace(f'<{k}>', f'0x{v:08x}')
        out.append(f'\t// {mn} {ops}')
        out.append('\tctx.r0.u64 = 0;')
        if mn == 'bctr' and it in CASES:
            out.append('\tswitch (ctx.r11.u32) {')
            for i, lab in enumerate(CASES[it]):
                out.append(f'\tcase {i}:')
                out.append(f'\t\tgoto loc_{labels[lab]:08X};')
            out.append('\t}')
        addr += 4
    out.append('}')
    return '\n'.join(out) + '\n', labels


CASES = {}


class Env:
    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix='qdecomp_selftest_')
        self.gen = os.path.join(self.dir, 'gen')
        self.cache = os.path.join(self.dir, 'cache')
        os.makedirs(self.gen)
        self.image = os.path.join(self.dir, 'image.bin')
        img = bytearray(0x1000)
        struct.pack_into('>f', img, 0x100, 1.0 / 600)      # 0x82000100
        struct.pack_into('>I', img, 0x104, 0x1234)         # 0x82000104
        with open(self.image, 'wb') as f:
            f.write(img)
        self.con = sqlite3.connect(':memory:')
        self.con.row_factory = sqlite3.Row
        self.con.execute('CREATE TABLE funcs (addr INTEGER, name TEXT, size INTEGER, insns INTEGER, file TEXT, line INTEGER,'
                         ' n_bctrl INTEGER, n_bctr INTEGER, resync INTEGER, has_body INTEGER)')
        self.saved = (qdecomp.GEN, qdecomp.CACHE_DIR, qdecomp.IMAGE, qdecomp._IMG, dict(qdecomp._SHA))
        qdecomp.GEN, qdecomp.CACHE_DIR, qdecomp.IMAGE, qdecomp._IMG = self.gen, self.cache, self.image, None
        qdecomp._SHA.clear()
        self.nfile = 0

    def name(self, addr, name):
        self.con.execute('INSERT INTO funcs VALUES (?,?,0,0,NULL,0,0,0,0,0)', (addr, name))

    def add(self, base, items, name=None):
        text, labels = build_listing(base, items, name)
        self.nfile += 1
        fname = f'wwe2k14_recomp.{self.nfile}.cpp'
        with open(os.path.join(self.gen, fname), 'w', encoding='utf-8') as f:
            f.write(text)
        n = sum(1 for i in items if isinstance(i, str))
        self.con.execute('INSERT INTO funcs VALUES (?,?,?,?,?,?,0,0,0,1)',
                         (base, name or f'sub_{base:08X}', n * 4, n, fname, 1))
        return fname, labels

    def get(self, base, use_cache=False):
        return qdecomp.decompile(self.con, base, use_cache=use_cache)

    def close(self):
        qdecomp.GEN, qdecomp.CACHE_DIR, qdecomp.IMAGE, qdecomp._IMG = self.saved[:4]
        qdecomp._SHA.clear()
        qdecomp._SHA.update(self.saved[4])
        shutil.rmtree(self.dir, ignore_errors=True)


def rw(r, kind):
    return {(x['addr'], x['w']) for x in r['summary'][kind]}


def text(r):
    return '\n'.join(r['lines'])


# ------------------------------------------------------------------ A: synthetic
def synthetic(e):
    # 1. if-skip: a conditional forward branch becomes `if (!cond) { ... }`; reads/writes with the base register named
    e.add(0x82001000, ['lwz r11,112(r3)', 'cmpwi cr6,r11,0', 'beq cr6,<L>', 'lwz r10,8(r3)', 'stw r10,16(r3)',
                       ('label', 'L'), 'li r3,1', 'blr'])
    r = e.get(0x82001000)
    check('if-skip reads', rw(r, 'reads') == {('in_r3+112', 'u32'), ('in_r3+8', 'u32')}, str(rw(r, 'reads')))
    check('if-skip writes', rw(r, 'writes') == {('in_r3+16', 'u32')}, str(rw(r, 'writes')))
    check('if-skip structure', 'if ([in_r3+112] != 0) {' in text(r), text(r))
    check('if-skip return value', 'return r3=1' in text(r), text(r))
    check('if-skip store value', '[in_r3+16] = [in_r3+8]  (u32)' in text(r), text(r))

    # 2. if / else join
    e.add(0x82001100, ['cmpwi cr6,r4,5', 'bge cr6,<E>', 'li r3,10', 'b <J>', ('label', 'E'), 'li r3,20', ('label', 'J'),
                       'blr'])
    r = e.get(0x82001100)
    t = text(r)
    check('if/else structure', 'if (in_r4 < 5) {' in t and '} else {' in t, t)
    check('if/else join value', 'return r3=r3@' in t, t)      # r3 differs per path: a join value, not a guess

    # 3. do-while loop with a counter: the counter is a loop value
    e.add(0x82001200, ['li r30,0', ('label', 'L'), 'addi r30,r30,1', 'cmpw cr6,r30,r4', 'blt cr6,<L>', 'mr r3,r30',
                       'blr'])
    r = e.get(0x82001200)
    t = text(r)
    check('loop structure', 'do {' in t and '} while (r30@' in t and '< in_r4);' in t, t)
    check('loop join value', 'return r3=r30@' in t, t)

    # 4. lis + addi / lis + lwz: absolute addresses and base provenance
    e.add(0x82001300, ['lis r11,-31853', 'lwz r10,80(r11)', 'lfs f0,32(r10)', 'stfs f0,4(r3)', 'addi r9,r11,-9160',
                       'lfs f1,60(r9)', 'blr'])
    r = e.get(0x82001300)
    check('lis+lwz global read', ('0x83930050', 'u32') in rw(r, 'reads'), str(rw(r, 'reads')))
    check('provenance fold read', ('[0x83930050]+32', 'f32') in rw(r, 'reads'), str(rw(r, 'reads')))
    check('lis+addi absolute', ('0x8392DC74', 'f32') in rw(r, 'reads'), str(rw(r, 'reads')))
    check('float store', ('in_r3+4', 'f32') in rw(r, 'writes'), str(rw(r, 'writes')))

    # 5. prologue saves, spill + reload forwarding, frame size
    e.add(0x82001400, ['mflr r12', 'stw r12,-8(r1)', 'std r30,-24(r1)', 'stwu r1,-96(r1)', 'mr r30,r3', 'stw r4,80(r1)',
                       'lwz r5,80(r1)', 'lwz r6,0(r5)', 'stw r6,12(r30)', 'addi r1,r1,96', 'lwz r12,-8(r1)', 'mtlr r12',
                       'ld r30,-24(r1)', 'blr'])
    r = e.get(0x82001400)
    s = r['summary']
    check('frame size', s['frame'] == 96, str(s['frame']))
    check('saves', s['saves'] == ['lr', 'r30'], str(s['saves']))
    check('spill reload forwarded', rw(r, 'reads') == {('in_r4', 'u32')}, str(rw(r, 'reads')))
    check('store through saved reg', rw(r, 'writes') == {('in_r3+12', 'u32')}, str(rw(r, 'writes')))
    check('spill store shown as local', 'local[sp+80] = in_r4' in text(r), text(r))

    # 6. calls: names, arguments (fresh registers + r3), result, a store of the result
    e.name(0x82009000, 'helper_fn')
    e.add(0x82001500, ['mr r30,r3', 'li r3,5', 'bl 0x82009000', 'stw r3,0(r30)', 'blr'])
    r = e.get(0x82001500)
    t = text(r)
    check('call args', 'call#8 helper_fn(r3=5)' in t, t)
    check('call result stored', '[in_r3] = ret#8  (u32)' in t, t)
    check('call summary', r['summary']['calls'] == {'helper_fn': 1}, str(r['summary']['calls']))
    check('call site args', r['summary']['sites']['82009000'][0]['args'] == ['r3=5'],
          str(r['summary']['sites']))

    # 7. vtable call
    e.add(0x82001600, ['lwz r11,0(r3)', 'lwz r10,24(r11)', 'mtctr r10', 'bctrl', 'blr'])
    r = e.get(0x82001600)
    check('vcall target', 'vcall#C *([[in_r3]+24])(r3=in_r3, r10=[[in_r3]+24])' in text(r), text(r))

    # 8. switch via bctr
    sw = 'bctr '
    CASES[sw] = ['A', 'B']
    e.add(0x82001700, ['cmplwi cr6,r4,1', 'bgt cr6,<D>', 'mr r11,r4', 'bctr ', ('label', 'A'), 'li r3,1', 'blr',
                       ('label', 'B'), 'li r3,2', 'blr', ('label', 'D'), 'li r3,0', 'blr'])
    r = e.get(0x82001700)
    check('switch event', 'switch (' in text(r) and text(r).count('@') >= 3, text(r))
    del CASES[sw]

    # 9. rlwinm / clrlwi decode and an indexed access
    e.add(0x82001800, ['rlwinm r11,r4,4,0,27', 'clrlwi r10,r4,16', 'lwzx r9,r3,r11', 'stwx r9,r3,r10', 'blr'])
    r = e.get(0x82001800)
    check('rlwinm decode read', ('(in_r4 << 4)', 'u32') in {(a.replace('in_r3+', ''), w) for a, w in rw(r, 'reads')} or
          ('in_r3+(in_r4 << 4)', 'u32') in rw(r, 'reads') or ('(in_r3 + (in_r4 << 4))', 'u32') in rw(r, 'reads'),
          str(rw(r, 'reads')))
    check('clrlwi decode write', any('(in_r4 & 65535)' in a for a, w in rw(r, 'writes')), str(rw(r, 'writes')))

    # 10. image float decode: 0x82000100 holds 1/600, 0x82000104 an int
    e.add(0x82001900, ['lis r11,-32256', 'lfs f0,256(r11)', 'lwz r10,260(r11)', 'fmuls f1,f1,f0', 'stw r10,0(r3)',
                       'blr'])
    r = e.get(0x82001900)
    t = text(r)
    check('float decode', '= 0.0016666667 (1/600) (image)' in t, t)
    check('int decode', '= 4660 (image)' in t, t)

    # 11. cache: a hit returns the stored result; a changed source (new hash) rebuilds
    e.add(0x82001A00, ['lwz r11,4(r3)', 'blr'])
    e.get(0x82001A00, use_cache=True)
    files = os.listdir(e.cache)
    check('cache written', any(f.startswith('82001A00.') and f.endswith('.json') for f in files), str(files))
    row = e.con.execute('SELECT file FROM funcs WHERE addr = ?', (0x82001A00,)).fetchone()
    p = os.path.join(e.gen, row['file'])
    cached = e.get(0x82001A00, use_cache=True)
    check('cache hit', cached['source_sha1'] == qdecomp.file_sha1(p))
    with open(p, 'a', encoding='utf-8') as f:
        f.write('// edited\n')
    after = e.get(0x82001A00, use_cache=True)
    check('cache rebuilt on source change', after['source_sha1'] != cached['source_sha1'])
    n82 = [f for f in os.listdir(e.cache) if f.startswith('82001A00.')]
    check('stale cache entry removed', len(n82) == 1, str(n82))
    check('cache key holds qdecomp\'s own code hash', len(n82) == 1 and qdecomp._code_sha()[:10] in n82[0], str(n82))

    # 12. a join of an `add` and a plain value: the join value is defined, with BOTH inputs (S110 QDEC case A)
    fname, lab = e.add(0x82001B00, ['lwz r10,8(r3)', 'lwz r11,12(r3)', 'cmpwi cr6,r4,0', 'beq cr6,<L>',
                                    'add r11,r10,r11', ('label', 'L'), 'stw r11,0(r5)', 'blr'])
    r = e.get(0x82001B00)
    t = text(r)
    check('join defines its value with both inputs',
          f'r11@{lab["L"]:08X} = one of:' in t and '([in_r3+8] + [in_r3+12])' in t and '[in_r3+12]' in t, t)
    check('join: no printed name is undefined', qdecomp_phi.undefined(r['lines']) == [], str(qdecomp_phi.undefined(r['lines'])))

    # 13. an argument set in a predecessor block reaches the call in the join block
    e.add(0x82001C00, ['li r4,7', 'cmpwi cr6,r5,0', 'beq cr6,<L>', 'li r6,9', ('label', 'L'), 'bl 0x82009000', 'blr'])
    r = e.get(0x82001C00)
    check('argument set in a predecessor block', any('call#' in l and 'r4=7' in l for l in r['lines']), text(r))
    # ... but a register set on only ONE path to the call is not printed as an argument (it may be stale)
    check('register set on one path only is not an argument',
          any('call#' in l and 'r4=7' in l and 'r6=' not in l for l in r['lines']), text(r))

    # 14. the carry: `addic rX,rY,-1` then `subfe rZ,rZ,rZ` is (rY != 0) - 1, never a `?r`
    e.add(0x82001D00, ['bl 0x82009000', 'mr r30,r3', 'addic r9,r3,-1', 'subfe r8,r8,r8', 'stw r8,0(r30)', 'blr'])
    r = e.get(0x82001D00)
    t = text(r)
    check('addic + subfe is (r != 0) - 1', '((ret#0 != 0) - 1)' in t and '?r' not in t, t)
    # ... and a carry that is not known is marked, with real listing lines
    e.add(0x82001D80, ['subfe r8,r4,r5', 'stw r8,0(r3)', 'blr'])
    r = e.get(0x82001D80)
    check('unknown carry is marked with listing lines', '?carry (not modelled))  (u32)  ?(simplified: see listing :3-:5)' in text(r)
          and r['summary']['simplified'] == 1, text(r))
    # a block with no known predecessor says so instead of printing the entry value
    e.add(0x82001E00, ['b <E>', ('label', 'D'), 'mr r3,r7', 'blr', ('label', 'E'), 'li r3,0', 'blr'])
    r = e.get(0x82001E00)
    check('no known predecessor is marked', 'return r3=?r7 (no known predecessor), f1=' in text(r)
          and '?(simplified: see listing :6-:8)' in text(r)
          and 'in_r7' not in text(r), text(r))

    # 15. a join input that is ANOTHER register's join value stays that name: the other register's predecessors and
    # values are not r4's (S110 QDECFIX review: r4@82842B24 listed r29's blocks as if r4 held -1 / 1337 there)
    fname, lab = e.add(0x82001F00, ['li r29,5', 'cmpwi cr6,r4,0', 'beq cr6,<A>', 'li r29,6', ('label', 'A'),
                                    'li r4,-1', 'cmpwi cr6,r5,0', 'beq cr6,<B>', 'mr r4,r29', ('label', 'B'),
                                    'stw r4,0(r3)', 'blr'])
    r = e.get(0x82001F00)
    a_, b_ = lab['A'], lab['B']
    r4line = [l for l in r['lines'] if f'r4@{b_:08X} = one of:' in l]
    check('cross-register join input keeps its own name',
          len(r4line) == 1 and f'@{b_ - 4:08X} r29@{a_:08X}' in r4line[0] and ' 5' not in r4line[0]
          and qdecomp_phi.undefined(r['lines']) == [], text(r))

    # 16. `+N more not shown: see listing :A-:B` covers the blocks of the hidden inputs, not only the direct predecessors
    items = []
    for k in range(13):
        items += [f'cmpwi cr6,r4,{k}', f'li r9,{k + 100}', 'beq cr6,<J>']
    items += ['li r9,50', ('label', 'J'), 'cmpwi cr6,r5,0', 'beq cr6,<K>', 'li r9,99', ('label', 'K'), 'stw r9,0(r3)', 'blr']
    e.add(0x82002000, items)
    r = e.get(0x82002000)
    t = text(r)
    # 13 + 2 inputs, 12 shown: the first hidden one is the 13th compare's block @82002090 (a predecessor of @820020A0,
    # not of the join @820020AC whose direct predecessors start at :84)
    hidden = int(re.search(r'@82002090  \[listing :(\d+)\]', t).group(1))
    more = re.search(r'more not shown: see listing :(\d+)-:(\d+)', t)
    check('"more not shown" range reaches the hidden inputs', more is not None and int(more.group(1)) <= hidden, t)


# ------------------------------------------------------------------ planted twins (synthetic)
def planted_synthetic(e):
    """The same assertions against a wrong expectation must report a failure."""
    r = e.get(0x82001000)
    caught = 0
    for name, ok in [
        ('wrong offset (112 -> 116)', rw(r, 'reads') == {('in_r3+116', 'u32'), ('in_r3+8', 'u32')}),
        ('wrong width (u32 -> u16)', rw(r, 'reads') == {('in_r3+112', 'u16'), ('in_r3+8', 'u16')}),
        ('wrong write base (r3 -> r4)', rw(r, 'writes') == {('in_r4+16', 'u32')}),
    ]:
        if ok:
            print(f'NOT CAUGHT planted twin: {name}')
        else:
            caught += 1
            print(f'ok   planted twin caught: {name}')
    r = e.get(0x82001500)
    for name, ok in [('wrong call target', r['summary']['calls'] == {'other_fn': 1}),
                     ('wrong call arg', r['summary']['sites']['82009000'][0]['args'] == ['r3=6'])]:
        if ok:
            print(f'NOT CAUGHT planted twin: {name}')
        else:
            caught += 1
            print(f'ok   planted twin caught: {name}')
    return caught == 5


# ------------------------------------------------------------------ B: the real fixture
def expectation_ok(r, ex):
    """None when the decomp `r` meets the hand-checked expectation `ex`, else a message."""
    s = r['summary']
    reads = {(x['addr'], x['w']) for x in s['reads']}
    writes = {(x['addr'], x['w']) for x in s['writes']}
    for a, w in ex.get('reads', []):
        if (a, w) not in reads:
            return f'missing read [{a}] {w}; have {sorted(reads)}'
    for a, w in ex.get('writes', []):
        if (a, w) not in writes:
            return f'missing write [{a}] {w}; have {sorted(writes)}'
    if 'exact_writes' in ex and sorted(map(tuple, ex['exact_writes'])) != sorted(writes):
        return f'writes {sorted(writes)} != {ex["exact_writes"]}'
    targets = {k for k in s['sites']}
    for t in ex.get('call_targets', []):
        if t.upper().replace('0X', '') not in targets:
            return f'missing call target {t}; have {sorted(targets)}'
    if 'n_call_targets' in ex and len(targets) != ex['n_call_targets']:
        return f'{len(targets)} call targets, expected {ex["n_call_targets"]}'
    for c in ex.get('constants', []):
        if not any(c in l for l in r['lines']) and c not in s['consts'] and c not in s['globals']:
            return f'constant {c} not shown'
    for fl in ex.get('floats', []):
        if not any(f'[{fl[0]}]' in l and f'= {fl[1]}' in l for l in r['lines']):
            return f'float {fl} not decoded'
    for sub in ex.get('text', []):
        if sub not in '\n'.join(r['lines']):
            return f'text {sub!r} missing'
    for prefix, subs in ex.get('line_has', []):         # one line holding the prefix AND every word
        if not any(prefix in l and all(x in l for x in subs) for l in r['lines']):
            return f'no line with {prefix!r} holds all of {subs}'
    if ex.get('defined'):                               # the general rule: every printed name is defined, none marked away
        bad = qdecomp_phi.undefined(r['lines'])
        if bad:
            return f'printed names with no definition: {bad[:6]}'
    else:
        bad = qdecomp_phi.undefined(r['lines'], marked_ok=True)
        if bad:
            return f'printed names with no definition and no marker: {bad[:6]}'
    return None


def real_fixture():
    if not os.path.exists(os.path.join(qdecomp.GEN, 'codegen.stamp')) or not os.path.exists(os.path.join(qdecomp.REPO, 'out', 'harness', 'index', 'index.db')):
        print('SKIP real fixture: generated code or the q index is not on this machine')
        return True
    import q
    con = q.connect(q.DB)
    with open(FIXTURE, encoding='utf-8') as f:
        fx = json.load(f)
    caught = True
    for ex in fx['functions']:
        a = int(ex['func'][4:], 16)
        f = con.execute('SELECT * FROM funcs WHERE addr = ?', (a,)).fetchone()
        r = qdecomp.decompile_uncached(con, f, os.path.join(qdecomp.GEN, f['file']), 'x')
        msg = expectation_ok(r, ex)
        check(f'fixture {ex["func"]}', msg is None, msg or '')
        # planted twin: break one field of the expectation, the check must now fail
        for key, bad in ex.get('plant', {}).items():
            twin = json.loads(json.dumps(ex))
            if key == 'read_offset':
                twin['reads'][0][0] = bad
            elif key == 'call_target':
                twin['call_targets'][0] = bad
            elif key == 'constant':
                twin['constants'][0] = bad
            elif key == 'float':
                twin['floats'][0][1] = bad
            elif key == 'line_has':
                twin['line_has'][0][1][0] = bad
            if expectation_ok(r, twin) is None:
                print(f'NOT CAUGHT planted twin in {ex["func"]}: {key}')
                caught = False
            else:
                print(f'ok   planted twin caught in {ex["func"]}: {key}')
    return caught


def main():
    e = Env()
    try:
        synthetic(e)
        caught = planted_synthetic(e)
    finally:
        e.close()
    caught = real_fixture() and caught
    if FAILS:
        print(f'\n{len(FAILS)} check(s) FAILED')
        return 1
    if not caught:
        print('\na planted twin was not caught')
        return 2
    print('\nall decomp checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
