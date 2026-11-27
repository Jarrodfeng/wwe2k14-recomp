"""Self-test of `q callargs` / `q seqids` (S111 CALLARGS): `py -I tools/q/qcallargs_selftest.py [--no-real]`   (0 game runs)

A. Planted listings (our own invented instruction sequences in a TEMP folder, written by decomp_selftest.build_listing; no game code),
   each run through the production path (qcallargs.callargs over qslice's facts), each with its twin (the same check against a WRONG
   expectation must FAIL):
     constant     a caller passes r4 = 7: the site's r4 is the constant 7
     value set    a caller passes 3 on one branch arm and 9 on the other: the set {3, 9}
     unknown      a caller passes its own incoming r5 in r4: `?` with its definition (no caller of it known)
     case join    a caller switches on [in_r3+4] - 100 (a 3-case jump table) and each case passes its own r4: the case value (= the screen
                  code, the selector being the screen word) and the compare-guarded site (`[in_r3+4] == 1500`) both state their screen;
                  a `!=` guard is never turned into a screen
B. The real re-finds on the local tree: the exit request sub_8283CFF0 r4 (36 sites, exit 4 at 0x828666C0 and exit 7 at 0x8286826C with
   their screens, no 28 / 29) and `q seqids` (sequence 31 = sub_82826638, 3 = sub_827DCCA0). A machine without generated/ or the q
   index SKIPs part B.
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import decomp_selftest  # noqa: E402
import qcallargs  # noqa: E402
import qdecomp  # noqa: E402
import qseqids  # noqa: E402
import qslice_selftest as qs  # noqa: E402
from qslice_selftest import check, twin  # noqa: E402


def listing_of(S):
    def listing(addr):
        f = S.funcs.get(addr)
        return qdecomp.read_listing(f['path'], 1, f['name'], addr)[0] if f else None
    return listing


def site_of(res, caller, rel):
    return next((s for s in res['sites'] if s['caller_addr'] == caller and s['rel'] == rel), None)


def planted():
    tmp = tempfile.mkdtemp(prefix='qcallargs_selftest_')
    try:
        S = qs.PlantSource(tmp)
        T = 0x82300000
        S.add(T, ['blr'])                                        # the callee
        # constant: r4 = 7 at the call
        K = 0x82300100
        S.add(K, ['li r4,7', f'bl 0x{T:08x}', 'blr'])
        S.edge(K, K + 4, T)
        # value set: 3 or 9 by a branch
        V = 0x82300200
        S.add(V, ['cmpwi cr6,r3,0', 'beq cr6,<L1>', 'li r4,3', 'b <L2>', ('label', 'L1'), 'li r4,9', ('label', 'L2'),
                  f'bl 0x{T:08x}', 'blr'])
        S.edge(V, V + 0x14, T)
        # unknown: r4 is the caller's own incoming r5
        U = 0x82300300
        S.add(U, ['mr r4,r5', f'bl 0x{T:08x}', 'blr'])
        S.edge(U, U + 4, T)
        # case join: switch on [r3+4] - 100 (3 cases), each case its own r4; then a compare-guarded site and a `!=` one
        W = 0x82300400
        sw = 'bctr '
        decomp_selftest.CASES[sw] = ['A', 'B', 'C']
        S.add(W, ['lwz r11,4(r3)', 'addi r10,r11,-100', 'cmplwi cr6,r10,2', 'bgt cr6,<D>', 'mr r11,r10', 'bctr ',
                  ('label', 'A'), 'li r4,41', f'bl 0x{T:08x}', 'blr',
                  ('label', 'B'), 'li r4,42', f'bl 0x{T:08x}', 'blr',
                  ('label', 'C'), 'li r4,43', f'bl 0x{T:08x}', 'blr',
                  ('label', 'D'), 'cmpwi cr6,r11,1500', 'bne cr6,<E>', 'li r4,44', f'bl 0x{T:08x}', 'blr',
                  ('label', 'E'), 'li r4,45', f'bl 0x{T:08x}', 'blr'])
        del decomp_selftest.CASES[sw]
        # call sites: A at +0x1C, B at +0x28, C at +0x34, 1500-guarded at +0x48, last at +0x54
        for rel in (0x1C, 0x28, 0x34, 0x48, 0x54):
            S.edge(W, W + rel, T)
        res = qcallargs.callargs(S, T, ['r4'], listing=listing_of(S))
        for s in res['sites']:
            print(f'  {s["caller"]}+0x{s["rel"]:X} r4={s["args"]["r4"]["kind"]}{s["args"]["r4"]["values"]} case={s["case"]} screen={s["screen"]!r} '
                  f'not={s["not_screen"]} guards={s["guards"]}')
        k = site_of(res, K, 4)
        check('constant: r4 = 7', k and k['args']['r4']['kind'] == 'const' and k['args']['r4']['values'] == [7], str(k))
        twin('constant: r4 = 8', bool(k) and k['args']['r4']['values'] == [8])
        v = site_of(res, V, 0x14)
        check('value set: {3, 9}', v and v['args']['r4']['kind'] == 'set' and v['args']['r4']['values'] == [3, 9], str(v))
        twin('value set: a single constant', bool(v) and v['args']['r4']['kind'] == 'const')
        u = site_of(res, U, 4)
        check('unknown: ? with its definition', u and u['args']['r4']['kind'] == 'unknown' and u['args']['r4']['unknown'], str(u))
        twin('unknown: a constant', bool(u) and u['args']['r4']['kind'] == 'const')
        cases = {}
        for rel in (0x1C, 0x28, 0x34):
            s = site_of(res, W, rel)
            cases[rel] = s
        check('case join: case values 100 / 101 / 102 and their screens',
              all(cases[r] and cases[r]['case'] and cases[r]['case']['values'] == [100 + i] and cases[r]['screen'] == str(100 + i)
                  for i, r in enumerate((0x1C, 0x28, 0x34))), str(cases))
        twin('case join: no case recorded', all(cases[r] and not cases[r]['case'] for r in cases))
        g = site_of(res, W, 0x48)
        check('guard: the `== 1500` compare states screen 1500', g and g['screen'] == '1500' and not g['case'], str(g))
        e = site_of(res, W, 0x54)
        check('guard: a `!=` compare is never a screen', e and e['screen'] == '' and e['not_screen'] == [1500], str(e))
        twin('guard: the != site states screen 1500', bool(e) and e['screen'] == '1500')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def real():
    import time
    try:
        import q as qmod
        import qslice
        con = qmod.connect(qmod.DB)
    except Exception as ex:  # noqa: BLE001 - no q index on this machine: part B is skipped, said so
        print(f'SKIP B (no q index: {ex})')
        return
    if not os.path.isdir(qdecomp.GEN):
        print('SKIP B (no generated/ tree)')
        return
    t0 = time.time()
    src = qslice.QSource(con)
    res = qcallargs.callargs(src, 0x8283CFF0, ['r4'], con=con)
    print(f'== callargs sub_8283CFF0 r4: {len(res["sites"])} sites, {time.time() - t0:.1f} s')
    n = len(res['sites'])
    vals = {v for s in res['sites'] for v in s['args']['r4']['values']}
    check('real: 36 call sites of the exit request, every r4 constant', n == 36 and all(s['args']['r4']['kind'] == 'const' for s in res['sites']), f'{n}')
    twin('real: an exit 28 or 29 sender', 28 in vals or 29 in vals)
    s4 = next((s for s in res['sites'] if s['site'] == 0x828666C0), None)
    check('real: exit 4 at 0x828666C0 (sub_82866680), screen not 1500 / 1501',
          s4 and s4['args']['r4']['values'] == [4] and s4['caller'] == 'sub_82866680' and s4['not_screen'] == [1500, 1501] and not s4['screen'], str(s4))
    twin('real: exit 4 states screen 1531', bool(s4) and s4['screen'] == '1531')
    s7 = next((s for s in res['sites'] if s['site'] == 0x8286826C), None)
    check('real: exit 7 at 0x8286826C (leaf map sub_82867C48), case / screen 1519',
          s7 and s7['args']['r4']['values'] == [7] and s7['caller'] == 'sub_82867C48' and s7['screen'] == '1519' and s7['case'], str(s7))
    twin('real: exit 7 at screen 1518', bool(s7) and s7['screen'] == '1518')
    rows = qseqids.seqids(src, con=con)
    by = {r['ids'][0]: r['ctor'] for r in rows if r['ids']}
    print(f'== seqids: {len(rows)} rows')
    check('real: seqids 27 rows, 31 = sub_82826638 (CRTWBeginSequence), 3 = sub_827DCCA0 (title)',
          len(rows) == 27 and by.get(31) == 'sub_82826638' and by.get(3) == 'sub_827DCCA0', f'{len(rows)} {by.get(31)} {by.get(3)}')
    twin('real: seqid 31 = sub_827DCCA0', by.get(31) == 'sub_827DCCA0')


def main():
    planted()
    if '--no-real' not in sys.argv:
        real()
    nf, nt = len(qs.FAILS), len(qs.TWINS_MISSED)
    print(f'qcallargs_selftest: {nf} failed, {nt} twins not caught')
    return 1 if nf else 2 if nt else 0


if __name__ == '__main__':
    sys.exit(main())
