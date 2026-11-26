"""qasset_census_selftest (S111 QCENSUS): proves `q asset params` --by / --sets / --pac / @pseudo-names / the cut line on a FIXTURE game folder in a TEMP dir (0 game runs).

Fixture: pac\\ch\\ch910.pac (family characters) = yCh_skin x5: A tech 0x11 g_A 1, B tech 0x11 g_A 1, C tech 0x22 g_A 2, D tech 0x11 no amb but g_A 5, E tech 0x11 g_A 6 g_B g_C (5 names);
pac\\ch\\ch911.pac = the twin pac: yCh_skin tech 0x11 g_A 1 (the SAME name and value, another pac). pac\\evt\\evtobj.pac (family evt objects) = yCh_skin tech 0x33 g_A 3 (the same effect and
name in ANOTHER FAMILY with another technique word) and yProp_Fix tech 0x11 g_A 9 (another effect).
Each twin is a row that a wrong grouping / filter would fold in: family twin (evt yCh_skin g_A 3 must not appear in the characters group), technique twin (0x22 / 0x33 must not be
counted as 0x11), pac twin (ch911 must not leak into --pac ch910), effect twin (yProp_Fix must not count as yCh_skin).
Run:  py -I tools/q/qasset_census_selftest.py
"""
import os
import shlex
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qasset_selftest as F     # noqa: E402
from qasset_selftest import check, run, jout, Fail     # noqa: E402


def write_census_fixture(game):
    ch = os.path.join(game, 'extracted', 'pac', 'ch')
    evt = os.path.join(game, 'extracted', 'pac', 'evt')
    os.makedirs(ch, exist_ok=True)
    os.makedirs(evt, exist_ok=True)
    tex = F.texdir([('FixCap', F.dds(b'DXT5', 8, 8, F.dxt5_block(40, 0, 0) * 4))])
    j = F.jboy
    p1 = F.pach([(1, j('yCh_skin', tech=0x11, extra=[('g_A', 1.0)])), (2, j('yCh_skin', tech=0x11, extra=[('g_A', 1.0)])),
                 (3, j('yCh_skin', tech=0x22, extra=[('g_A', 2.0)])), (4, j('yCh_skin', tech=0x11, with_amb=False, extra=[('g_A', 5.0)])),
                 (5, j('yCh_skin', tech=0x11, extra=[('g_A', 6.0), ('g_B', 1.0), ('g_C', 1.0)])), (10, tex)])
    with open(os.path.join(ch, 'ch910.pac'), 'wb') as f:
        f.write(F.epac([p1], [('EMD', '0000')]))
    with open(os.path.join(ch, 'ch911.pac'), 'wb') as f:
        f.write(F.epac([F.pach([(1, j('yCh_skin', tech=0x11, extra=[('g_A', 1.0)])), (10, tex)])], [('EMD', '0000')]))
    p3 = F.pach([(1, j('yCh_skin', tech=0x33, extra=[('g_A', 3.0)])), (2, j('yProp_Fix', tech=0x11, extra=[('g_A', 9.0)])), (10, tex)])
    with open(os.path.join(evt, 'evtobj.pac'), 'wb') as f:
        f.write(F.epac([p3], [('SOBJ', '2036')]))


def hist(group, name):
    return {tuple(x['value']): x['meshes'] for x in group['values'][name]}


def main():
    tmp = tempfile.mkdtemp(prefix='qasset_census_st_')
    try:
        game = os.path.join(tmp, 'game_data')
        env = {'WWE_QASSET_GAME': game, 'WWE_QASSET_DB': os.path.join(tmp, 'asset.db'), 'WWE_QASSET_BUDGET': '120', 'WWE_QASSET_MODS': 'none'}
        write_census_fixture(game)

        def q(*a):
            return run(['params'] + list(a), env)

        rc, out = q('yCh_skin', '--json')
        j = jout(out)
        check(rc == 0 and j['matched'] == 7 and j['selected'] == 7, 'baseline: 7 yCh_skin meshes (5 + 1 + 1; the yProp_Fix twin is not counted)', out)
        # --- --by family
        rc, out = q('yCh_skin', '--by', 'family', '--values', 'g_A', '--json')
        j = jout(out)
        g = {x['group']: x for x in j['groups']}
        fam_c = [k for k in g if k.startswith('characters')]
        fam_e = [k for k in g if k.startswith('evt objects')]
        check(len(g) == 2 and fam_c and fam_e, '--by family: two groups, characters (chNNN) and evt objects', out)
        check(hist(g[fam_c[0]], 'g_A') == {(1.0,): 3, (2.0,): 1, (5.0,): 1, (6.0,): 1} and g[fam_c[0]]['selected'] == 6,
              'characters group: g_A 1 x3 (ch910 x2 + the ch911 twin), 2 x1, 5 x1, 6 x1; 6 meshes', out)
        check(hist(g[fam_e[0]], 'g_A') == {(3.0,): 1} and g[fam_e[0]]['selected'] == 1, 'FAMILY TWIN: the evt group holds only its own g_A 3 (the characters values are not folded in, yProp_Fix g_A 9 is another effect)', out)
        rc, out = q('yCh_skin', '--by', 'family', '--values', 'g_A')
        check('== family characters' in out and '== family evt objects' in out, 'the text form prints one block per family', out)
        # --- pseudo-names
        rc, out = q('yCh_skin', '--values', '@technique', '--json')
        j = jout(out)
        check(hist(j, '@technique') == {('0x11',): 5, ('0x22',): 1, ('0x33',): 1}, '@technique histogram over all yCh_skin: 0x11 x5, 0x22 x1, 0x33 x1', out)
        rc, out = q('yCh_skin', '--pac', 'ch910', '--values', '@technique', '--json')
        j = jout(out)
        check(hist(j, '@technique') == {('0x11',): 4, ('0x22',): 1} and j['selected'] == 5, '--pac ch910: 0x11 x4 and 0x22 x1 (PAC TWIN: ch911 and the 0x33 evt mesh do not leak in)', out)
        rc, out = q('yCh_skin', '--pac', 'nosuchpac', '--json')
        check(jout(out)['matched'] == 0, 'PLANTED MISS: --pac that matches no pac selects nothing', out)
        rc, out = q('yCh_skin', '--has', '@technique', '--values', '@verts', '--json')
        j = jout(out)
        check(j['selected'] == 7 and hist(j, '@verts') == {(3,): 7}, '--has @technique: every mesh has the column; @verts = 3 for all 7', out)
        rc, out = q('yCh_skin', '--lacks', '@technique', '--json')
        check(jout(out)['selected'] == 0, '--lacks @technique: none', out)
        rc, out = q('yCh_skin', '--has', 'g_A', '--values', '@cull', '--values', '@pal0', '--json')
        j = jout(out)
        check(sum(hist(j, '@cull').values()) == 7 and sum(hist(j, '@pal0').values()) == 7, '@cull / @pal0 give one value per selected mesh', out)
        rc, out = q('yCh_skin', '--has', 'g_B', '--by', 'family', '--json')
        j = jout(out)
        check(len(j['groups']) == 1 and j['groups'][0]['selected'] == 1 and j['groups_none_passed'] == 1, '--has g_B --by family: one group passes, the other is only counted', out)
        # --- --by technique (the effect twin must not count)
        rc, out = q('yCh_skin', '--by', 'technique', '--json')
        g = {x['group']: x for x in jout(out)['groups']}
        check(set(g) == {'0x11', '0x22', '0x33'} and g['0x11']['selected'] == 5 and g['0x22']['selected'] == 1 and g['0x33']['selected'] == 1,
              '--by technique: 0x11 x5, 0x22 x1, 0x33 x1 (TECHNIQUE TWIN: the same effect with another word is its own group; yProp_Fix 0x11 not counted)', out)
        rc, out = q('*', '--by', 'technique', '--json')
        g = {x['group']: x for x in jout(out)['groups']}
        check(g['0x11']['selected'] == 6, "'*' --by technique: yProp_Fix joins 0x11 (6)", out)
        # --- --by effect / pac / source
        rc, out = q('*', '--by', 'effect', '--json')
        g = {x['group']: x['selected'] for x in jout(out)['groups']}
        check(g == {'yCh_skin': 7, 'yProp_Fix': 1}, '--by effect: yCh_skin 7, yProp_Fix 1', out)
        rc, out = q('yCh_skin', '--by', 'pac', '--json')
        g = {x['group']: x['selected'] for x in jout(out)['groups']}
        check(g == {'pac/ch/ch910.pac': 5, 'pac/ch/ch911.pac': 1, 'pac/evt/evtobj.pac': 1}, '--by pac: ch910 5, ch911 1, evtobj 1', out)
        rc, out = q('yCh_skin', '--by', 'source', '--json')
        g = {x['group']: x['selected'] for x in jout(out)['groups']}
        check(g == {'game': 7}, '--by source: all 7 are game', out)
        # --- --sets
        rc, out = q('yCh_skin', '--sets', '--json')
        j = jout(out)
        sets = {' '.join(x['names']): x['meshes'] for x in j['sets']}
        base = 'g_A g_fDifAmbLevel sampDiffusePrm'
        check(sets == {base: 5, 'g_A sampDiffusePrm': 1, 'g_A g_B g_C g_fDifAmbLevel sampDiffusePrm': 1}, 'sets: base signature x5, D (no amb level) x1, E (g_B g_C) x1', out)
        rc, out = q('yCh_skin', '--by', 'family', '--sets', '--json')
        g = {x['group'].split(' ')[0]: x for x in jout(out)['groups']}
        check(sum(s['meshes'] for s in g['evt']['sets']) == 1 and len(g['evt']['sets']) == 1, 'sets per group: the evt group has its one signature', out)
        rc, out = q('yCh_skin', '--sets')
        check('name sets (3 distinct' in out and '5 meshes' in out, 'sets text form', out)
        # --- the cut line: total + the exact call that gives the rest
        rc, out = q('yCh_skin', '--by', 'pac', '--limit', '1')
        line = [l for l in out.splitlines() if 'showing 1 of' in l and 'names' in l]
        check(line and '--all' in line[0] and 'asset params yCh_skin --by pac --limit' not in line[0] and 'narrow' not in line[0], 'the cut line names the total and the call (--all)', out)
        call = line[0].split('all: ', 1)[1].split(', or')[0].strip()
        args = shlex.split(call, posix=False)[args_index(call):]
        rc, out2 = run(['params'] + [x.strip('"') for x in args], env)
        check('showing' not in out2, 'the printed call gives every list whole (no cut line in its answer): ' + call, out2)
        rc, out = q('yCh_skin', '--by', 'pac', '--limit', '1', '--json')
        check(len(jout(out)['groups']) == 3 and len(jout(out)['groups'][0]['names']) == 5, '--json is never cut', out)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('ALL PASS')
    return 0


def args_index(call):
    return shlex.split(call, posix=False).index('params') + 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Fail as e:
        print('FAIL  ' + str(e))
        sys.exit(1)
