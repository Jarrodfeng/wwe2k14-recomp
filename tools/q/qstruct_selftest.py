"""Self-test of `q struct` / `q class` (S111 STRUCTREC): `py -I tools/q/qstruct_selftest.py [--no-real]`   (0 game runs)

A. Planted listings (our own invented instruction sequences in a TEMP folder, written by decomp_selftest.build_listing; no game code),
   read by the production path (qslice.build_facts over qdecomp's interpreter, then qstruct.struct_of / class_of), each check with its
   twin: the same check against a WRONG expectation must FAIL (a checker that cannot fail is not a checker). The planted class
   (vtable 0x82200000, 4 slots) has:
     a constructor that calls its base's constructor (vtable 0x82200200) on this first, stores its vptr at +0, a subobject vptr
       (0x82200100) at +32 and 7 at +8; its caller allocates 2600 bytes and keeps the object in the global word 0x83010200
     a record array read by a loop at +2464 + 48*k and written by an unrolled loop (stride 96: +2464 and +2512) plus a store 4 bytes
       off (+2468 + 96*k), which is NOT the same field
     a context-indexed word [this + 1388 + 5292*[this+268]] beside a loop-walked +800 + 588*k array (1388 = 800 + 588 and 588
       divides 5292: the context row must stay its own row, with its index)
     a halfword read through extsh (signed), a store of 5 at +16, a direct callee reading [this'+40] (bound through the call),
     the subobject's own slot reading its [this'+4] (= +36 of the class), a reader through the global word (+20)
   and a destructor body (stores the vptr, then calls the base's) and an unrelated vtable holding two of our slots at other indexes.
B. The real re-finds on the local tree (MATSIZE / BLOODMOD2 / SLICER's splat pool, M100_Q10's SmartMenu scroll, PLACECOUNT0's placement
   table header) and `q class` of the splat pool, each with its wall time and a planted wrong expectation that must be absent. A
   machine without generated/ or the q index SKIPs part B.
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import contextlib
import io
import os
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qdecomp  # noqa: E402
import qslice  # noqa: E402
import qstruct  # noqa: E402
from decomp_selftest import build_listing  # noqa: E402  the planted-listing writer qdecomp's own selftest uses

FAILS, TWINS_MISSED = [], []


def check(name, cond, detail=''):
    if not cond:
        FAILS.append(f'{name}: {detail}')
        print(f'FAIL {name} {detail}')
    else:
        print(f'ok   {name}')


def twin(name, cond):
    """a planted wrong expectation: cond is the check run against it; it must be False"""
    if cond:
        TWINS_MISSED.append(name)
        print(f'TWIN NOT CAUGHT {name}')
    else:
        print(f'ok   twin {name} (the wrong expectation fails)')


class PlantSource(qslice.Source):
    """q struct's outside world from planted listings: functions, call edges, vtables (slot lists), the global's users / writers"""

    def __init__(self, tmp):
        self.tmp, self.n = tmp, 0
        self.funcs, self.edges, self.slots, self.gf, self.kw, self._f = {}, [], {}, {}, {}, {}
        self.gl = {}
        self.read = 0

    def add(self, base, items, name=None):
        text, _labels = build_listing(base, items, name)
        self.n += 1
        path = os.path.join(self.tmp, f'plant.{self.n}.cpp')
        with open(path, 'w', encoding='utf-8') as fh:
            fh.write(text)
        self.funcs[base] = {'addr': base, 'name': name or f'sub_{base:08X}', 'path': path}
        return base

    def edge(self, src, site, dst):
        self.edges.append((src, site, dst, 'call'))

    def func(self, addr):
        return self.funcs.get(addr)

    def is_func(self, a):
        return a in self.funcs

    def facts(self, addr):
        if addr not in self._f:
            f = self.funcs.get(addr)
            if f is None:
                self._f[addr] = None
            else:
                insns, labels, _a, _b = qdecomp.read_listing(f['path'], 1, f['name'], addr)
                names = lambda a: self.funcs[a]['name'] if a in self.funcs else None  # noqa: E731
                self._f[addr] = qslice._decode(qslice.json.loads(qslice.json.dumps(
                    qslice.build_facts(f['name'], addr, insns, labels, names, self))))
                self.read += 1
        return self._f[addr]

    def callers(self, addr):
        return [(s, site, k) for s, site, d, k in self.edges if d == addr and s != addr]

    def word(self, a):
        return None

    def is_vt(self, a):
        return bool(self.slots.get(a))

    def vt_slot_list(self, vt):
        return list(self.slots.get(vt, ()))

    def vslots(self, addr):
        return [(vt, i) for vt, fs in sorted(self.slots.items()) for i, f in enumerate(fs) if f == addr]

    def _vptr_at_this(self, addr):
        """the vtables a function stores at this+0 (what the field index's objtype `vptr` rows give a constructor's a0)"""
        T = self.facts(addr)
        if T is None:
            return set()
        return {s['val'][1] for s in T['stores'] if s['val'][0] == 'c' and s['af'] == ('in', 'r3') and self.is_vt(s['val'][1])}

    def vt_store_funcs(self, vt):
        """the functions that store the constant vt anywhere (QSource: the field index's vptr stores + the code's immediates)"""
        out = set()
        for f in self.funcs:
            T = self.facts(f)
            if T and any(s['val'] == ('c', vt) for s in T['stores']):
                out.add(f)
        return out

    def arg_types(self, addr, reg):
        if reg != 'r3':
            return set()
        return {vt for vt, _i in self.vslots(addr)} | self._vptr_at_this(addr)

    def type_funcs(self, vts):
        return {f for vt in vts for f in self.slots.get(vt, ())}

    def k_writers(self, a, g):
        return set(self.kw.get(a, ()))

    def g_funcs(self, a):
        return set(self.gf.get(a, ()))

    def g_loaders(self, a):
        return set(self.gl.get(a, ()))


VT, SUBVT, BASEVT, OTHERVT = 0x82200000, 0x82200100, 0x82200200, 0x82200300
G = 0x83010200
CTOR, MAKER, ALLOC, BASECTOR, DTORB = 0x82101000, 0x82102000, 0x82103000, 0x82101800, 0x82101900
S0, S1, S2, S3, T0, HELPER, USER = 0x82101100, 0x82101200, 0x82101300, 0x82101400, 0x82101500, 0x82101600, 0x82101700


def plant(S):
    S.add(BASECTOR, ['lis r11,-32224', 'addi r11,r11,512', 'stw r11,0(r3)', 'blr'])
    S.add(CTOR, ['mr r31,r3', f'bl 0x{BASECTOR:08x}', 'lis r11,-32224', 'addi r11,r11,0', 'stw r11,0(r31)', 'lis r10,-32224',
                 'addi r10,r10,256', 'stw r10,32(r31)', 'li r9,7', 'stw r9,8(r31)', 'mr r3,r31', 'blr'])
    S.edge(CTOR, CTOR + 4, BASECTOR)
    S.add(ALLOC, ['blr'])
    S.add(MAKER, ['li r3,2600', f'bl 0x{ALLOC:08x}', f'bl 0x{CTOR:08x}', 'lis r11,-31999', 'stw r3,512(r11)', 'blr'])
    S.edge(MAKER, MAKER + 4, ALLOC)
    S.edge(MAKER, MAKER + 8, CTOR)
    S.add(DTORB, ['mr r31,r3', 'lis r11,-32224', 'addi r11,r11,0', 'stw r11,0(r31)', 'mr r3,r31', f'bl 0x{BASECTOR:08x}', 'blr'])
    S.edge(DTORB, DTORB + 20, BASECTOR)
    S.add(HELPER, ['lwz r4,40(r3)', 'stw r4,0(r5)', 'blr'])
    S.add(S0, ['lwz r4,8(r3)', f'bl 0x{HELPER:08x}', 'blr'])
    S.edge(S0, S0 + 4, HELPER)
    S.add(S1, ['addi r27,r3,2464', ('label', 'L'), 'lfs f13,0(r27)', 'stfs f13,0(r4)', 'addi r27,r27,48', 'cmpw cr6,r27,r5',
               'blt cr6,<L>', 'lhz r6,12(r3)', 'extsh r6,r6', 'stw r6,0(r4)', 'blr'])
    S.add(S2, ['addi r11,r3,2468', ('label', 'L'), 'stfs f1,-4(r11)', 'stfs f2,44(r11)', 'stfs f3,0(r11)', 'addi r11,r11,96',
               'cmpw cr6,r11,r5', 'blt cr6,<L>', 'li r4,5', 'stw r4,16(r3)', 'blr'])
    S.add(S3, ['lwz r11,268(r3)', 'mulli r11,r11,5292', 'add r11,r11,r3', 'lwz r4,1388(r11)', 'stw r4,0(r5)',
               'addi r10,r3,800', ('label', 'L2'), 'lwz r9,0(r10)', 'stw r9,0(r5)', 'addi r10,r10,588', 'cmpw cr6,r10,r6',
               'blt cr6,<L2>', 'blr'])
    S.add(T0, ['lwz r4,4(r3)', 'stw r4,0(r5)', 'blr'])
    S.add(USER, ['lis r11,-31999', 'lwz r3,512(r11)', 'lwz r4,20(r3)', 'stw r4,0(r5)', 'blr'])
    S.slots[VT] = [S0, S1, S2, S3]
    S.slots[SUBVT] = [T0]
    S.slots[BASEVT] = [S0]
    S.slots[OTHERVT] = [S1, S0]
    S.gf[G] = {USER, MAKER}
    S.kw[G] = {MAKER}


def row(rows, off, stride=()):
    return next((r for r in rows if r['off'] == off and tuple(r['stride']) == tuple(stride)), None)


def planted():
    tmp = tempfile.mkdtemp(prefix='qstruct_selftest_')
    try:
        S = PlantSource(tmp)
        plant(S)
        t, rows, al, sc = qstruct.struct_of(S, f'0x{VT:08X}', claims=[])
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            qstruct.print_struct(t, rows, al, sc, 0.0)
        text = out.getvalue()
        print(text)
        # the record array: the loop reader and the unrolled writer are ONE row; the store 4 bytes off is its own row
        r = row(rows, 2464, (48,))
        check('array: +2464 + 48*k read by slot 1, written twice by the unrolled slot 2', r is not None and r['reads'] == 1 and r['writes'] == 2
              and any('s1:' in x for x in r['readers']) and any('s2:' in x for x in r['writers']) and 'f32' in r['kinds'], repr(r))
        twin('array: the unrolled +2464 + 96*k is a row of its own', row(rows, 2464, (96,)) is not None)
        twin('array: the store at +2468 + 96*k is the same field as +2464 + 48*k', row(rows, 2468, (96,)) is None)
        # the context-indexed word
        r = row(rows, 1388, (5292,))
        check('context: +1388 + 5292*k with its index [this+268]', r is not None and any('5292*[this+268]' in i for i in r['index']), repr(r))
        twin('context: +1388 as a fixed offset', row(rows, 1388) is not None)
        r8 = row(rows, 800, (588,))
        twin('context: the +1388 read joins the loop-walked +800 + 588*k row', r8 is not None and any(f'sub_{S3:08X}+0xC' in s for s in r8['sites']))
        check('loop array: +800 + 588*k', r8 is not None and r8['reads'] == 1, repr(r8))
        # the subobject and its own slot
        check('subobject: +32 vtable 0x82200100 stored by the ctor', (32, SUBVT, CTOR) in t.subs, repr(t.subs))
        r = row(rows, 36)
        check('subobject slot: its [this\'+4] is the class\'s +36', r is not None and any(x.startswith('+32.s0:') for x in r['readers']), repr(r))
        twin('subobject slot: read at the raw +4', row(rows, 4) is not None)
        # signed, constants, the callee, the global's reader
        r = row(rows, 12)
        check('signed: +12 is s16 (extsh)', r is not None and r['kinds'] == ['s16'], repr(r))
        twin('signed: +12 as u16', r is not None and 'u16' in r['kinds'])
        check('values: +8 = 7 (ctor), +16 = 5', (row(rows, 8) or {}).get('values') == ['7'] and (row(rows, 16) or {}).get('values') == ['5'])
        twin('values: +8 = 5', (row(rows, 8) or {}).get('values') == ['5'])
        r = row(rows, 40)
        check('callee: [this\'+40] of a direct callee, bound through the call', r is not None and any(f'via sub_{S0:08X}' in x for x in r['readers']), repr(r))
        twin('callee: the callee\'s read is not seen', r is None)
        r = row(rows, 20)
        check('global: the reader through [0x83010200]', r is not None and any(f'sub_{USER:08X}' in x for x in r['readers']), repr(r))
        # ctor / alloc / instances / names
        check('ctor and dtor body', (CTOR, 'ctor') in t.ctors and (DTORB, 'dtor body') in t.ctors, repr(t.ctors))
        twin('the dtor body called a ctor', (DTORB, 'ctor') in t.ctors)
        check('alloc: 2600 by the allocator', 'alloc: 2600 by sub_82103000 in sub_82102000' in text, text)
        twin('alloc: 7', 'alloc: 7 ' in text)
        check('instances: [0x83010200]', G in t.globals, repr(t.globals))
        check('generated names', (row(rows, 2464, (48,)) or {}).get('name') == 'a_0x9A0_x48_f32' and (row(rows, 32) or {}).get('name') == 'sub_0x20_vptr'
              and (row(rows, 0) or {}).get('name') == 'vptr', repr([r['name'] for r in rows]))
        claims = [{'kind': 'field', 'key': 'vt:0x82200000+16', 'mark': 'C', 'name': 'planted_count', 'claim': 'x'},
                  {'kind': 'field', 'key': 'vt:0x82200100+4', 'mark': 'L', 'name': 'planted_sub_word', 'claim': 'y'}]
        t2, rows2, _al, _sc = qstruct.struct_of(S, f'0x{VT:08X}', claims=claims)
        check('lodged names: [C] at +16, [L] through the subobject\'s key at +36', (row(rows2, 16) or {}).get('name') == '[C] planted_count'
              and (row(rows2, 36) or {}).get('name') == '[L] planted_sub_word', repr([(r['off'], r['name']) for r in rows2]))
        twin('lodged names: the subobject key at the raw +4', (row(rows2, 4) or {}).get('name') == '[L] planted_sub_word')
        # the other ways in: the global word, the constructor, the subobject's vtable
        for given in (f'g:0x{G:08X}', f'sub_{CTOR:08X}', f'0x{SUBVT:08X}'):
            t3, rows3, _a, _s = qstruct.struct_of(S, given, claims=[])
            check(f'root {given} is class 0x82200000', t3.root == ('type', (VT,)) and row(rows3, 1388, (5292,)) is not None, f'{t3.label} {t3.root}')
        # q class
        _t, cls = qstruct.class_of(S, f'0x{VT:08X}', names={S1: 'planted_reader'})
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            qstruct.print_class(cls, 0.0)
        ctext = out.getvalue()
        print(ctext)
        check('class: 4 slots with the db name', len(cls['slots']) == 4 and cls['slots'][1]['name'] == 'planted_reader', repr(cls['slots']))
        p = next((p for p in cls['parents'] if p['vtable'] == f'0x{BASEVT:08X}'), None)
        check('class: the base 0x82200200 (ctor calls its ctor on this first; its vtable is our prefix)', p is not None and 'first' in p['how']
              and 'vtable prefix' in p['how'] and len(cls['parents']) == 1, repr(cls['parents']))
        twin('class: the unrelated 0x82200300 (our slots at other indexes) is a parent', any(p['vtable'] == f'0x{OTHERVT:08X}' for p in cls['parents']))
        check('class: subobject +32 0x82200100, instance [0x83010200]', any(s['off'] == 32 and s['vtable'] == f'0x{SUBVT:08X}' for s in cls['subobjects'])
              and cls['globals'] == [f'0x{G:08X}'], repr(cls))
        _t, bcls = qstruct.class_of(S, f'0x{BASEVT:08X}', names={})
        check('class: 0x82200000 is built on the base (its ctor calls the base ctor on its this)', any(c['vtable'] == f'0x{VT:08X}' and c['off'] == 0 for c in bcls['children']), repr(bcls['children']))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


REAL = [
    # (name, verb, target, must have, must NOT have (the planted wrong expectation))
    ('splat pool (MATSIZE / BLOODMOD2 / SLICER)', 'struct', '0x82023298',
     ['q struct class 0x82023298', 'subobjects: +32 vtable 0x82023290', '+2464 + 48*k', 'a_0x9A0_x48_f32', 'ctors: sub_82697888 [ctor]',
      'alloc: 3904 by sub_82C2DCD8', 'instances in: [0x837689D4]'],
     '+2464  '),
    ('SmartMenu scroll per context (M100_Q10)', 'struct', '0x82037A18',
     ['q struct class 0x82037A74', '0x82037A18 is the vtable of the subobject at +20', '+1388 + 5292*k', 'k = 5292*[this+268]',
      'sub_8283EAD8 via sub_82840508'],
     '+1388  '),
    ('placement table header (PLACECOUNT0)', 'struct', 'g:0x837635B0',
     ['class 0x8201DE28', 'ctors: sub_8258C878 [ctor]', 'alloc: 40 by sub_82C2DCD8 in sub_8258CCC8', 'instances in: [0x837635B0]',
      'f_0x4_s16', 'f_0x1C_s16', 'f_0x8_u32', 'f_0x24_u32'],
     'f_0x4_u16'),
    ('splat pool class picture', 'class', '0x82023298',
     ['q class 0x82023298: 8 slots', '0x8206F294 [C]', 'on this first', '+32 0x82023290', '[0x837689D4]', 'sub_82697988 [dtor body]'],
     'sub_82697988 [ctor]'),
]


def real():
    try:
        import q as qmod
        con = qmod.connect(qmod.DB)
    except Exception as e:  # noqa: BLE001 - no q index on this machine: part B is skipped, said so
        print(f'SKIP B (no q index: {e})')
        return
    if not os.path.isdir(qdecomp.GEN):
        print('SKIP B (no generated/ tree)')
        return
    for name, verb, target, want, wrong in REAL:
        t0 = time.time()
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            if verb == 'struct':
                qstruct.main([target], con)
            else:
                qstruct.main_class([target], con)
        dt = time.time() - t0
        text = out.getvalue()
        print(f'== {name}: q {verb} {target}   ({dt:.1f} s)')
        print(text)
        miss = [w for w in want if w not in text]
        check(f'real: {name}', not miss, f'missing: {miss}')
        twin(f'real: {name}: {wrong!r}', wrong in text)


G2, GET, SET, C1, C2, C3 = 0x83010300, 0x82104000, 0x82104100, 0x82104200, 0x82104300, 0x82104400


def getters():
    """S111 QFIX5: a global object reached through a getter (returns [G2]) and written through a setter (`stw r4,8(r3)`): the callers' stores and the setter's
    store are G2+0 / +8 / +12 writers; a twin getter of ANOTHER global does not leak in."""
    tmp = tempfile.mkdtemp(prefix='qstruct_getter_')
    try:
        S = PlantSource(tmp)
        S.add(GET, ['lis r11,-31999', 'lwz r3,768(r11)', 'blr'])
        S.add(SET, ['stw r4,8(r3)', 'blr'])
        S.add(C1, ['mflr r12', f'bl 0x{GET:08x}', 'li r4,1', 'stw r4,12(r3)', 'blr'])
        S.add(C2, ['mflr r12', f'bl 0x{GET:08x}', 'li r4,5', f'bl 0x{SET:08x}', 'blr'])
        S.add(C3, ['mflr r12', f'bl 0x{GET + 0x1000:08x}', 'li r4,9', 'stw r4,16(r3)', 'blr'])
        S.add(GET + 0x1000, ['lis r11,-31999', 'lwz r3,1024(r11)', 'blr'])
        S.edge(C1, C1 + 4, GET)
        S.edge(C2, C2 + 4, GET)
        S.edge(C2, C2 + 12, SET)
        S.edge(C3, C3 + 4, GET + 0x1000)
        S.gl[G2] = {GET}
        S.gl[0x83010400] = {GET + 0x1000}
        t, rows, al, sc = qstruct.struct_of(S, f'g:0x{G2:08X}', claims=[])
        r12, r8, r16 = row(rows, 12), row(rows, 8), row(rows, 16)
        check('getter: the caller\'s store through the getter\'s result is a G2+12 write', r12 is not None and r12['writes'] == 1 and any('c1' in x.lower() or f'{C1:08X}' in x for x in r12['writers']), repr(r12))
        check('setter: the setter\'s store (+8) is a G2 write, via the caller that passed the getter\'s result', r8 is not None and r8['writes'] == 1 and any(f'{SET:08X}' in x for x in r8['writers']), repr(r8))
        twin('getter: another global\'s getter result (+16) is a row of G2', r16 is not None)
        check('getter: the scan names the getter', sc.getters == [GET], repr(sc.getters))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    planted()
    getters()
    if '--no-real' not in sys.argv:
        real()
    print(f'qstruct_selftest: {len(FAILS)} failed, {len(TWINS_MISSED)} twins not caught')
    if FAILS:
        return 1
    if TWINS_MISSED:
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
