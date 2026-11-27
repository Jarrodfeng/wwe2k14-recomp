"""Self-test of `q consumers` (S111 PTRORIGIN): `py -I tools/q/qconsumers_selftest.py [--no-real]`   (0 game runs)

A. Planted listings (our own invented instruction sequences in a TEMP folder, written by decomp_selftest.build_listing; no game code), each
   run through the production path (qconsumers.run_ret over qslice's facts), each with its twin (a wrong expectation must FAIL):
     direct      a caller loads +8 off the value sub_X returned
     slot        the caller stores the pointer to a stack slot, calls another function, reloads it and passes it to a callee that reads +60 and
                 writes +72: both listed (depth 1), with the path through the slot; at depth 0 the callee's accesses are NOT listed (cut: depth)
     join        a phi of the pointer and 0 read at +12 (marked phi)
     twin source a callee reached with a pointer that comes from elsewhere ([r31+4]) and a read at +100 off that pointer: not listed
     offset      `addi r4,r3,16` passed to the callee: its +60 read is the origin's +76
B. The re-find on the local tree (the record lookup sub_83424808, depth 2): runs to the end, prints its coverage. A machine without
   generated/ or the q index SKIPs part B.
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qconsumers  # noqa: E402
import qdecomp  # noqa: E402
import qslice_selftest as qs  # noqa: E402
from qslice_selftest import check, twin  # noqa: E402


def sites(r, off):
    return r['offsets'].get(str(off), [])


def planted():
    tmp = tempfile.mkdtemp(prefix='qconsumers_selftest_')
    try:
        S = qs.PlantSource(tmp)
        X, Y, R = 0x82400000, 0x82400010, 0x82400020
        S.add(X, ['blr'])
        S.add(Y, ['blr'])
        S.add(R, ['lwz r4,60(r3)', 'stw r5,72(r3)', 'blr'])
        C = 0x82400100
        S.add(C, ['bl 0x%08x' % X, 'lwz r5,8(r3)', 'stw r3,80(r1)', 'bl 0x%08x' % Y, 'lwz r3,80(r1)', 'bl 0x%08x' % R, 'blr'])
        S.edge(C, C, X)
        # the twin source: a pointer from elsewhere reaches R, and +100 is read off it; X is called but its result is dropped
        E = 0x82400200
        S.add(E, ['bl 0x%08x' % X, 'lwz r3,4(r31)', 'lwz r6,100(r3)', 'bl 0x%08x' % R, 'blr'])
        S.edge(E, E, X)
        # join: r30 = the result or 0
        P = 0x82400300
        S.add(P, ['bl 0x%08x' % X, 'mr r30,r3', 'cmpwi cr6,r4,0', 'beq cr6,<L1>', 'li r30,0', ('label', 'L1'), 'lwz r6,12(r30)', 'blr'])
        S.edge(P, P, X)
        # offset: &result[16] is passed on
        O = 0x82400400
        S.add(O, ['bl 0x%08x' % X, 'addi r4,r3,16', 'mr r3,r4', 'bl 0x%08x' % R, 'blr'])
        S.edge(O, O, X)
        # a wrapper returns the pointer; its caller reads +20
        W = 0x82400500
        S.add(W, ['bl 0x%08x' % X, 'blr'])
        S.edge(W, W, X)
        V = 0x82400600
        S.add(V, ['bl 0x%08x' % W, 'lwz r6,20(r3)', 'blr'])
        S.edge(V, V, W)
        r = qconsumers.result_of(*qconsumers.run_ret(S, X, None, 1), None, 'ret')
        for off, hs in r['offsets'].items():
            for h in hs:
                print(f'  +{off} {h["rw"]} {h["func"]}+0x{h["rel"]:X}   {h["paths"][0]}')
        print('  ' + qconsumers.coverage_line(r))
        d = sites(r, 8)
        check('direct: the caller reads +8', len(d) == 1 and d[0]['rw'] == 'R' and d[0]['func_addr'] == C, str(d))
        twin('direct: read at +9', bool(sites(r, 9)))
        s60 = [h for h in sites(r, 60) if h['func_addr'] == R]
        check('slot: the callee reads +60 through the stack slot', len(s60) == 1 and s60[0]['rw'] == 'R' and 'slot' in s60[0]['paths'][0], str(s60))
        s72 = [h for h in sites(r, 72) if h['func_addr'] == R]
        check('slot: the callee writes +72', len(s72) == 1 and s72[0]['rw'] == 'W', str(s72))
        twin('slot: the callee reads +64', bool(sites(r, 64)))
        check('join: +12 read in the joining caller, marked phi',
              any(h['func_addr'] == P and 'phi' in h['paths'][0] for h in sites(r, 12)), str(sites(r, 12)))
        twin('join: +12 read in a function that never saw the pointer', any(h['func_addr'] == E for h in sites(r, 12)))
        check('twin source: +100 off the unrelated pointer is not listed', not sites(r, 100), str(sites(r, 100)))
        check('twin source: E reads nothing through the origin', not any(h['func_addr'] == E for hs in r['offsets'].values() for h in hs))
        s76 = [h for h in sites(r, 76) if h['func_addr'] == R]
        check('offset: the callee\'s +60 read is the origin\'s +76 (addi r4,r3,16)', len(s76) == 1 and s76[0]['rw'] == 'R' and '+16' in s76[0]['paths'][0], str(s76))
        twin('offset: it is at +60 from this path only', len(s60) == 2)
        check('coverage: 5 origin sites, paths and functions counted', r['origin_sites'] == 5 and r['coverage']['functions_scanned'] == 8, str(r['coverage']))
        w20 = sites(r, 20)
        check('returned onward: the wrapper\'s caller reads +20', len(w20) == 1 and w20[0]['func_addr'] == V and 'returned by' in w20[0]['paths'][0], str(w20))
        twin('returned onward: nobody reads +20', not w20)
        r00 = qconsumers.result_of(*qconsumers.run_ret(S, X, None, 0), None, 'ret')
        check('returned onward at depth 0: not followed, the cut is counted', not sites(r00, 20) and r00['coverage']['cut'].get('returned onward (depth)', 0) >= 1,
              str(r00['coverage']))
        r0 = qconsumers.result_of(*qconsumers.run_ret(S, X, None, 0), None, 'ret')
        check('depth 0: the callee is not entered, the cut is counted', not [h for hs in r0['offsets'].values() for h in hs if h['func_addr'] == R]
              and r0['coverage']['cut'].get('depth', 0) >= 2, str(r0['coverage']))
        twin('depth 0: the callee is entered', any(h['func_addr'] == R for hs in r0['offsets'].values() for h in hs))
        ra = qconsumers.result_of(*qconsumers.run_arg(S, R, 'r3', 1), None, 'arg')
        check('--arg r3 of the callee: its own +60 read', len(sites(ra, 60)) == 1, str(ra['offsets']))
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
    sc, extra = qconsumers.run_ret(src, 0x83424808, con, 1)
    r = qconsumers.result_of(sc, extra, None, 'ret')
    print(f'== consumers sub_83424808 --ret: {r["coverage"]["hits"]} accesses, {time.time() - t0:.1f} s')
    print('  ' + qconsumers.coverage_line(r))
    check('real: the record lookup has call sites and some consumer path', r['origin_sites'] >= 5 and r['coverage']['paths_followed'] >= 5, str(r['coverage']))
    twin('real: no consumer path at all', r['coverage']['paths_followed'] == 0)


def main():
    planted()
    if '--no-real' not in sys.argv:
        real()
    nf, nt = len(qs.FAILS), len(qs.TWINS_MISSED)
    print(f'qconsumers_selftest: {nf} failed, {nt} twins not caught')
    return 1 if nf else 2 if nt else 0


if __name__ == '__main__':
    sys.exit(main())
