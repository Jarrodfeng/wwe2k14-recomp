"""qasset_cov_selftest (S111 ASSETCOV): proves `q asset params`, `q asset vcolor` and the REPLACED? signal on a FIXTURE game folder in a TEMP dir (0 game runs, nothing from
the real game), each with a PLANTED twin that must NOT match; then, when the real index exists, three read-only real-tree checks.

Fixture (on top of qasset_selftest.write_fixture): pac\\ch\\ch902.pac = seven models with planted parameter sets (yCh_skin x5 with / without g_fSpecLev / g_fSpecPow, yCh_skin_flex,
yProp_Spec with g_fSpecularLev); pac\\ch\\ch903.pac = one model of 2 meshes, mesh 0 with a planted vertex-colour stream (3 vertices), mesh 1 without one. Both fixture ch pacs are EPAC, so
they read as REPLACED?; the planted stock twin is the same pac with its magic recorded as XCTD (the index's own column).
Run:  py -I tools/q/qasset_cov_selftest.py
"""
import os
import shutil
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qasset_selftest as F     # noqa: E402  the fixture builders (jboy / texdir / pach / epac) and run / check
from qasset_selftest import check, run, jout, Fail     # noqa: E402


def write_cov_fixture(game):
    pac_dir = os.path.join(game, 'extracted', 'pac', 'ch')
    tex = F.texdir([('FixCap', F.dds(b'DXT5', 8, 8, F.dxt5_block(40, 0, 0) * 4))])
    j = F.jboy
    spec = F.pach([(1, j('yCh_skin', extra=[('g_fSpecLev', 0.5)])),
                   (2, j('yCh_skin', extra=[('g_fSpecLev', 0.5), ('g_fSpecPow', 20.0)])),
                   (3, j('yCh_skin', extra=[('g_fSpecPow', 20.0)])),
                   (4, j('yCh_skin', extra=[('g_fSpecLev', 0.8)])),
                   (5, j('yCh_skin')),
                   (6, j('yCh_skin_flex', extra=[('g_fSpecLev', 0.5)])),
                   (7, j('yProp_Spec', extra=[('g_fSpecularLev', 0.3)])),
                   (10, tex)])
    with open(os.path.join(pac_dir, 'ch902.pac'), 'wb') as f:
        f.write(F.epac([spec], [('EMD', '0000')]))
    vc = [[(8, 50, 60, 70), (8, 50, 61, 70), (24, 50, 60, 70)], None]
    with open(os.path.join(pac_dir, 'ch903.pac'), 'wb') as f:
        f.write(F.epac([F.pach([(5, j('yCh_skin', nmesh=2, vc=vc)), (10, tex)])], [('EMD', '0000')]))


def mesh_of(j, mesh):
    return [x for x in j['meshes'] if x['mesh'] == mesh]


def main():
    tmp = tempfile.mkdtemp(prefix='qasset_cov_st_')
    try:
        game = os.path.join(tmp, 'game_data')
        db = os.path.join(tmp, 'asset.db')
        env = {'WWE_QASSET_GAME': game, 'WWE_QASSET_DB': db, 'WWE_QASSET_BUDGET': '120', 'WWE_QASSET_MODS': 'none'}
        F.write_fixture(game)
        write_cov_fixture(game)
        # --- params
        rc, out = run(['params', 'yCh_skin', '--json'], env)
        check(rc == 0, 'params yCh_skin runs', out)
        j = jout(out)
        check(j['matched'] == 7 and j['selected'] == 7, 'params yCh_skin (exact effect): 7 meshes (5 in ch902 + the 2 of ch903; yCh_skin_flex is NOT counted)', out)
        rc, out = run(['params', 'yCh_skin', '--has', 'g_fSpecLev', '--lacks', 'g_fSpecPow', '--json'], env)
        j = jout(out)
        check(j['selected'] == 2 and j['matched'] == 7, "BODYSPEC's question: yCh_skin meshes that name g_fSpecLev without g_fSpecPow = 2 of 7 (planted: A and D; B has both, C only Pow, E neither)", out)
        hist = {tuple(x['value']): x['meshes'] for x in j['values']['g_fSpecLev']}
        check(hist == {(0.5,): 1, (0.8,): 1}, 'the value histogram of g_fSpecLev among them: 0.5 x1, 0.8 x1', out)
        rc, out = run(['params', 'yCh_skin', '--has', 'g_fSpecLev', '--lacks', 'g_fSpecPow'], env)
        check('2 of 7' in out and 'g_fSpecLev' in out and '0.8' in out, 'the text form says 2 of 7 and lists the histogram', out)
        rc, out = run(['params', 'yCh_skin', '--like', '--has', 'g_fSpecLev', '--lacks', 'g_fSpecPow', '--json'], env)
        check(jout(out)['selected'] == 3 and jout(out)['matched'] == 8, '--like yCh_skin takes yCh_skin_flex too: 3 of 8', out)
        rc, out = run(['params', '*', '--has', 'g_fSpecularLev', '--json'], env)
        check(jout(out)['selected'] == 1 and [x['effect'] for x in jout(out)['effects']] == ['yProp_Spec'], 'BODYSPEC: any material naming g_fSpecularLev: exactly the 1 planted yProp_Spec mesh', out)
        rc, out = run(['params', 'yCh_skin', '--has', 'g_fSpecularLev', '--json'], env)
        check(jout(out)['selected'] == 0, 'PLANTED MISS: no yCh_skin mesh names g_fSpecularLev (the name of another effect is not borrowed)', out)
        rc, out = run(['params', 'yCh_skin', '--has', 'g_fspeclev', '--lacks', 'g_fSpecPow', '--json'], env)
        check(jout(out)['selected'] == 2, 'a parameter name matches case-insensitively', out)
        rc, out = run(['params', 'yCh_skin', '--source', 'mods', '--json'], env)
        check(jout(out)['matched'] == 0, '--source mods: the fixture has no mod pac, so 0 (the source filter is applied)', out)
        rc, out = run(['params', 'NoSuchEffect', '--json'], env)
        check(rc == 0 and jout(out)['matched'] == 0, 'PLANTED MISS: an effect that does not exist matches nothing', out)
        # --- vcolor
        rc, out = run(['vcolor', 'ch903', '--json'], env)
        check(rc == 0, 'vcolor ch903 runs', out)
        j = jout(out)
        m0 = mesh_of(j, 0)
        ch = {c['ch']: c for c in m0[0]['channels']} if m0 else {}
        check(m0 and m0[0]['nvert'] == 3 and (ch[24]['min'], ch[24]['max'], ch[24]['distinct']) == (8, 24, 2) and ch[24]['top'] == [[8, 2], [24, 1]],
              'vcolor ch903 mesh 0: byte 24 min 8, max 24, 2 distinct, top 8 x2 then 24 x1 (planted)', out)
        check((ch[25]['min'], ch[25]['max'], ch[25]['distinct']) == (50, 50, 1) and (ch[26]['min'], ch[26]['max'], ch[26]['distinct']) == (60, 61, 2) and ch[27]['top'] == [[70, 3]],
              'vcolor ch903 mesh 0: byte 25 constant 50, byte 26 60..61 (2 distinct), byte 27 70 x3 (planted)', out)
        m1 = mesh_of(j, 1)
        check(m1 and m1[0]['channels'] == [], 'PLANTED MISS: mesh 1 has no colour stream and gets NO invented histogram', out)
        rc, out = run(['vcolor', 'ch903', '--mesh', '0'], env)
        check('mesh 0' in out and 'mesh 1' not in out and 'min 8 max 24 distinct 2' in out and '8 x2' in out, 'vcolor --mesh 0 prints only that mesh, with min / max / distinct / top values', out)
        rc, out = run(['vcolor', 'ch903', '--mesh', '7', '--json'], env)
        check(jout(out)['meshes'] == [], 'PLANTED MISS: --mesh 7 does not exist: no rows', out)
        rc, out = run(['vcolor', 'nosuchpac'], env)
        check('no pac matches' in out, 'PLANTED MISS: an unknown pac is said so', out)
        # --- REPLACED?
        rc, out = run(['pac', 'ch902'], env)
        check('REPLACED?' in out and 'EPAC' in out, 'pac ch902: a game-folder ch pac whose magic is EPAC is marked REPLACED?', out)
        rc, out = run(['pac', 'zz_epk'], env)
        check('REPLACED?' not in out, 'PLANTED TWIN: a non-ch EPK8 pac is not marked', out)
        rc, out = run(['info', '--json'], env)
        check(rc == 0 and '{' in out, 'info --json runs', out)
        j = jout(out)
        check(j['replaced_ch_pacs']['count'] == 4 and 'pac/ch/ch902.pac' in j['replaced_ch_pacs']['pacs'], 'info: REPLACED? count 4 (the four fixture ch pacs are EPAC), ch902 named', out)
        c = sqlite3.connect(db)
        mg = dict(c.execute("SELECT path, magic FROM pacs WHERE path LIKE 'pac/ch/%' OR path LIKE '%zz_epk%'").fetchall())
        c.execute("UPDATE pacs SET magic='XCTD' WHERE path='pac/ch/ch902.pac'")
        c.commit()
        c.close()
        check(mg.get('pac/ch/ch902.pac') == 'EPAC' and mg.get('pac/zz_epk.pac') == 'EPK8', 'the index stores the pac file magic (EPAC / EPK8) at build time (%s)' % mg)
        r2 = dict(env, WWE_QASSET_NOREFRESH='1')
        rc, out = run(['pac', 'ch902'], r2)
        check('REPLACED?' not in out, 'PLANTED STOCK TWIN: the same ch pac recorded as XCTD is NOT marked', out)
        rc, out = run(['pac', 'ch903'], r2)
        check('REPLACED?' in out, 'the other fixture ch pac (still EPAC) is still marked', out)
        rc, out = run(['info', '--json'], r2)
        check(jout(out)['replaced_ch_pacs']['count'] == 3, 'info: REPLACED? count drops to 3 with the stock twin', out)
        import qasset_cov
        cases = [('pac/ch/ch1.pac', 'XCTD', False), ('pac/ch/ch1.pac', 'EPAC', True), ('pac/ch/ch1.pac', 'EPK8', True), ('pac/ch/ch1.pac', None, False),
                 ('mods/superstars/library/A/pac/ch/ch1.pac', 'EPAC', False), ('dlc/pac/ch/ch1.pac', 'EPAC', False), ('pac/evt/x.pac', 'EPAC', False)]
        bad = [(p, m, w) for p, m, w in cases if qasset_cov.is_replaced(p, m) != w]
        check(not bad, 'is_replaced: only a game-folder pac\\ch pac with a known non-XCTD magic (%s)' % bad)
        check(qasset_cov.magic_text(bytes([0x0F, 0xF5, 0x12, 0xED])) == 'XCTD' and qasset_cov.magic_text(b'EPAC') == 'EPAC' and qasset_cov.magic_text(b'\x01\x02\x03\x04') == '01020304', 'magic_text: XCTD / ascii / hex')
        # --- the s5 -> s6 seed: a schema-5 file (no vcolor table, no magic column) is copied, marked for re-read, the old file left byte-identical
        import hashlib
        idx = os.path.join(tmp, 'out', 'harness', 'index')
        os.makedirs(idx, exist_ok=True)
        s5 = os.path.join(idx, 'asset.s5.db')
        shutil.copyfile(db, s5)
        c = sqlite3.connect(s5)
        c.execute("UPDATE meta SET value='5' WHERE key='schema'")
        c.execute('DROP TABLE vcolor')
        c.execute('ALTER TABLE pacs DROP COLUMN magic')
        c.commit()
        nonraw = c.execute('SELECT COUNT(*) FROM pacs WHERE raw=0').fetchone()[0]
        c.close()
        h5 = hashlib.sha256(open(s5, 'rb').read()).hexdigest()
        e3 = {k: v for k, v in env.items() if k != 'WWE_QASSET_DB'}
        import qasset
        s6 = os.path.join(idx, qasset.db_name())
        # (review) the first query after the merge in the main checkout: its budget ends long before the re-read does (~6 min there). It must say PARTIAL and answer from the
        # seeded rows (the same counts as before the seed), never an emptied or half-wiped index. A tiny budget stops the fixture's re-read after at most one pac.
        rc, out = run(['params', 'yCh_skin', '--json'], dict(e3, WWE_QASSET_BUDGET='0.01'))
        c = sqlite3.connect(s6)
        left = c.execute('SELECT COUNT(*) FROM pacs WHERE mtime_ns = -1').fetchone()[0]
        c.close()
        check(rc == 0 and 'seeded from' in out and ('indexing %d pacs' % nonraw) in out and 'WARNING PARTIAL' in jout(out)['index'] and jout(out)['matched'] == 7 and left >= 1,
              'seed: the first query is PARTIAL (%d pacs still marked) and answers from the seeded rows: params yCh_skin = 7 meshes, as before the seed' % left, out)
        # (review) a second q while a live writer holds the lock (the first one still re-reading): it never builds, says STALE and answers from the same rows
        lk = s6 + '.lock'
        with open(lk, 'w') as f:
            f.write(str(os.getpid()))
        rc, out = run(['params', 'yCh_skin', '--json'], dict(e3, WWE_QASSET_LOCK_WAIT='1'))
        c = sqlite3.connect(s6)
        left2 = c.execute('SELECT COUNT(*) FROM pacs WHERE mtime_ns = -1').fetchone()[0]
        c.close()
        os.remove(lk)
        check(rc == 0 and 'WARNING STALE' in jout(out)['index'] and 'indexing' not in out and jout(out)['matched'] == 7 and left2 == left,
              'seed: a second q during the re-read (lock held) reads nothing, says STALE, answers 7 meshes', out)
        rc, out = run(['vcolor', 'ch903', '--json'], e3)
        c = sqlite3.connect(s6)
        sv = c.execute("SELECT value FROM meta WHERE key='schema'").fetchone()[0]
        nv = c.execute('SELECT COUNT(*) FROM vcolor').fetchone()[0]
        left3 = c.execute('SELECT COUNT(*) FROM pacs WHERE mtime_ns = -1').fetchone()[0]
        c.close()
        check(rc == 0 and ('indexing %d pacs' % left) in out and left3 == 0 and sv == str(qasset.SCHEMA) and nv >= 4 and hashlib.sha256(open(s5, 'rb').read()).hexdigest() == h5,
              'seed s5 -> s%d: copied, every decoded pac (%d) marked for re-read and read by the next queries (%d left), vcolor rows now present (%d), the s5 file byte-identical'
              % (qasset.SCHEMA, nonraw, left3, nv), out)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    real_proofs()
    print('ALL PASS')
    return 0


def real_proofs():
    here = os.path.dirname(os.path.dirname(HERE))
    import qasset
    cands = [os.environ.get('WWE_QASSET_DB', ''), os.path.join(here, 'out', 'harness', 'index', qasset.db_name()),
             os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(here))), 'out', 'harness', 'index', qasset.db_name())]
    dbp = next((c for c in cands if c and os.path.exists(c)), None)
    if not dbp:
        print('SKIP  real-tree proofs: no asset.s%d.db (run: py tools\\q\\q.py asset update)' % qasset.SCHEMA)
        return
    c = sqlite3.connect('file:%s?mode=ro' % dbp, uri=True)
    try:
        sv = c.execute("SELECT value FROM meta WHERE key='schema'").fetchone()[0]
        nv = c.execute('SELECT COUNT(*) FROM vcolor').fetchone()[0]
        nm = c.execute("SELECT COUNT(*) FROM pacs WHERE path GLOB 'pac/ch/*.pac' AND magic IS NULL").fetchone()[0]
        bad = c.execute("SELECT path, magic FROM pacs WHERE path GLOB 'pac/ch/*.pac' AND magic IS NOT NULL AND magic != 'XCTD'").fetchall()
    except sqlite3.Error:
        print('SKIP  real-tree proofs: the real index is not of this schema yet')
        return
    finally:
        c.close()
    if sv != str(qasset.SCHEMA) or nv == 0 or nm:
        print('SKIP  real-tree proofs: the real index is not fully re-read yet (%d vcolor rows, %d ch pacs without a magic)' % (nv, nm))
        return
    check(not bad, 'REAL: no game-folder pac\\ch pac is REPLACED? (non-XCTD magic: %s)' % bad[:5])


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Fail as e:
        print('FAIL  ' + str(e))
        sys.exit(1)
