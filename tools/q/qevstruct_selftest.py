"""Self-test of `q slice --struct` and `q msgbox` (S111 QEVENT): `py -I tools/q/qevstruct_selftest.py [--no-real]`   (0 game runs)

A. Planted listings (qslice_selftest's PlantSource, our own invented instruction sequences), each with its twin (a WRONG expectation that must fail):
     two paths     a field stored on both arms of a branch: both stores listed, each with its site; a field stored after the others
     caller        a field that is the poster's argument: one row per caller site (3 and 9)
     init          a field no store reaches: `init Y` with the planted default; a store AFTER the init call wins (the loading-request shape)
     filled        a direct call given the struct address (not an init) between the store and the consumer: `filled by`, never the store's value
     pointer       the struct pointer is itself an argument (in_r4): each caller's own stack struct is read
B. The real re-finds on the local tree (skipped without generated/ or the q index): q msgbox reproduces the message-box scan of M100-MSG
   (293 post sites, 503 rows, 469 constant ids), the loading request of sub_82825A58 resolves 14 and 18.
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qdecomp  # noqa: E402
import qslice  # noqa: E402
import qevstruct  # noqa: E402
import qslice_selftest as base  # noqa: E402

check, twin = base.check, base.twin


def vals(row, off):
    return sorted((x['v'] for x in row['fields'][off]), key=str)


def planted():
    tmp = tempfile.mkdtemp(prefix='qstruct_selftest_')
    try:
        S = base.PlantSource(tmp)
        C, I = 0x82100F00, 0x82100F80
        S.add(C, ['blr'])
        S.add(I, ['blr'])
        # --- two paths: +4 is 5 on one arm, 7 on the other; +0 is stored once, after the join
        P = 0x82100000
        S.add(P, ['cmpwi cr6,r3,0', 'beq cr6,<L1>', 'li r5,5', 'stw r5,84(r1)', 'b <L2>', ('label', 'L1'), 'li r5,7', 'stw r5,84(r1)',
                  ('label', 'L2'), 'addi r4,r1,80', 'li r6,2', 'stw r6,80(r1)', f'bl 0x{C:08x}', 'blr'])
        rows = qevstruct.struct_rows(S, P, 0x28, 'r4', [0, 4], 0)
        print(rows)
        check('two paths: +4 is 5 and 7, +0 is 2', len(rows) == 1 and vals(rows[0], 4) == [5, 7] and vals(rows[0], 0) == [2], rows)
        check('two paths: each value names its store', {x['at'] for x in rows[0]['fields'][4]} == {f'sub_{P:08X}+0xC', f'sub_{P:08X}+0x18'}, rows[0]['fields'][4])
        twin('two paths: only the 7', vals(rows[0], 4) == [7])
        # --- caller: +4 = the poster's r5; two callers pass 3 and 9
        Q, E1, E2 = 0x82100100, 0x82100200, 0x82100300
        S.add(Q, ['addi r4,r1,80', 'stw r5,84(r1)', f'bl 0x{C:08x}', 'blr'])
        S.add(E1, ['li r5,3', f'bl 0x{Q:08x}', 'blr'])
        S.add(E2, ['li r5,9', f'bl 0x{Q:08x}', 'blr'])
        S.edge(E1, E1 + 4, Q)
        S.edge(E2, E2 + 4, Q)
        rows = qevstruct.struct_rows(S, Q, 0x8, 'r4', [4], 3)
        got = sorted((r['via'][0], vals(r, 4)[0]) for r in rows)
        check('caller: one row per caller site (3 and 9)', got == [(f'sub_{E1:08X}+0x4', 3), (f'sub_{E2:08X}+0x4', 9)], rows)
        twin('caller: a single row', len(rows) == 1)
        rows0 = qevstruct.struct_rows(S, Q, 0x8, 'r4', [4], 0)
        check('caller: K = 0 leaves the argument', len(rows0) == 1 and rows0[0]['fields'][4][0]['v'] == ('ARG', 'r5'), rows0)
        # --- init: +8 never stored (the init's default 77), +4 stored after the init call
        R = 0x82100400
        S.add(R, ['addi r3,r1,80', f'bl 0x{I:08x}', 'li r5,9', 'stw r5,84(r1)', 'addi r4,r1,80', f'bl 0x{C:08x}', 'blr'])
        rows = qevstruct.struct_rows(S, R, 0x14, 'r4', [4, 8], 0, {I: {4: 1, 8: 77}})
        check('init: the store after the init wins, the unstored field is the default', vals(rows[0], 4) == [9] and vals(rows[0], 8) == [77], rows)
        twin('init: the init default 1 for +4', vals(rows[0], 4) == [1])
        # --- filled: without naming I an init, the struct was given to a callee after nothing stored: filled by
        rows = qevstruct.struct_rows(S, R, 0x14, 'r4', [4, 8], 0)
        check('filled: a call given the struct stops the walk', vals(rows[0], 4) == [9] and vals(rows[0], 8) == [f'filled by sub_{I:08X}'], rows)
        T = 0x82100500
        S.add(T, ['li r5,4', 'stw r5,84(r1)', 'addi r3,r1,80', f'bl 0x{I:08x}', 'addi r4,r1,80', f'bl 0x{C:08x}', 'blr'])
        rows = qevstruct.struct_rows(S, T, 0x14, 'r4', [4], 0)
        check('filled: a store BEFORE the filling call is not the value', vals(rows[0], 4) == [f'filled by sub_{I:08X}'], rows)
        twin('filled: the earlier store 4 is trusted', vals(rows[0], 4) == [4])
        # --- pointer: F2 passes its in_r4 on; G builds the struct and passes it
        F2, G = 0x82100600, 0x82100700
        S.add(F2, [f'bl 0x{C:08x}', 'blr'])
        S.add(G, ['addi r4,r1,80', 'li r5,6', 'stw r5,84(r1)', f'bl 0x{F2:08x}', 'blr'])
        S.edge(G, G + 12, F2)
        rows = qevstruct.struct_rows(S, F2, 0x0, 'r4', [4], 3)
        check('pointer: the caller\'s struct is read', len(rows) == 1 and vals(rows[0], 4) == [6] and rows[0]['func'] == f'sub_{G:08X}', rows)
        rows = qevstruct.struct_rows(S, F2, 0x0, 'r4', [4], 0)
        check('pointer: K = 0 says not on the stack', 'err' in rows[0], rows)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def real():
    try:
        import q as qmod
        con = qmod.connect(qmod.DB)
    except Exception as e:  # noqa: BLE001
        print(f'SKIP B (no q index: {e})')
        return
    if not os.path.isdir(qdecomp.GEN):
        print('SKIP B (no generated/ tree)')
        return
    import qmsgbox
    src = qslice.QSource(con)
    f = 0x82825A58
    rows = qevstruct.struct_rows(src, f, 0x49C, 'r3', [0], 0, {0x827E5848: {}})
    v = {x['v'] for x in rows[0]['fields'][0]}
    check('real: sub_82825A58 loading request posts 14 and 18', {14, 18} <= v, v)
    twin('real: sub_82825A58 loading request posts 99', 99 in v)
    sites = qmsgbox.post_sites(con)
    rs = qmsgbox.scan(src, sites)
    n_const = sum(1 for r in rs if qmsgbox.constant(r))
    cat = qmsgbox.catalogue(rs, {})
    print(f'msgbox: {len(sites)} sites, {len(rs)} rows, {n_const} constant, {len(cat)} ids')
    check('real: msgbox sites / rows / constant rows', (len(sites), len(rs), n_const) == (293, 503, 469), (len(sites), len(rs), n_const))
    ids = {e['id'] for e in cat}
    check('real: msgbox ids (the scan\'s 391 and the 4 the path walk adds from sub_82B0D6E0)', len(ids) == 395 and {50175, 53244} <= ids, len(ids))
    twin('real: msgbox 391 ids exactly', len(ids) == 391)
    try:
        lr = qmsgbox.layout_rows()
    except Exception as e:  # noqa: BLE001 - no menu pacs here
        print(f'SKIP layout (no pacs: {e})')
        return
    lid = {r['id'][0]: r['kind'][0] for r in lr}
    check('real: 28 layout command rows (14 per menu pac), ids 52068 / 50868 / 50866, 50870 kind 2',
          len(lr) == 28 and {52068, 50868, 50866} <= set(lid) and lid.get(50870) == 2, (len(lr), sorted(lid)))
    twin('real: layout 50870 is kind 0', lid.get(50870) == 0)


def main():
    planted()
    if '--no-real' not in sys.argv:
        real()
    print(f'qevstruct_selftest: {len(base.FAILS)} failed, {len(base.TWINS_MISSED)} twins not caught')
    return 1 if base.FAILS else 2 if base.TWINS_MISSED else 0


if __name__ == '__main__':
    sys.exit(main())
