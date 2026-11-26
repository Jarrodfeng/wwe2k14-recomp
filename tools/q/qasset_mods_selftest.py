"""qasset_mods_selftest (S111 MODINDEX): proves the MODS folder in `q asset` on a planted game folder + mods folder in a TEMP dir (0 game runs, nothing from the real game,
the player's own mods folder and settings never read).

Planted: game pac\\ch\\ch900.pac (yCh_skin_flex WITH g_fDifAmbLevel); mods superstars\\frame\\info\\catalog.dlc (a built frame), superstars\\library\\0000000A (card.txt
`name Dolph Mod`, pac\\ch\\ch497.pac: yCh_skin_flex WITHOUT g_fDifAmbLevel, sampler -> modtex, texture ModBody), superstars\\sources\\Mod Guy.pac, characters\\Viewer Guy.pac,
titantron\\343.mp4 (not a pac); a launcher settings file with USE MY SUPERSTARS on.
Checks: every mod pac is indexed with its source; a mod material is its own group beside the stock one (diffable: the missing parameter shows); --source game / mods /
mod:NAME narrow it, a bad --source is refused; a texture carries its source; the toggle turned off tags the mod off with NO re-read; no frame = off; an importer source and a
viewer character are tagged never read by the game; a CHANGED mod pac is re-read by the next query; a REMOVED one leaves the index; nothing is written into the mods folder;
q constdiff / q texdiff read the game rows by default and the mod rows on request; a missing asset.s5.db is seeded from the asset.s4.db beside it (left byte-identical);
WWE_QASSET_MODS=none drops the mod rows.
Run:  py -I tools/q/qasset_mods_selftest.py
"""
import hashlib
import json
import os
import shutil
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qasset_selftest as F     # noqa: E402  the fixture builders (jboy / texdir / dds / pach / epac) and run / check
from qasset_selftest import check, run, Fail     # noqa: E402

LIB = 'mod:superstars/library/0000000A'


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def char_pac(effect, with_amb, tex, texname):
    t = F.texdir([(texname, F.dds(b'DXT5', 8, 8, F.dxt5_block(40, 0, 0) * 4))])
    return F.epac([F.pach([(0, F.pach([(5, F.jboy(effect, with_amb=with_amb, tex=tex, nodes=('Bip', 'body'))), (10, t)]))])], [('EMD', '0000')])


def plant(tmp):
    game = os.path.join(tmp, 'game_data')
    mods = os.path.join(tmp, 'mods')
    write(os.path.join(game, 'extracted', 'pac', 'ch', 'ch900.pac'), char_pac('yCh_skin_flex', True, 'fixtex', 'StockBody'))
    write(os.path.join(mods, 'superstars', 'frame', 'info', 'catalog.dlc'), b'planted catalog')
    write(os.path.join(mods, 'superstars', 'library', '0000000A', 'card.txt'), b'card 1\nkey 0000000A\nname Dolph Mod\n')
    write(os.path.join(mods, 'superstars', 'library', '0000000A', 'pac', 'ch', 'ch497.pac'), char_pac('yCh_skin_flex', False, 'modtex', 'ModBody'))
    write(os.path.join(mods, 'superstars', 'sources', 'Mod Guy.pac'), char_pac('yCh_ModSrc', True, 'srctex', 'SrcBody'))
    write(os.path.join(mods, 'characters', 'Viewer Guy.pac'), char_pac('yCh_ViewerOnly', True, 'viewtex', 'ViewBody'))
    write(os.path.join(mods, 'titantron', '343.mp4'), b'not a pac')
    return game, mods


def set_stars(path, on):
    with open(path, 'w', encoding='utf-8') as f:
        json.dump({'superstars': on, 'mods_dir': ''}, f)


def snapshot(d):
    out = {}
    for dp, dn, fn in os.walk(d):
        for f in fn:
            p = os.path.join(dp, f)
            st = os.stat(p)
            out[os.path.relpath(p, d)] = (st.st_size, st.st_mtime_ns, hashlib.sha256(open(p, 'rb').read()).hexdigest())
    return out


def line_with(out, word):
    return next((ln for ln in out.splitlines() if word in ln), '')


def main():
    tmp = tempfile.mkdtemp(prefix='qasset_mods_st_')
    try:
        game, mods = plant(tmp)
        st_file = os.path.join(tmp, 'settings.json')
        set_stars(st_file, True)
        db = os.path.join(tmp, 'asset.db')
        env = {'WWE_QASSET_GAME': game, 'WWE_QASSET_DB': db, 'WWE_QASSET_MODS': mods, 'WWE_QASSET_SETTINGS': st_file, 'WWE_QASSET_BUDGET': '120'}
        before = snapshot(mods)
        rc, out = run(['info'], env)
        check(rc == 0 and 'pacs: 4' in out, 'the first query indexes the game pac and the 3 mod pacs (the .mp4 is not a pac)', out)
        check('(3 from the mods folder)' in out, 'the header counts the mod pacs', out)
        ln = line_with(out, LIB + ':')
        check('1 pacs, 1 models, 1 materials, 1 textures' in ln and 'ON (USE MY SUPERSTARS is on)' in ln and 'names: Dolph Mod' in ln,
              'info: the library character is one source with its counts, ON, and the name its card carries', out)
        check('off (an importer source' in line_with(out, 'mod:superstars/sources/Mod Guy:'), 'info: an importer source is tagged never read by the game', out)
        check('off (an Arena Viewer custom character' in line_with(out, 'mod:characters/Viewer Guy:'), 'info: a viewer character is tagged viewer only', out)
        rc, out = run(['mat', 'yCh_skin_flex'], env)
        check('2 mesh materials in 2 distinct' in out, 'mat: the mod material is its own group beside the stock one', out)
        check(('source: %s "Dolph Mod" [ON: USE MY SUPERSTARS is on]' % LIB) in out, 'mat: the mod group names its source, the character and ON', out)
        mod_line = line_with(out, 'modtex')
        stock_line = line_with(out, 'fixtex')
        check(mod_line and 'g_fDifAmbLevel' not in mod_line and 'g_fDifAmbLevel 1' in stock_line, 'mat: DIFFABLE: the parameter the mod lacks shows against the stock material', out)
        check('mods/superstars/library/0000000A/pac/ch/ch497.pac' in out, 'mat: the mod row is located in the mods folder', out)
        rc, out = run(['mat', 'yCh_skin_flex', '--source', 'game'], env)
        check('1 mesh materials' in out and 'fixtex' in out and 'modtex' not in out, '--source game: the stock material only', out)
        rc, out = run(['mat', 'yCh_skin_flex', '--source', 'mods'], env)
        check('1 mesh materials' in out and 'modtex' in out and 'fixtex' not in out, '--source mods: the mod material only', out)
        rc, out = run(['mat', 'yCh_skin_flex', '--source', LIB], env)
        check('1 mesh materials' in out and 'modtex' in out, '--source mod:NAME: that mod only', out)
        rc, out = run(['mat', 'yCh_skin_flex', '--source', 'mod:characters'], env)
        check('0 mesh materials' in out, 'PLANTED MISS: --source mod:characters holds no yCh_skin_flex', out)
        rc, out = run(['mat', 'yCh_skin_flex', '--source', 'bogus'], env)
        check(rc != 0 and '--source' in out, 'PLANTED MISS: a bad --source is refused', out)
        rc, out = run(['mat', 'yCh_skin_flex', 'ch497'], env)
        check('1 mesh materials' in out and 'modtex' in out, 'mat with the mod pac stem (ch497) narrows to it', out)
        rc, out = run(['tex', 'ModBody'], env)
        check(('[%s "Dolph Mod" [ON' % LIB) in out, 'tex: a mod texture carries its source', out)
        check(snapshot(mods) == before, 'nothing was written into the mods folder (every file: size, mtime, sha256)')
        set_stars(st_file, False)
        rc, out = run(['mat', 'yCh_skin_flex'], env)
        check('[off: USE MY SUPERSTARS is off]' in out and '0 pacs changed' in out, 'the toggle off: the mod is tagged off, and NOTHING was re-read', out)
        set_stars(st_file, True)
        os.remove(os.path.join(mods, 'superstars', 'frame', 'info', 'catalog.dlc'))
        rc, out = run(['mat', 'yCh_skin_flex'], env)
        check('[off: no frame built' in out, 'no frame built: a library character is tagged off', out)
        write(os.path.join(mods, 'superstars', 'frame', 'info', 'catalog.dlc'), b'planted catalog')
        # a planted change: the mod pac rewritten WITH the parameter; the next query must re-read it by itself
        p = os.path.join(mods, 'superstars', 'library', '0000000A', 'pac', 'ch', 'ch497.pac')
        write(p, char_pac('yCh_skin_flex', True, 'modtex', 'ModBody'))
        st = os.stat(p)
        os.utime(p, ns=(st.st_atime_ns, st.st_mtime_ns + 5_000_000_000))
        rc, out = run(['mat', 'yCh_skin_flex', '--source', 'mods'], env)
        check('refreshed just now: 1 pacs re-read' in out and 'g_fDifAmbLevel 1' in line_with(out, 'modtex'), 'PLANTED CHANGE: the changed mod pac was re-read and answers its new parameter', out)
        os.remove(os.path.join(mods, 'characters', 'Viewer Guy.pac'))
        rc, out = run(['info'], env)
        check('pacs: 3' in out and 'Viewer Guy' not in out, 'a REMOVED mod pac leaves the index', out)
        # the consumers: q constdiff / q texdiff read the game rows by default, the mod rows on request
        import qconstdiff
        import qtexdiff
        _m, tg = qconstdiff.material_names(db)
        _m, tm = qconstdiff.material_names(db, source='mods')
        check(tg == {'yCh_skin_flex': 1} and tm == {'yCh_skin_flex': 1, 'yCh_ModSrc': 1}, 'q constdiff material_names: game rows by default (%s), the mods on request (%s)' % (tg, tm))
        _n, tg, _t = qtexdiff.sampler_names(db)
        _n, ta, _t = qtexdiff.sampler_names(db, source='all')
        check(tg == {'yCh_skin_flex': 1} and ta == {'yCh_skin_flex': 2, 'yCh_ModSrc': 1}, 'q texdiff sampler_names: game rows by default (%s), all on request (%s)' % (tg, ta))
        # the seed: a missing asset.s5.db of the default name is copied from the asset.s4.db beside it (same tables), then only new / changed pacs are read
        idx = os.path.join(tmp, 'out', 'harness', 'index')
        os.makedirs(idx, exist_ok=True)
        s4 = os.path.join(idx, 'asset.s4.db')
        shutil.copyfile(db, s4)
        c = sqlite3.connect(s4)
        c.execute("UPDATE meta SET value='4' WHERE key='schema'")
        c.commit()
        c.close()
        h4 = hashlib.sha256(open(s4, 'rb').read()).hexdigest()
        e3 = {k: v for k, v in env.items() if k != 'WWE_QASSET_DB'}
        rc, out = run(['info'], e3)
        import qasset as QA
        s5 = os.path.join(idx, QA.db_name())      # S111 ASSETCOV: schema 6 now; its seed (qasset_cov.seed) takes the s4 copy, whose tables here already hold vcolor / magic: 0 pacs re-read
        check(rc == 0 and 'seeded from' in out and 'indexing 0 pacs' in out and 'pacs: 3' in out, 'seed: %s made from asset.s4.db, 0 pacs read' % QA.db_name(), out)
        c = sqlite3.connect(s5)
        sv = c.execute("SELECT value FROM meta WHERE key='schema'").fetchone()[0]
        c.close()
        check(sv == str(QA.SCHEMA) and hashlib.sha256(open(s4, 'rb').read()).hexdigest() == h4, 'seed: the new file is schema %d, the s4 file byte-identical' % QA.SCHEMA)
        rc, out = run(['info'], dict(env, WWE_QASSET_MODS='none'))
        check('pacs: 1' in out and 'no mod pacs indexed' in out, 'WWE_QASSET_MODS=none: the mod rows leave the index', out)
        # (review) a mod pac with a stock pac's stem: pac_rows lists the game's pac FIRST (q asset slot takes rows[0]; 'mods/' sorts before 'pac/' by path)
        write(os.path.join(mods, 'characters', 'ch900.pac'), char_pac('yCh_ViewerOnly', True, 'viewtex', 'ViewBody'))
        rc, out = run(['info'], env)
        import qasset
        c = sqlite3.connect('file:%s?mode=ro' % db, uri=True)
        c.row_factory = sqlite3.Row
        try:
            prs = [r['path'] for r in qasset.pac_rows(c, 'ch900')]
        finally:
            c.close()
        check(prs == ['pac/ch/ch900.pac', 'mods/characters/ch900.pac'], 'PLANTED STEM CLASH: pac_rows lists the game pac first, the mod pac of the same stem after it (%s)' % prs)
        # (review) an unreadable launcher settings file degrades quietly to the launcher's defaults (USE MY SUPERSTARS off; mods folder <game data>\mods)
        with open(st_file, 'wb') as f:
            f.write(b'\xff{ not json')
        rc, out = run(['info'], env)
        check(rc == 0 and 'Traceback' not in out and 'USE MY SUPERSTARS off' in out and 'off (USE MY SUPERSTARS is off)' in line_with(out, LIB + ':'),
              'a damaged settings file: no error, USE MY SUPERSTARS read as off, the library character tagged off', out)
        import qasset_mods
        keep = {k: os.environ.get(k) for k in ('WWE_QASSET_MODS', 'WWE_QASSET_SETTINGS', 'WWE_QASSET_GAME')}
        try:
            os.environ.pop('WWE_QASSET_MODS', None)
            os.environ.pop('WWE_QASSET_GAME', None)
            os.environ['WWE_QASSET_SETTINGS'] = st_file
            qasset_mods._SETTINGS.clear()
            got = qasset_mods.mods_dir(game)
        finally:
            for k, v in keep.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
            qasset_mods._SETTINGS.clear()
        check(got == os.path.join(game, 'mods'), 'a damaged settings file: the mods folder falls back to <game data>\\mods (%s)' % got)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('ALL PASS')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Fail as e:
        print('FAIL  ' + str(e))
        sys.exit(1)
