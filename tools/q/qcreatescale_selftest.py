"""Self-test of `q createscale` and `q slice --arg` (S111 CREATEX): `py -I tools/q/qcreatescale_selftest.py [--no-real]`   (0 game runs)

A. Planted listings (our own invented instruction sequences in a TEMP folder; no game code), each walked by the production path
   (qslice.build_facts, then qcreatescale.analyse / run), with its twin (the same check against a WRONG expectation must FAIL):
     no store       a site that inits the record and creates through a slot-56 call with nothing stored at +0: scale = the init's 10.0
     store          the same with a float 1.0 stored at +0 between the two: scale 1
     expression     a store of a loaded word: an expression, not a constant
     init only      an init whose record no create takes: one row, no create
     verdicts       ours read from planted source text: no create scale = MISMATCH against 10; ApplyCreateScale(m, kDefault 10) = OK
     slice --arg    `q slice ... --arg r5+4` reads the stored word at a call argument; a non-stack pointer is a load; a non-call is refused
B. The real re-finds on the local tree: the blood splat (sub_824ECAD8 +0x590 -> +0x6B8) reads MISMATCH on the tip (game 10, ours none), every
   crowd family OK, key 31000 and 5000 scale 1. A machine without generated/ or the q index SKIPs part B.
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qcreatescale  # noqa: E402
import qslice  # noqa: E402
import qslice_selftest as ss  # noqa: E402  the planted source and the check / twin helpers
from qslice_selftest import check, twin  # noqa: E402

INIT_PL = 0x82300000
P1, P2, P3, P4, P5 = 0x82300100, 0x82300200, 0x82300300, 0x82300400, 0x82300500
# the create tail: ctr = [[in_r4]+56] (vtable slot 56), r5 = the record at sp+96
TAIL = ['lwz r11,0(r4)', 'lwz r12,56(r11)', 'mtctr r12', 'addi r5,r1,96', 'bctrl', 'blr']
ONE = 0x82310000          # a planted image word holding the float 1.0
BLOOD_H = '''inline bool LoadBloodEntry(launcher::GameFiles& files, int arena, Model& m) {
  FinishModel(m);
%s  return true;
}
'''


def plant():
    tmp = tempfile.mkdtemp(prefix='qcreatescale_selftest_')
    S = ss.PlantSource(tmp)
    S.words[qcreatescale.INIT_WORD] = 0x41200000        # 10.0
    S.words[ONE] = 0x3F800000                           # 1.0
    S.add(INIT_PL, ['blr'])
    # P1: init, nothing stored, create (init +4, create +0x18)
    S.add(P1, ['addi r3,r1,96', f'bl 0x{INIT_PL:08x}'] + TAIL)
    S.edge(P1, P1 + 4, INIT_PL)
    # P2: init, a float 1.0 stored at +0, create
    S.add(P2, ['addi r3,r1,96', f'bl 0x{INIT_PL:08x}', 'lis r11,-32207', 'lfs f0,0(r11)', 'stfs f0,96(r1)'] + TAIL)
    S.edge(P2, P2 + 4, INIT_PL)
    # P3: init, a loaded word stored at +0 (an expression), create
    S.add(P3, ['addi r3,r1,96', f'bl 0x{INIT_PL:08x}', 'lis r11,-32207', 'lwz r6,4(r11)', 'stw r6,96(r1)'] + TAIL)
    S.edge(P3, P3 + 4, INIT_PL)
    # P4: init, no create anywhere
    S.add(P4, ['addi r3,r1,96', f'bl 0x{INIT_PL:08x}', 'blr'])
    S.edge(P4, P4 + 4, INIT_PL)
    # P5: slice --arg: a word stored at record +4, then a call with the record in r5 and in_r3 in r3
    S.add(P5, ['li r4,7', 'stw r4,100(r1)', 'addi r5,r1,96', f'bl 0x{INIT_PL:08x}', 'blr'])
    return tmp, S


def planted():
    tmp, S = plant()
    root = tempfile.mkdtemp(prefix='qcreatescale_ours_')
    try:
        rows = qcreatescale.analyse(S, P1, INIT_PL)
        print(rows)
        g = rows[0]['game'] if rows else {}
        check('no store: the init default 10', len(rows) == 1 and rows[0]['create'] == 0x18 and g.get('kind') == 'const' and g['value'] == 10.0, str(rows))
        twin('no store: reads 1', len(rows) == 1 and g.get('value') == 1.0)
        rows = qcreatescale.analyse(S, P2, INIT_PL)
        g = rows[0]['game'] if rows else {}
        check('store: the stored 1.0', len(rows) == 1 and g.get('kind') == 'const' and g['value'] == 1.0, str(rows))
        twin('store: reads 10 (the default)', len(rows) == 1 and g.get('value') == 10.0)
        rows = qcreatescale.analyse(S, P3, INIT_PL)
        g = rows[0]['game'] if rows else {}
        check('expression: a loaded word', len(rows) == 1 and g.get('kind') == 'expr' and '0x82310004' in g.get('text', ''), str(rows))
        twin('expression: a constant', len(rows) == 1 and g.get('kind') == 'const')
        rows = qcreatescale.analyse(S, P4, INIT_PL)
        check('init only: one row, no create', len(rows) == 1 and rows[0]['create'] is None, str(rows))
        twin('init only: has a create', len(rows) == 1 and rows[0]['create'] is not None)
        # verdicts: planted ours source
        qcreatescale.FAMILIES.extend([(P1, None, 'planted blood', 'blood'), (P2, None, 'planted raw', 'raw')])
        os.makedirs(os.path.join(root, 'src', 'assets'))
        asset = os.path.join(root, 'src', 'assets', 'asset_load.h')
        with open(asset, 'w') as fh:
            fh.write(BLOOD_H % '')
        v = {r['creator']: r['verdict'] for r in qcreatescale.run(S, root, INIT_PL)}
        check('verdict: no create scale in ours against the game\'s 10 = MISMATCH', v.get(P1) == 'MISMATCH', str(v))
        check('verdict: raw ours against the game\'s 1.0 = OK', v.get(P2) == 'OK', str(v))
        twin('verdict: no create scale reads OK', v.get(P1) == 'OK')
        with open(asset, 'w') as fh:
            fh.write(BLOOD_H % '  create_scale::ApplyCreateScale(m, create_scale::kDefaultCreateScale);\n')
        with open(os.path.join(root, 'src', 'assets', 'model_create_scale.h'), 'w') as fh:
            fh.write('inline constexpr float kDefaultCreateScale = 10.f;\n')
        v = {r['creator']: r['verdict'] for r in qcreatescale.run(S, root, INIT_PL)}
        check('verdict: ApplyCreateScale(kDefaultCreateScale = 10) = OK', v.get(P1) == 'OK', str(v))
        twin('verdict: it still reads MISMATCH', v.get(P1) == 'MISMATCH')
        # a crowd site that names a constant of the header (kCrowdCreateScale = kDefaultCreateScale = 10): resolved through the header
        with open(os.path.join(root, 'src', 'assets', 'model_create_scale.h'), 'w') as fh:
            fh.write('inline constexpr float kDefaultCreateScale = 10.f;\ninline constexpr float kCrowdCreateScale = kDefaultCreateScale;\n')
        with open(os.path.join(root, 'src', 'assets', 'crowd_prop_game_space.cpp'), 'w') as fh:
            fh.write('bool ConvertJboy(int x) {\n  constexpr float s = create_scale::kCrowdCreateScale;\n  return true;\n}\n')
        ov = qcreatescale.ours_value('crowd_prop', root)
        check('ours: a crowd constant resolves through the header chain to 10', ov[0] == 10.0 and ov[2], str(ov))
        with open(os.path.join(root, 'src', 'assets', 'model_create_scale.h'), 'w') as fh:
            fh.write('inline constexpr float kDefaultCreateScale = 10.f;\ninline constexpr float kCrowdCreateScale = 1.f;\n')
        ov = qcreatescale.ours_value('crowd_prop', root)
        twin('ours: the header\'s crowd constant changed to 1 still reads 10', ov[0] == 10.0)
        # q slice --arg
        lines, expr = ss.run(S, P5, 0xC, 'arg:r5+4')
        check('slice --arg r5+4: the stored 7', expr == '7' and ss.has(lines, 'local[sp+100]'), expr + ' / ' + '\n'.join(lines))
        twin('slice --arg r5+4: reads the record +0 slot', ss.has(lines, 'local[sp+96]'))
        lines, expr = ss.run(S, P5, 0xC, 'arg:r5')
        check('slice --arg r5: no store here (the callee wrote it)', ss.has(lines, 'no store to the slot'), '\n'.join(lines))
        try:
            ss.run(S, P5, 0x0, 'arg:r5')
            check('slice --arg at a non-call is refused', False, 'no error')
        except qslice.SliceError as e:
            check('slice --arg at a non-call is refused', 'not a call' in str(e), str(e))
    finally:
        del qcreatescale.FAMILIES[-2:]
        shutil.rmtree(tmp, ignore_errors=True)
        shutil.rmtree(root, ignore_errors=True)


def real():
    try:
        import q as qmod
        con = qmod.connect(qmod.DB)
    except Exception as e:  # noqa: BLE001 - no q index on this machine
        print(f'SKIP B (no q index: {e})')
        return
    if not os.path.isdir(qcreatescale.qdecomp.GEN):
        print('SKIP B (no generated/ tree)')
        return
    src = qslice.QSource(con)
    rows = qcreatescale.run(src)
    print('\n'.join(qcreatescale.table_lines(rows)))

    def pick(creator, init):
        return [r for r in rows if r['creator'] == creator and r['init'] == init]
    blood = pick(0x824ECAD8, 0x590)
    has_scale = os.path.exists(os.path.join(qcreatescale.REPO_OURS, 'src', 'assets', 'model_create_scale.h'))
    check('real: blood splat 32000: game 10', len(blood) == 1 and blood[0]['game']['value'] == 10.0 and blood[0]['key'] == 32000, str(blood))
    check('real: blood splat: ours none = MISMATCH on the tip before MATLIVE (OK once model_create_scale.h is in)',
          len(blood) == 1 and blood[0]['verdict'] == ('OK' if has_scale else 'MISMATCH'), str(blood))
    twin('real: blood splat reads the wrong verdict', len(blood) == 1 and blood[0]['verdict'] == ('MISMATCH' if has_scale else 'OK'))
    for init, key in ((0x20C, 31000), (0x64, 5000)):
        r = pick(0x824ECAD8, init)
        check(f'real: key {key} scale 1', len(r) == 1 and r[0]['key'] == key and r[0]['game']['value'] == 1.0, str(r))
    crowd = [r for r in rows if r['creator'] in (0x8250A200, 0x82507408, 0x825074C8, 0x8250ACA8, 0x8250B1E8) and r['create'] is not None]
    check('real: every crowd family OK at scale 10', len(crowd) == 5 and all(r['verdict'] == 'OK' and r['game']['value'] == 10.0 for r in crowd),
          str([(r['creator_name'], r['verdict']) for r in crowd]))
    twin('real: a crowd family reads MISMATCH', any(r['verdict'] == 'MISMATCH' for r in crowd))
    check('real: no unclassed creator', not any(r['family'].startswith('(unclassed') for r in rows), str([r['creator_name'] for r in rows if r['family'].startswith('(unclassed')]))


def main():
    planted()
    if '--no-real' not in sys.argv:
        real()
    print(f'qcreatescale_selftest: {len(ss.FAILS)} failed, {len(ss.TWINS_MISSED)} twins not caught')
    if ss.FAILS:
        return 1
    if ss.TWINS_MISSED:
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
