"""Self-test of `q tables` / `q vtable --dump` (S111 JTABLE): `py -I tools/q/qtables_selftest.py [--no-real]`   (0 game runs)

A. Planted listings (our own invented instruction sequences in a TEMP folder, written by decomp_selftest.build_listing; no game code), each run
   through the production path (qdecomp.read_listing -> qtables.analyse), each with its twin (the same check against a WRONG expectation must FAIL):
     jump      a `bctr` switch on [r3+4] - 100: value base 100, the table address, one row per case with what it does first (a call with r4, a
               constant store, a return of r3), the bounds-check default
     chain     three `cmpwi rX,K` + `beq`: K -> target; twin: two compares are no chain
     bdzf      `addi -1; mtctr; cmpwi 0; bdzf; bdzf; bne`: value = k + 1, the fall-through = the base, `other` = the bne target
     table     an indexed word table (lwzx from lis/addi) feeding `mtctr` + `bctrl`: the entries read from the image, length from the guard;
               twin: the same load with no call / compare use is no structure
     pretable  a byte table feeding a switch selector: table index -> loaded value -> case -> target
     vtable    `q vtable --dump` rows: slot -> function -> first stores / return (the save manager's `stw [r3+68] = op`)
B. The real re-finds on the local tree: the pause answer step sub_8235DC30 (22 states), the exit dispatcher sub_82833440 (30 ids, 25 distinct targets,
   28 / 29 -> the epilogue), the save task sub_8291F0E8 (8 bdzf kinds + other), the save manager vtable 0x82046FA0 (+60 op 8, +128 op 28, +268 none).
   A machine without generated/ or the q index SKIPs part B.
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import os
import shutil
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import decomp_selftest  # noqa: E402
import qdecomp  # noqa: E402
import qtables  # noqa: E402
from qslice_selftest import check, twin  # noqa: E402
import qslice_selftest as qs  # noqa: E402

BASE = 0x82000000


class FakeImg:
    base = BASE

    def __init__(self):
        self.data = bytearray(0x1000)

    def put(self, a, width, v):
        self.data[a - BASE:a - BASE + width] = v.to_bytes(width, 'big')

    def word(self, a):
        o = a - BASE
        return None if a & 3 or o < 0 or o + 4 > len(self.data) else int.from_bytes(self.data[o:o + 4], 'big')


def listing(tmp, base, items, name=None):
    text, labels = decomp_selftest.build_listing(base, items, name)
    path = os.path.join(tmp, f'f{base:08X}.cpp')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(text)
    insns, labs, _a, _b = qdecomp.read_listing(path, 1, name or f'sub_{base:08X}', base)
    return insns, labs, labels, path


def run(insns, labs, img, **kw):
    return qtables.analyse(insns, labs, img, lambda a: None, None, **kw)


def planted():
    tmp = tempfile.mkdtemp(prefix='qtables_selftest_')
    try:
        img = FakeImg()
        # ---- jump: switch on [r3+4] - 100
        J = 0x82300000
        decomp_selftest.CASES['bctr '] = ['A', 'B', 'C']
        items = ['lwz r11,4(r3)', 'addi r10,r11,-100', 'cmplwi cr6,r10,2', 'bgt cr6,<D>', 'lis r12,-32256', 'addi r12,r12,256',
                 'slwi r0,r10,2', 'lwzx r0,r12,r0', 'mtctr r0', 'bctr ',
                 ('label', 'A'), 'li r4,41', 'bl 0x82300800', 'blr',
                 ('label', 'B'), 'li r11,7', 'stw r11,48(r31)', 'blr',
                 ('label', 'C'), 'li r3,9', 'blr',
                 ('label', 'D'), 'li r3,0', 'blr']
        insns, labs, L, _p = listing(tmp, J, items)
        for i, k in enumerate('ABC'):
            img.put(0x82000100 + 4 * i, 4, L[k])
        r = run(insns, labs, img)
        s = r[0] if r else {}
        rows = {x['label']: x for x in s.get('rows', [])}
        check('jump: one structure, kind jump, 3 entries, base 100, selector [r3+4]',
              len(r) == 1 and s['kind'] == 'jump' and s['entries'] == 3 and s['base'] == 100 and s['selector'] == '[r3+4]', str(s)[:200])
        twin('jump: base 0', bool(s) and s['base'] == 0)
        check('jump: table address 0x82000100 (u32 x3)', s.get('table') == '0x82000100 (u32 x3)', str(s.get('table')))
        check('jump: value 100 -> case A: calls sub_82300800(r4=41)', rows.get('100', {}).get('effect', '').startswith('calls sub_82300800(r4=41)'),
              str(rows.get('100')))
        twin('jump: value 100 passes r4=42', rows.get('100', {}).get('effect', '').startswith('calls sub_82300800(r4=42)'))
        check('jump: value 101 stores 7 into [r31+48]', rows.get('101', {}).get('effect', '').startswith('stw [r31+48] = 7'), str(rows.get('101')))
        check('jump: value 102 returns r3=9', rows.get('102', {}).get('effect') == 'returns r3=9', str(rows.get('102')))
        check('jump: the default (bgt) goes to D and returns r3=0', s['default'] and s['default']['target'] == L['D']
              and s['default']['effect'] == 'returns r3=0', str(s.get('default')))
        check('jump: targets are the case labels', [rows[k]['target'] for k in ('100', '101', '102')] == [L['A'], L['B'], L['C']], str(rows))

        # ---- chain
        C = 0x82300100
        items = ['cmpwi cr6,r11,5', 'beq cr6,<A>', 'cmpwi cr6,r11,7', 'beq cr6,<B>', 'cmpwi cr6,r11,9', 'beq cr6,<C>', 'b <D>',
                 ('label', 'A'), 'li r3,1', 'blr', ('label', 'B'), 'li r3,2', 'blr', ('label', 'C'), 'li r3,3', 'blr', ('label', 'D'), 'li r3,0', 'blr']
        insns, labs, L, _p = listing(tmp, C, items)
        r = run(insns, labs, img)
        s = r[0] if r else {}
        check('chain: 3 compares -> a chain 5 / 7 / 9 -> returns r3=1 / 2 / 3, otherwise D',
              len(r) == 1 and s['kind'] == 'chain' and [(x['label'], x['effect']) for x in s['rows']] ==
              [('5', 'returns r3=1'), ('7', 'returns r3=2'), ('9', 'returns r3=3')] and s['default']['target'] == L['D'], str(s)[:240])
        twin('chain: 9 -> returns r3=2', bool(s) and s['rows'][2]['effect'] == 'returns r3=2')
        items2 = ['cmpwi cr6,r11,5', 'beq cr6,<A>', 'cmpwi cr6,r11,7', 'beq cr6,<B>', 'b <D>',
                  ('label', 'A'), 'li r3,1', 'blr', ('label', 'B'), 'li r3,2', 'blr', ('label', 'D'), 'li r3,0', 'blr']
        insns, labs, L, _p = listing(tmp, 0x82300180, items2)
        check('chain twin: two compares are no chain', run(insns, labs, img) == [], str(run(insns, labs, img))[:120])

        # ---- bdzf
        B = 0x82300200
        items = ['lwz r11,24(r3)', 'addi r11,r11,-1', 'cmplwi cr6,r11,2', 'bgt cr6,<D>', 'mtctr r11', 'cmpwi cr6,r11,0',
                 'bdzf 4*cr6+eq,<A>', 'bdzf 4*cr6+eq,<B>', 'bne cr6,<D>', 'li r3,10', 'blr',
                 ('label', 'A'), 'li r3,11', 'blr', ('label', 'B'), 'li r3,12', 'blr', ('label', 'D'), 'li r3,99', 'blr']
        insns, labs, L, _p = listing(tmp, B, items)
        r = run(insns, labs, img)
        s = r[0] if r else {}
        got = [(x['label'], x['effect']) for x in s.get('rows', [])]
        check('bdzf: states 1 (fall-through) / 2 / 3 -> returns r3=10 / 11 / 12, other -> 99, selector [r3+24]',
              len(r) == 1 and s['kind'] == 'bdzf' and got == [('1', 'returns r3=10'), ('2', 'returns r3=11'), ('3', 'returns r3=12')]
              and s['default']['effect'] == 'returns r3=99' and s['selector'] == '[r3+24]' and not s.get('note'), str(s)[:300])
        twin('bdzf: state 0 is the fall-through', bool(got) and got[0][0] == '0')

        # ---- indexed word table feeding a call through the counter
        T = 0x82300300
        items = ['cmplwi cr6,r4,2', 'bgt cr6,<D>', 'lis r12,-32256', 'addi r12,r12,512', 'slwi r0,r4,2', 'lwzx r11,r12,r0', 'mtctr r11', 'bctrl', 'blr',
                 ('label', 'D'), 'blr', ('label', 'L1'), 'li r3,5', 'blr', ('label', 'L2'), 'li r3,6', 'blr']
        insns, labs, L, _p = listing(tmp, T, items)
        for i, k in enumerate(('L1', 'L2', 'L1')):
            img.put(0x82000200 + 4 * i, 4, L[k])
        r = run(insns, labs, img)
        s = r[0] if r else {}
        check('table: 3 entries (guard cmplwi 2) of 0x82000200 feeding a call: returns r3=5 / 6 / 5',
              len(r) == 1 and s['kind'] == 'table' and s['use'] == 'call' and s['table'] == '0x82000200 (u32 x3)'
              and [x['effect'] for x in s['rows']] == ['returns r3=5', 'returns r3=6', 'returns r3=5'], str(s)[:300])
        twin('table: 4 entries', bool(s) and s['entries'] == 4)
        items2 = ['cmplwi cr6,r4,2', 'bgt cr6,<D>', 'lis r12,-32256', 'addi r12,r12,512', 'slwi r0,r4,2', 'lwzx r11,r12,r0', 'mr r3,r11', 'blr', ('label', 'D'), 'blr']
        insns, labs, L, _p = listing(tmp, 0x82300380, items2)
        check('table twin: a load with no call / compare use is no structure', run(insns, labs, img) == [], str(run(insns, labs, img))[:120])

        # ---- a byte table picking the case of a switch
        P = 0x82300400
        decomp_selftest.CASES['bctr  '] = ['A', 'B', 'C']
        items = ['cmplwi cr6,r4,3', 'bgt cr6,<D>', 'lis r12,-32256', 'addi r12,r12,768', 'lbzx r11,r12,r4', 'cmplwi cr6,r11,2', 'bgt cr6,<D>',
                 'slwi r0,r11,2', 'bctr  ',
                 ('label', 'A'), 'li r3,1', 'blr', ('label', 'B'), 'li r3,2', 'blr', ('label', 'C'), 'li r3,3', 'blr', ('label', 'D'), 'li r3,0', 'blr']
        insns, labs, L, _p = listing(tmp, P, items)
        for i, v in enumerate((1, 0, 2, 1)):
            img.put(0x82000300 + i, 1, v)
        r = run(insns, labs, img)
        s = r[0] if r else {}
        pre = s.get('pretable', {})
        check('pretable: byte table [1,0,2,1] -> cases B, A, C, B (returns r3=2 / 1 / 3 / 2)',
              len(r) == 1 and [x['target'] for x in pre.get('rows', [])] == [L['B'], L['A'], L['C'], L['B']]
              and [x['effect'] for x in pre.get('rows', [])] == ['returns r3=2', 'returns r3=1', 'returns r3=3', 'returns r3=2']
              and pre.get('table') == '0x82000300 (u8 x4)', str(s)[:400])
        twin('pretable: table index 0 -> case A', bool(pre) and pre['rows'][0]['target'] == L['A'])

        # ---- vtable --dump
        gen = os.path.join(tmp, 'gen')
        os.makedirs(gen)
        con = sqlite3.connect(':memory:')
        con.row_factory = sqlite3.Row
        con.execute('CREATE TABLE funcs (addr INTEGER, name TEXT, size INTEGER, insns INTEGER, file TEXT, line INTEGER)')
        slots = [(0x82300500, ['li r4,8', 'stw r4,68(r3)', 'blr']), (0x82300540, ['li r3,1', 'bl 0x82300800', 'blr'])]
        for i, (a, its) in enumerate(slots):
            text, _l = decomp_selftest.build_listing(a, its)
            with open(os.path.join(gen, f'g{i}.cpp'), 'w', encoding='utf-8') as f:
                f.write(text)
            con.execute('INSERT INTO funcs VALUES (?,?,0,0,?,1)', (a, f'sub_{a:08X}', f'g{i}.cpp'))
        for i, (a, _its) in enumerate(slots):
            img.put(0x82000400 + 4 * i, 4, a)
        saved = qdecomp.GEN
        qdecomp.GEN = gen
        try:
            d = qtables.dump_vtable(con, img, 0x82000400, 8)
        finally:
            qdecomp.GEN = saved
        firsts = [s['first'] for s in d['slots']]
        check('vtable --dump: slot 0 stores 8 into [r3+68] then returns; slot 1 calls sub_82300800(r3=1)',
              d['table_slots'] == 2 and firsts == ['stw [r3+68] = 8; returns', 'calls sub_82300800(r3=1); returns'], str(firsts))
        twin('vtable --dump: slot 0 stores 9', bool(firsts) and firsts[0].startswith('stw [r3+68] = 9'))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def real():
    try:
        import q as qmod
        con = qmod.connect(qmod.DB)
    except Exception as ex:  # noqa: BLE001 - no q index here: part B is skipped, said so
        print(f'SKIP B (no q index: {ex})')
        return
    if not os.path.isdir(qdecomp.GEN):
        print('SKIP B (no generated/ tree)')
        return
    import qvtable

    def of(name):
        addr = qmod.resolve(con, name)[0]
        f = con.execute('SELECT * FROM funcs WHERE addr = ?', (addr,)).fetchone()
        insns, labs, _a, _b = qdecomp.read_listing(os.path.join(qdecomp.GEN, f['file']), f['line'], f['name'], addr)
        return qtables.analyse(insns, labs, qtables.image(), lambda x: qdecomp._lookup(con, x), None)
    r = of('sub_8235DC30')
    s = r[0] if r else {}
    check('real: sub_8235DC30 = one jump table, 22 states, 22 distinct targets (table 0x8235DC98)',
          len(r) == 1 and s['kind'] == 'jump' and s['entries'] == 22 and s['distinct'] == 22 and s['table'] == '0x8235DC98 (u32 x22)', str(s)[:160])
    twin('real: sub_8235DC30 has 18 distinct targets', bool(s) and s['distinct'] == 18)
    r = of('sub_82833440')
    s = r[0] if r else {}
    rows = {x['label']: x for x in s.get('rows', [])}
    check('real: sub_82833440 = 30 exit ids, 25 distinct targets, base 1, table 0x828334A8, 28 / 29 -> 0x828339B4',
          len(r) == 1 and s['entries'] == 30 and s['distinct'] == 25 and s['base'] == 1 and s['table'].startswith('0x828334A8')
          and rows['28']['target'] == rows['29']['target'] == 0x828339B4, str(s)[:160])
    twin('real: sub_82833440 has 26 distinct targets', bool(s) and s['distinct'] == 26)
    r = of('sub_8291F0E8')
    s = r[0] if r else {}
    check('real: sub_8291F0E8 = a bdzf chain, 8 kinds (1..8) + other',
          len(r) == 1 and s['kind'] == 'bdzf' and s['entries'] == 8 and s['base'] == 1 and s['default'], str(s)[:160])
    twin('real: sub_8291F0E8 has 12 kinds', bool(s) and s['entries'] == 12)
    d = qtables.dump_vtable(con, qvtable.Img(os.path.join(os.path.dirname(qmod.DB), 'image.bin')), 0x82046FA0, 80)
    by = {x['offset']: x['first'] for x in d['slots']}
    check('real: save manager vtable +60 op 8, +128 op 28, +148 op 34, +172 op 41, +188 op 46, +272 op 63, +268 none',
          all(f'[r3+68] = {op}' in by[off] for off, op in ((60, 8), (128, 28), (148, 34), (172, 41), (188, 46), (272, 63)))
          and '[r3+68]' not in by[268], str({o: by[o][:60] for o in (60, 128, 148, 172, 188, 268, 272)}))
    twin('real: save manager +60 queues op 9', '[r3+68] = 9' in by[60])


def main():
    planted()
    if '--no-real' not in sys.argv:
        real()
    nf, nt = len(qs.FAILS), len(qs.TWINS_MISSED)
    print(f'qtables_selftest: {nf} failed, {nt} twins not caught')
    return 1 if nf else 2 if nt else 0


if __name__ == '__main__':
    sys.exit(main())
