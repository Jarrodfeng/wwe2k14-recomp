"""Self-test of `q slice` (S111 SLICER): `py -I tools/q/qslice_selftest.py [--no-real]`   (0 game runs)

A. Planted listings (our own invented instruction sequences in a TEMP folder, written by decomp_selftest.build_listing; no game
   code), each walked by the production path (qslice.build_facts over qdecomp's interpreter, then Slicer.value from the start
   operand), with its twin: the same check against a WRONG expectation must FAIL (a checker that cannot fail is not a checker):
     phi          a value set on both arms of a branch: one line per path with that path's condition
     call return  a callee's return value with the callee's argument bound to this call's (100 + 8)
     argument     an entry argument climbs into every caller (3 and 9)
     field        a load of a global word: its writer store found (the field index planted) and its value sliced (77);
                  a store to the next word is not a writer
     class array  a class field read through a loop induction (base + 48*k) and written by an unrolled loop (stride 480) of
                  another slot of the same vtable: the writer found; a store of the same class 4 bytes off is not one
     loop         a loop-carried counter is base + step*?loop, never a constant
B. The three real re-finds of lodged [C] chains (MATSIZE, CUBELEVELS, ALPHAWREST) on the local tree, each with its wall time and a
   planted wrong expectation that must be absent. A machine without generated/ or the q index SKIPs part B.
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import os
import shutil
import struct
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qdecomp  # noqa: E402
import qslice  # noqa: E402
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
    """the walk's outside world from planted listings: functions, call edges, field-index K writers, vtable slots, image words"""

    def __init__(self, tmp):
        self.tmp, self.n = tmp, 0
        self.funcs, self.edges, self.kw, self.words, self.slots, self._f = {}, [], {}, {}, {}, {}

    def add(self, base, items, name=None):
        text, _labels = build_listing(base, items, name)
        self.n += 1
        path = os.path.join(self.tmp, f'plant.{self.n}.cpp')
        with open(path, 'w', encoding='utf-8') as fh:
            fh.write(text)
        nm = name or f'sub_{base:08X}'
        self.funcs[base] = {'addr': base, 'name': nm, 'path': path}
        for it in items:
            if isinstance(it, str) and it.startswith('bl '):
                pass
        return base

    def edge(self, src, site, dst, kind='call'):
        self.edges.append((src, site, dst, kind))

    def func(self, addr):
        return self.funcs.get(addr)

    def facts(self, addr):
        if addr not in self._f:
            f = self.funcs.get(addr)
            if f is None:
                self._f[addr] = None
            else:
                insns, labels, _a, _b = qdecomp.read_listing(f['path'], 1, f['name'], addr)
                names = lambda a: self.funcs[a]['name'] if a in self.funcs else None  # noqa: E731
                self._f[addr] = qslice._decode(qslice.json.loads(qslice.json.dumps(
                    qslice.build_facts(f['name'], addr, insns, labels, names, self))))     # self.word = the planted image
        return self._f[addr]

    def callers(self, addr):
        return [(s, site, k) for s, site, d, k in self.edges if d == addr and s != addr]

    def k_writers(self, a, g):
        return set(self.kw.get(a, ()))

    def word(self, a):
        return self.words.get(a)

    def is_vt(self, a):
        return a in self.slots

    def vslots(self, addr):
        return [(vt, i) for vt, fs in self.slots.items() for i, f in enumerate(fs) if f == addr]

    def arg_types(self, addr, reg):
        return {vt for vt, _i in self.vslots(addr)} if reg == 'r3' else set()

    def type_funcs(self, vts):
        return {f for vt in vts for f in self.slots.get(vt, ())}


def run(src, addr, rel, opnd, depth=6):
    root, expr, head, sl = qslice.slice_site(src, addr, rel, opnd, depth, 200)
    return qslice.tree_lines(root, 1), expr


def has(lines, s):
    return any(s in l for l in lines)


def planted():
    tmp = tempfile.mkdtemp(prefix='qslice_selftest_')
    try:
        S = PlantSource(tmp)
        # --- phi: r4 = 7 when in_r3 == 0, else 5; stored at +0x14
        A = 0x82100000
        S.add(A, ['cmpwi cr6,r3,0', 'beq cr6,<L1>', 'li r4,5', 'b <L2>', ('label', 'L1'), 'li r4,7', ('label', 'L2'),
                  'stw r4,0(r5)', 'blr'])
        lines, expr = run(S, A, 0x14, '[mem]')
        print('\n'.join(lines))
        ok = has(lines, 'differs by path') and has(lines, 'in_r3 == 0') and has(lines, '= 7') and has(lines, '= 5')
        check('phi: both paths with their conditions', ok, '\n'.join(lines))
        check('phi --expr', expr.startswith('phi(') and '7' in expr and '5' in expr, expr)
        twin('phi: a single constant 7', not has(lines, 'differs by path') and has(lines, '= 7'))
        # --- call return: C returns in_r3 + 8; B passes 100 and stores the result
        B, C = 0x82100100, 0x82100200
        S.add(C, ['addi r3,r3,8', 'blr'])
        S.add(B, ['li r3,100', f'bl 0x{C:08x}', 'stw r3,0(r31)', 'blr'])
        lines, expr = run(S, B, 0x8, '[mem]')
        print('\n'.join(lines))
        check('call return: the callee return bound to the call', expr == '100+8' and has(lines, f'return r3 of sub_{C:08X}'),
              f'{expr} / ' + '\n'.join(lines))
        twin('call return: the callee\'s own in_r3+8', expr == 'in_r3+8')
        # --- argument climb: D stores its r4; E1 passes 3, E2 passes 9
        D, E1, E2 = 0x82100300, 0x82100400, 0x82100500
        S.add(D, ['stw r4,8(r3)', 'blr'])
        S.add(E1, ['li r4,3', f'bl 0x{D:08x}', 'blr'])
        S.add(E2, ['li r4,9', f'bl 0x{D:08x}', 'blr'])
        S.edge(E1, E1 + 4, D)
        S.edge(E2, E2 + 4, D)
        lines, expr = run(S, D, 0x0, 'r4')
        print('\n'.join(lines))
        check('argument: every caller listed', has(lines, '2 caller sites') and has(lines, f'caller sub_{E1:08X}+0x4 r4 = 3') and
              has(lines, f'caller sub_{E2:08X}+0x4 r4 = 9'), '\n'.join(lines))
        twin('argument: only one caller (3)', not has(lines, '= 9') and has(lines, '= 3'))
        # --- field: F loads the global word 0x83000100; W1 stores 77 there, W2 stores 66 at 0x83000104
        F, W1, W2 = 0x82100600, 0x82100700, 0x82100800
        S.add(F, ['lis r11,-31999', 'lwz r3,256(r11)', 'blr'])
        S.add(W1, ['lis r11,-31999', 'li r4,77', 'stw r4,256(r11)', 'blr'])
        S.add(W2, ['lis r11,-31999', 'li r4,66', 'stw r4,260(r11)', 'blr'])
        S.kw[0x83010100] = {W1}
        S.kw[0x83010104] = {W2}
        S.words[0x83010100] = 5
        lines, expr = run(S, F, 0x4, '[mem]')
        print('\n'.join(lines))
        check('field: the writer and its value', has(lines, f'writer sub_{W1:08X}+0x8 [C]') and has(lines, '= 77') and
              has(lines, 'image value 5'), '\n'.join(lines))
        twin('field: the next word\'s store is a writer', has(lines, f'writer sub_{W2:08X}'))
        # --- class array: R (slot 0 of vtable VT) reads [in_r3 + 2464 + 48k] in a loop; U (slot 1) writes in_r3+2468+480k-4 (unrolled)
        VT = 0x82200000
        R, U = 0x82100900, 0x82100A00
        S.add(R, ['addi r27,r3,2464', ('label', 'L'), 'lfs f13,0(r27)', 'stfs f13,0(r4)', 'addi r27,r27,48', 'cmpw cr6,r27,r5',
                  'blt cr6,<L>', 'blr'])
        S.add(U, ['addi r11,r3,2468', ('label', 'L'), 'stfs f1,-4(r11)', 'stfs f2,0(r11)', 'addi r11,r11,480', 'cmpw cr6,r11,r5',
                  'blt cr6,<L>', 'blr'])
        S.slots[VT] = [R, U]
        lines, expr = run(S, R, 0x8, '[mem]')
        print('\n'.join(lines))
        check('class array: the unrolled writer of the same class field', has(lines, 'class 0x82200000 +2464 + 48*k') and
              has(lines, f'writer sub_{U:08X}+0x4 [C]') and has(lines, 'in_f1'), '\n'.join(lines))
        twin('class array: the store 4 bytes off is a writer', has(lines, f'writer sub_{U:08X}+0x8'))
        check('class array --expr', expr == '[in_r3 + 48*?loop + 2464]', expr)
        # --- loop leaf: r4 counts up by 4 from 0 until 64, then is stored
        G = 0x82100B00
        S.add(G, ['li r4,0', ('label', 'L'), 'addi r4,r4,4', 'cmpwi cr6,r4,64', 'blt cr6,<L>', 'stw r4,0(r3)', 'blr'])
        lines, expr = run(S, G, 0x10, '[mem]')
        print('\n'.join(lines))
        check('loop: base + step*?loop', '?loop' in expr and has(lines, 'loop induction'), f'{expr} / ' + '\n'.join(lines))
        twin('loop: a constant 64', expr == '64')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


REAL = [
    # (name, site, operand, must have, must NOT have (the planted wrong expectation))
    ('MATSIZE splat world scale', 0x826979F8, 0xE0, '[mem]',
     ['class 0x82023298 +2464 + 48*k', 'writer sub_826983B8+0x88 [C]', 'class 0x82023298 +104 + 800*k',
      'writer sub_82697C28+0x16C [C]', '0.1 (1/10)[0x82003E28]', 'writer sub_82697EB8+0x234 [C]', '0.0023333333[0x820232C8]',
      'writer sub_82697C28+0x174 [C]', 'caller sub_82698120+0x224 f1', 'a table at 0x83622ACC, stride 4; image f32: 0.1 (1/10), 0.2 (1/5), 0.06',
      'a table at 0x83622AE4'],
     'a table at 0x83622AD0'),
    ('CUBELEVELS cube level count', 0x82CCD778, 0x68, 'r9',
     ['sub_82CCD778+0x68 r9 = in_r7', 'argument r7 of sub_82CCD778: 1 caller site', 'caller sub_82BD4360+0x130 r7 = 1[0x83629CA8]',
      'the word at 0x83629CA8', 'writer sub_82BD42E0+0x4 [C]'],
     'caller sub_82BD4360+0x130 r8'),
    ('ALPHAWREST category reset alpha ref', 0x82C22E88, 0x48, '[mem]',
     ['sub_82C22E88+0x48 [mem] = 1   -- constant'],
     'sub_82C22E88+0x48 [mem] = 5'),
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
    for name, addr, rel, opnd, want, wrong in REAL:
        t0 = time.time()
        src = qslice.QSource(con)
        lines, expr = run(src, addr, rel, opnd)
        dt = time.time() - t0
        print(f'== {name}: q slice sub_{addr:08X}+{rel:X} {opnd}   ({dt:.1f} s, {len(src._facts)} functions read, {src.read} analysed now)')
        print('value: ' + expr)
        print('\n'.join(lines))
        miss = [w for w in want if not has(lines, w)]
        check(f'real: {name}', not miss, f'missing: {miss}')
        twin(f'real: {name}: {wrong}', has(lines, wrong))


def main():
    planted()
    if '--no-real' not in sys.argv:
        real()
    print(f'qslice_selftest: {len(FAILS)} failed, {len(TWINS_MISSED)} twins not caught')
    if FAILS:
        return 1
    if TWINS_MISSED:
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
