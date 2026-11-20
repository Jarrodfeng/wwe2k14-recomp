#!/usr/bin/env python3
r"""S103 WGD: the joiner's unit, driving tools\harvest\choice_diff.py the way main() does (join -> tabulate, which runs classify and the cycle-group check) on INVENTED
fixture rows: no game file is read, no rig runs. Names are our committed registry's effect / variant names only (src\renderer\shaders\fx\*.fx.json); slots, meshes,
arenas are made up.
Cases: an equal pair; a define only we have / only the game has; the fixed-effect mapping (a screen judged against yMovieDefault: equal, our highlight, another family);
fields not compared (ring states, an 'other' part, a None value on either side); a mesh one side lacks (both directions); the offline no-floor-map input; depth write;
cull in both directions and the game not drawing a mesh; a timed-cycle group drawing 1 / 0 / 2 members; a mesh the game hides in this look but we draw; a duplicate game
key; the out_alpha gate of the blend / alpha-test mappings (proven: equal; not proven: an [L] class); the headline counts (pairs compared / equal / not compared).
A table runs every skip path of classify and requires a not-compared entry of the right field (nothing is skipped silently).
Planted joiners (each must make at least one check FAIL, or the unit itself fails): classify that returns nothing for a mesh ours lacks; classify that drops every
not-compared entry (a not-compared field folded into equal); tabulate that counts every compared pair as equal.
  py -I tools\harvest\choice_diff_unit.py        exit 0 all pass, 1 a case failed (named)"""
# entry: join
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import choice_diff as cd  # noqa: E402

DEF = {   # our registry's defines of the variant names used below (src\renderer\shaders\fx\*.fx.json)
    'yBG_Default_base_vertex': [], 'yBG_Default_base_fragment': [], 'yBG_Default_specular_vertex': ['FX_SPECULAR'], 'yBG_Default_specular_fragment': ['FX_SPECULAR'],
    'yBG_Default_vertex_lm_vertex': ['FX_VERTEX_LM'], 'yBG_Default_shadow_vertex': ['FX_SHADOW'], 'yBG_Default_shadow_fragment': ['FX_SHADOW'],
    'yMovieDefault_base_vertex': [], 'yMovieDefault_base_fragment': [], 'yBG_UVScroll_base_vertex': [], 'yBG_UVScroll_base_fragment': [],
    'yProp_Default_base_vertex': [], 'yProp_Default_base_fragment': [],
}
B, BF = 'yBG_Default_base_vertex', 'yBG_Default_base_fragment'
ALPHA = 'on SRC_ALPHA/ONE_MINUS_SRC_ALPHA/ADD a:SRC_ALPHA/ONE_MINUS_SRC_ALPHA/ADD'
PROVEN = dict(vs_oa=1, vc=255, mdl_a=1.0, amb_w=1.0, dif_w=1.0, dal=0.0)   # our draw's alpha inputs that prove the output alpha 1


def eff(n):
    return 'yMovieDefault' if n.startswith('yMovieDefault') else n.rsplit('_', 2)[0]


def G(slot, mesh, vs=B, ps=BF, fam='stage', look='match', **kw):
    r = dict(arena=90, look=look, file='bg90.pac#%d' % slot, slot=slot, mesh=mesh, family=fam, effect='yBG_Default', vs=vs, ps=ps, vs_defines=DEF.get(vs, []), ps_defines=DEF.get(ps, []),
             vs_effect=eff(vs), ps_effect=eff(ps))
    if fam == 'stage':
        r.update(blend=ALPHA, alpha='GEQUAL 0.00392157', zwrite=1, faces='both', compare_states=True, assumed=['phase_word'], samplers={'0': dict(u=0, v=0, w=0, mag=1, min=1, mip=1)})
    r.update(kw)
    return r


def O(slot, mesh, vs=B, ps=BF, fam='stage', look='match', raw=None, **kw):
    r = dict(arena=90, look=look, file='bg90.pac#%d' % slot, slot=slot, mesh=mesh, family=fam, effect='yBG_Default', path='rule', vs=vs, ps=ps, vs_defines=DEF.get(vs, []),
             ps_defines=DEF.get(ps, []), vs_effect=eff(vs), ps_effect=eff(ps), blend='off', alpha='off', zwrite=1, cull=0, out_alpha=1, raw=dict(PROVEN, **(raw or {})),
             samplers={'0': [0, 0, 0, 1, 1, 1]})
    r.update(kw)
    return r


def fixture():
    game = [G(1, 0),                                                                         # equal in every field
            G(2, 0),                                                                         # ours adds FX_SPECULAR (ps)
            G(3, 0, 'yBG_Default_vertex_lm_vertex'),                                         # the game has FX_VERTEX_LM, ours not
            G(800, 0, cd.MOVIE_VS, cd.MOVIE_PS, fam='screen'),                               # mapped family, equal
            G(801, 0, cd.MOVIE_VS, cd.MOVIE_PS, fam='screen'),                               # mapped family, ours keeps the highlight
            G(802, 0, cd.MOVIE_VS, cd.MOVIE_PS, fam='screen'),                               # ours draws the screen with another family
            G(960, 0, 'yProp_Default_base_vertex', 'yProp_Default_base_fragment', fam='ring'),   # states not compared
            G(4, 0),                                                                         # ours lacks
            G(5, 0, 'yBG_Default_shadow_vertex', 'yBG_Default_shadow_fragment'),             # offline: no floor map
            G(6, 0),                                                                         # depth write differs
            G(8, 0, faces='one'),                                                            # cull: game one face, ours two-sided
            G(9, 0, faces='both'),                                                           # cull: game both, ours one face
            G(10, 0, faces='none'),                                                          # the game does not draw it, we do
            G(11, 0, look_hidden='g token'),                                                 # the game hides it in this look, we draw it
            G(12, 0), G(12, 0, ps='yBG_Default_specular_fragment'),                          # a duplicate game key (the first kept)
            G(13, 0, zwrite=None),                                                           # a None value -> not compared
            G(14, 0),                                                                        # blend / alpha: our alpha not proven 1 -> [L] classes
            G(20, 0, cycle_group=60010), G(21, 0, cycle_group=60010),                        # cycle group drawing 1 of 2 members: no class
            G(22, 0, cycle_group=60011), G(23, 0, cycle_group=60011),                        # draws 0 of 2: a class
            G(24, 0, cycle_group=60012), G(25, 0, cycle_group=60012),                        # draws 2 of 2: a class
            G(30, 0, faces='one', cull_mode=2, copy_mode=1),                                 # SCU: an `r` slot culled as the game: base CW, the copy CCW -> no class
            G(31, 0, faces='one', cull_mode=2, copy_mode=1),                                 # SCU: the copy keeps CW (the no-flip plant) -> a class
            G(32, 0, faces='one', cull_mode=2, copy_mode=1),                                 # SCU: the copy drawn two-sided -> a class
            G(33, 0, faces='one', cull_mode=2)]                                              # SCU: base drawn CCW -> a class
    ours = [O(1, 0), O(2, 0, ps='yBG_Default_specular_fragment'), O(3, 0),
            O(800, 0, fam='screen', path='plain'), O(801, 0, ps='yBG_Default_specular_fragment', fam='screen', path='plain'),
            O(802, 0, 'yBG_UVScroll_base_vertex', 'yBG_UVScroll_base_fragment', fam='screen', path='plain'),
            O(960, 0, 'yProp_Default_base_vertex', 'yProp_Default_base_fragment', fam='ring', path='plain'),
            O(5, 0, rule=dict(shadow=1, pass_rule=2, pass_used=0)),
            O(6, 0, zwrite=0), O(8, 0), O(9, 0, cull=2), O(10, 0), O(11, 0), O(12, 0), O(13, 0),
            O(14, 0, out_alpha=0),
            O(20, 0),
            O(24, 0), O(25, 0),
            O(30, 0, cull=2, ff=1, cull_mirror=2, ff_mirror=0), O(31, 0, cull=2, ff=1, cull_mirror=2, ff_mirror=1), O(32, 0, cull=2, ff=1, cull_mirror=0, ff_mirror=1), O(33, 0, cull=2, ff=0),
            dict(O(-1, 0, 'yProp_Default_base_vertex', 'yProp_Default_base_fragment', fam='other', path='plain'), file='bgetc.pac#-1'),   # an 'other' part
            O(7, 0)]                                                                         # ours only (no game row)
    return game, ours


def run(game, ours):
    joined, dup, gdup = cd.join(game, ours)
    classes, ncs, head = cd.tabulate(joined)
    return classes, ncs, head, gdup


def checks(classes, ncs, head, gdup):
    """-> list of (name, passed)"""
    names = set(classes)
    in_class = lambda f: {m[1] for c in classes.values() for m in c['meshes']} & {f}   # noqa: E731
    st = head['stage']
    return [
        ('equal pair -> no class for slot 1', not in_class('bg90.pac#1')),
        ('a define only we have -> "stage ps defines +FX_SPECULAR"', 'stage ps defines +FX_SPECULAR' in names),
        ('a define only the game has -> "stage vs defines -FX_VERTEX_LM"', 'stage vs defines -FX_VERTEX_LM' in names),
        ('fixed-effect mapping: screen yBG_Default base == yMovieDefault base', not in_class('bg90.pac#800')),
        ('fixed-effect mapping: our highlight on a screen -> "screen ps defines +FX_SPECULAR"', 'screen ps defines +FX_SPECULAR' in names),
        ('fixed-effect mapping: another family on a screen -> its own class', any(c.startswith('screen vs family: ours yBG_UVScroll') for c in names)),
        ('not compared: ring states listed', ncs.get('ring blend / alpha / depth write / cull: no game rule ported') == 1),
        ('not compared: other parts', ncs.get('other parts (props of other pacs): no game rule row in this version') == 1),
        ('ours lacks a mesh -> a class', classes.get('stage ours lacks the mesh', {}).get('count') == 1),
        ('ours only -> a class', classes.get('stage ours draws a mesh the game side has no row for', {}).get('count') == 1),
        ('offline no floor map -> not compared, no FX_SHADOW class', 'stage vs defines -FX_SHADOW' not in names and any('no floor map' in n for n in ncs)),
        ('depth write -> "stage depth write game 1 / ours 0"', 'stage depth write game 1 / ours 0' in names),
        ('cull: game one face / ours two-sided -> a class', any(c.startswith('stage cull: game one sub-pass') for c in names)),
        ('cull: game both faces / ours one face -> a class', any(c.startswith('stage cull: game both faces') for c in names)),
        ('cull: the game does not draw it / ours does -> a class', any(c.startswith('stage cull: the game does not draw') for c in names)),
        ('SCU: an `r` slot culled as the game (base CW, copy CCW) -> no class', not in_class('bg90.pac#30')),
        ('SCU: the `r` copy kept CW -> "stage cull front face: game CCW / ours CW (the `r` copy)"', classes.get('stage cull front face: game CCW / ours CW (the `r` copy)', {}).get('count') == 1),
        ('SCU: the `r` copy two-sided -> a class', any(c.startswith('stage cull: game culls the `r` copy') for c in names)),
        ('SCU: the base CCW -> "stage cull front face: game CW / ours CCW (base copy)"', classes.get('stage cull front face: game CW / ours CCW (base copy)', {}).get('count') == 1),
        ('a mesh the game hides in this look but we draw -> a class', any('game hides in this look' in c for c in names)),
        ('a duplicate game key is counted', gdup == 1),
        ('a None value -> a not-compared entry', any(n.startswith('stage depth write: the game side has no value') for n in ncs)),
        ('out_alpha gate: proven alpha -> no blend / alpha class on slot 1', not in_class('bg90.pac#1')),
        ('out_alpha gate: alpha not proven -> [L] blend class', any(c.startswith('stage blend [L]') for c in names)),
        ('out_alpha gate: alpha not proven -> [L] alpha-test class', any(c.startswith('stage alpha test [L]') for c in names)),
        ('cycle group: 1 of 2 drawn -> no class; 0 of 2 and 2 of 2 -> classes', sum(1 for c in names if 'timed-cycle group' in c) == 2 and
         classes.get('stage timed-cycle group [L] draws 0 of 2 members at the record\'s time (one expected)', {}).get('count') == 1),
        ('headline: pairs compared = equal + differ + not compared', st['pairs compared'] == st['pairs equal'] + st['pairs differ'] + st['pairs with a field not compared']),
        ('headline: the shadow-pass row is "not compared", never equal', st['pairs with a field not compared'] == 1),
        ('headline: the stage pairs that differ = slots 2 and 3', st['pairs differ'] == 2),
        ('headline: a row equal in every field exists (slot 1) and slot 13 (a None field) is not one', st['rows equal in every field'] >= 1 and
         st['rows equal in every field'] < st['pairs equal']),
    ]


def skip_table():
    """every skip path of classify appends a not-compared entry of its field"""
    S = lambda **kw: G(30, 0, **kw)   # noqa: E731
    cases = [
        ('game problem, ours lacks', S(problem='no prediction'), None, 'row'),
        ('look-hidden, ours lacks', S(look_hidden='e token'), None, 'row'),
        ('screen key outside the show set', G(803, 0, cd.MOVIE_VS, cd.MOVIE_PS, fam='screen'), None, 'row'),
        ('ring key not drawn', G(956, 0, fam='ring'), None, 'row'),
        ('cycle member not drawn', S(cycle_group=60010), None, 'row'),
        ('other part', None, dict(O(-1, 0, fam='other'), file='bgetc.pac#-1'), 'row'),
        ('game problem, ours draws', S(problem='no prediction'), O(30, 0), 'pair'),
        ('shadow pass, no floor map', S(vs='yBG_Default_shadow_vertex', ps='yBG_Default_shadow_fragment'), O(30, 0, rule=dict(shadow=1, pass_rule=2, pass_used=0)), 'pair'),
        ('blend None (game)', S(blend=None), O(30, 0), 'blend'),
        ('alpha None (ours)', S(), O(30, 0, alpha=None), 'alpha'),
        ('zwrite None (ours)', S(), O(30, 0, zwrite=None), 'zwrite'),
        ('faces None (game)', S(faces=None), O(30, 0), 'cull'),
        ('cull None (ours)', S(), O(30, 0, cull=None), 'cull'),
        ('states not ported', G(960, 0, fam='ring'), O(960, 0, fam='ring'), 'states'),
        ('no sampler table', S(samplers=None), O(30, 0), 'sampler'),
        ('sampler table without the slot', S(samplers={'0': None}), O(30, 0), 'sampler'),
        ('sampler: the pair differs', S(), O(30, 0, ps='yBG_Default_specular_fragment'), 'sampler'),
    ]
    out = []
    for name, g, o, field in cases:
        cls, nc = cd.classify(g, o)
        out.append(('skip path "%s" -> a not-compared %s entry' % (name, field), any(f == field for f, _n in nc)))
    return out


def report(results, prefix=''):
    bad = [n for n, ok in results if not ok]
    for n, ok in results:
        if prefix == '':
            print('%s  %s' % ('PASS' if ok else 'FAIL', n))
    return bad


def main():
    fails = []
    game, ours = fixture()
    fails += report(checks(*run(game, ours)))
    fails += report(skip_table())
    # planted joiners: each must make at least one check fail
    orig_classify, orig_tabulate = cd.classify, cd.tabulate

    def plant_lacks(g, o):
        return ([], []) if o is None else orig_classify(g, o)

    def plant_fold(g, o):
        cls, _nc = orig_classify(g, o)
        return cls, []

    def plant_all_equal(joined):
        classes, ncs, head = orig_tabulate(joined)
        for h in head.values():
            h['pairs equal'] += h['pairs with a field not compared'] + h['pairs differ']
            h['pairs with a field not compared'] = h['pairs differ'] = 0
        return classes, ncs, head
    for name, attr, fn in (('classify returns nothing for a mesh ours lacks', 'classify', plant_lacks), ('classify drops every not-compared entry', 'classify', plant_fold),
                           ('tabulate counts every compared pair as equal', 'tabulate', plant_all_equal)):
        setattr(cd, attr, fn)
        try:
            caught = report(checks(*run(game, ours)), 'plant') + (report(skip_table(), 'plant') if attr == 'classify' else [])
        finally:
            cd.classify, cd.tabulate = orig_classify, orig_tabulate
        ok = bool(caught)
        print('%s  planted joiner "%s" is caught (%d check(s) fail)' % ('PASS' if ok else 'FAIL', name, len(caught)))
        if not ok:
            fails.append('plant ' + name)
    print('choice_diff_unit: %d failed' % len(fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
