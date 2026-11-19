r"""S103 P1 (review rework): the arena-4 table "mesh -> the game's pixel-shader variant" (tools\arena\variant_table.py JSON files, merged) against candidate DATA rules (0 game runs).

  py -I tools\arena\variant_rule.py VT1.json [VT2.json ...] [--arena 4] [--rule NAME] [--list]

Classes of a game variant (from the ps variant name the stream draw carries):
  alpha_spec    *_specular_alpha_spec_fragment   (diffuse alpha = highlight mask, not output alpha)
  alpha_gloss   *_spec_alpha_gloss_fragment      (the yProp spelling of the same thing)
  specular      *_specular_fragment / *_spec_fragment (plain highlight, no alpha mask)
  other         base / lightmap / shadow / ... (no highlight)
Per mesh the file side is read (effect, spec level, sampSpecularPrm, normal map, texture format and alpha statistics) and each rule predicts "mask" (alpha_spec / alpha_gloss) or "not".
Prints every rule's agreement with the stream over the meshes the stream shows, and every mismatch of the chosen rule. Numbers only."""
import argparse
import collections
import io
import json
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import arena_files as af  # noqa: E402


def klass(ps):
    if 'alpha_spec' in ps or 'alpha_gloss' in ps:
        return 'mask'
    if 'specular' in ps or '_spec_' in ps:
        return 'specular'
    return 'other'


_tex = {}


def tex_stats(arena, name):
    if name in _tex:
        return _tex[name]
    t = arena.textures.get(name.lower()) if name else None
    if not t:
        _tex[name] = None
        return None
    im = Image.open(io.BytesIO(t.blob))
    im.load()
    al = np.asarray(im.convert('RGBA'))[..., 3]
    st = {'fourcc': t.fourcc.decode('latin1'), 'le32': float((al <= 32).mean()), 'zero': float((al == 0).mean()), 'mean': float(al.mean() / 255.0), 'min': int(al.min())}
    _tex[name] = st
    return st


def features(arena, slot, mesh):
    m = arena.slots[slot][mesh]
    lev = (m.params.get('g_fSpecularLev') or (0.0,))[0]
    st = tex_stats(arena, m.samplers.get('sampDiffusePrm'))
    return {'effect': m.effect, 'lev': lev, 'specmap': 'sampSpecularPrm' in m.samplers, 'normal': any('Normal' in k for k in m.samplers),
            'lm': 'sampLightMapPrm' in m.samplers, 'tex': m.samplers.get('sampDiffusePrm'), 'st': st, 'params': sorted(m.params)}


def AlphaCutsOut(st):
    return st is not None and st['fourcc'] in ('DXT5', 'DXT3') and st['le32'] >= 0.08


def final_rule(f):
    """The adopted rule (jboy.h, Material::alpha_mask): yBG_Default / yProp_Default, no specular or normal map, BC2 / BC3 diffuse, < 2 % exactly-zero alpha texels, mean alpha >= 0.10."""
    return (f['effect'] in ('yBG_Default', 'yProp_Default') and not f['specmap'] and not f['normal'] and f['st'] is not None and f['st']['fourcc'] in ('DXT5', 'DXT3')
            and f['st']['zero'] < 0.02 and f['st']['mean'] >= 0.10)


RULES = {
    'final': final_rule,
    'old_cutout': lambda f: False,   # the loader before P1: no mesh takes the mask pair when its texture cuts out
    'dxt5': lambda f: f['st'] is not None and f['st']['fourcc'] == 'DXT5',
    'spec_nomap': lambda f: f['lev'] > 0 and not f['specmap'],
    'spec_nomap_nonormal': lambda f: f['lev'] > 0 and not f['specmap'] and not f['normal'],
    'spec_nomap_alpha_present': lambda f: f['lev'] > 0 and not f['specmap'] and f['st'] is not None and f['st']['fourcc'] in ('DXT5', 'DXT3'),
    'texel_p1': lambda f: f['lev'] > 0 and not f['specmap'] and not f['normal'] and f['st'] is not None and f['st']['fourcc'] in ('DXT5', 'DXT3') and f['st']['zero'] < 0.02,
    'texel_p1_nocut': lambda f: f['lev'] > 0 and not f['specmap'] and not f['normal'] and f['st'] is not None and f['st']['fourcc'] in ('DXT5', 'DXT3') and f['st']['zero'] < 0.02 and f['st']['le32'] >= 0.08,
    'texel_nolev': lambda f: not f['specmap'] and not f['normal'] and f['st'] is not None and f['st']['fourcc'] in ('DXT5', 'DXT3') and f['st']['zero'] < 0.02,
    'texel_nolev_z01': lambda f: not f['specmap'] and not f['normal'] and f['st'] is not None and f['st']['fourcc'] in ('DXT5', 'DXT3') and f['st']['zero'] < 0.005,
    'texel_nolev_z05': lambda f: not f['specmap'] and not f['normal'] and f['st'] is not None and f['st']['fourcc'] in ('DXT5', 'DXT3') and f['st']['zero'] < 0.05,
    'texel_eff': lambda f: f['effect'] in ('yBG_Default', 'yProp_Default') and not f['specmap'] and not f['normal'] and f['st'] is not None and f['st']['fourcc'] in ('DXT5', 'DXT3') and f['st']['zero'] < 0.02,
    'texel_eff_nospecmap_free': lambda f: f['effect'] in ('yBG_Default', 'yProp_Default') and f['st'] is not None and f['st']['fourcc'] in ('DXT5', 'DXT3') and f['st']['zero'] < 0.02,
    'spec_any_alpha': lambda f: f['lev'] > 0 and not f['specmap'] and not f['normal'] and f['st'] is not None and f['st']['fourcc'] in ('DXT5', 'DXT3') and f['st']['le32'] < 0.08 or
    (f['lev'] > 0 and not f['specmap'] and not f['normal'] and f['st'] is not None and f['st']['fourcc'] in ('DXT5', 'DXT3') and f['st']['zero'] < 0.02),
}


# ---- S103 L5: which arena meshes the game draws WITH a highlight (the *_specular* pixel variants) ----------------------------------------------------------------------------------------
# The game draws every other yBG_Default / yProp_Default arena mesh with the plain `*_base` / `*_lightmap` variant (no highlight lobe) even when its g_fSpecularLev is 0.5 (the stairs,
# tier walls, exit frame: msg_stair / msg_wall0 / msg_exit4, lev 0.5, DXT1). Highlight = a mask mesh (final_rule) only. ONE known exception [U]: slot 46 mesh 1 (ar_sak2, DXT5, 50 % zero texels, lev 0.1) is drawn with the plain `Default_specular`; the other cut-outs with a level (ar_fence2 0.1, msg_fence1 / msg_2d_kyaku 0.5) are drawn plain, no file field tells them apart.
def highlight_rule(f):
    if f['effect'] not in ('yBG_Default', 'yProp_Default'):
        return None
    return bool(final_rule(f) and f['lev'] > 0)   # what ships: Material::alpha_mask AND g_fSpecularLev > 0 (MaterialWants::spec); a level-0 mask draws base on ours (a zero lobe) where the game draws alpha_spec


def highlight_old(f):
    """The behaviour before L5 (the plant): any material with a level > 0 and no specular map takes a highlight pair."""
    if f['effect'] not in ('yBG_Default', 'yProp_Default'):
        return None
    return bool(f['lev'] > 0 and not f['specmap'])


HIGHLIGHT = {'highlight': highlight_rule, 'highlight_old': highlight_old}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('tables', nargs='+')
    ap.add_argument('--arena', type=int, default=4)
    ap.add_argument('--rule', default='')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--merge-out', default='', help='write the merged table (slot, mesh, mask, variants) as JSON')
    ap.add_argument('--check', default='', metavar='RULE', help='exit 1 unless RULE matches the table on 100 %% of its meshes')
    ap.add_argument('--expect-fail', default='', help='with --check: a planted rule; exit 0 only when it does NOT match 100 %%')
    a = ap.parse_args()
    arena = af.load_arena(a.arena)
    seen = collections.defaultdict(lambda: collections.Counter())
    for p in a.tables:
        for r in json.load(open(p)):
            for ps, n in (r['ps'].items() if 'ps' in r else [(v, 1) for v in r['variants']]):   # a variant_table.py row, or a merged row (tools\arena\arena4_variants.json)
                seen[(r['slot'], r['mesh'])][ps] += n
    rows = []
    for (slot, mesh), c in sorted(seen.items()):
        ks = {klass(ps) for ps in c}
        f = features(arena, slot, mesh)
        rows.append((slot, mesh, f, sorted(c), ks))
    if a.merge_out:
        json.dump([{'slot': r[0], 'mesh': r[1], 'mask': 'mask' in r[4], 'class': sorted(r[4]), 'variants': [p.split('_', 1)[1] for p in r[3]]} for r in rows], open(a.merge_out, 'w'), separators=(',', ':'))
    if a.check in HIGHLIGHT:
        use = [r for r in rows if HIGHLIGHT[a.check](r[2]) is not None]
        known = {(46, 1), (26, 0), (26, 1), (43, 1)} if a.check == 'highlight' else set()   # [U] ar_sak2 (above) + the level-0 masks ar_dm 26/0-1 and ar_fence 43/1 (P1's behaviour: base on ours, alpha_spec in the game, a zero lobe); the plant keeps none
        bad = [r for r in use if bool(r[4] & {'mask', 'specular'}) != HIGHLIGHT[a.check](r[2]) and (r[0], r[1]) not in known]
        print('check %s: %d of %d yBG / yProp_Default meshes match (%d wrong, %d named exceptions)%s' % (a.check, len(use) - len(bad), len(use), len(bad), len(known), ''.join(chr(10) + '   WRONG %d/%d %s lev %.2f game %s' % (r[0], r[1], r[2]['tex'], r[2]['lev'], ','.join(sorted(r[4]))) for r in bad[:8])))
        if a.expect_fail:
            sys.exit(0 if bad else 1)
        sys.exit(1 if bad or not use else 0)
    if a.check:
        bad = [r for r in rows if ('mask' in r[4]) != RULES[a.check](r[2])]
        print('check %s: %d of %d meshes match (%d wrong)%s' % (a.check, len(rows) - len(bad), len(rows), len(bad), ''.join(chr(10) + '   WRONG %d/%d %s' % (r[0], r[1], r[2]['tex']) for r in bad[:8])))
        if a.expect_fail:
            sys.exit(0 if bad else 1)
        sys.exit(1 if bad or not rows else 0)
    if a.list:
        for slot, mesh, f, ps, ks in rows:
            st = f['st'] or {}
            print('%3d/%d %-16s %-12s lev %.2f specmap %d normal %d lm %d %s le32 %.2f zero %.2f mean %.2f -> %s %s' % (slot, mesh, f['tex'], f['effect'], f['lev'], f['specmap'], f['normal'], f['lm'], st.get('fourcc', '-'),
                  st.get('le32', -1), st.get('zero', -1), st.get('mean', -1), ','.join(sorted(ks)), ','.join(p.split('_', 1)[1] for p in ps)))
    print('meshes the stream shows: %d (mixed variants for one mesh: %d)' % (len(rows), sum(len(r[4]) > 1 for r in rows)))
    for name, fn in RULES.items():
        if a.rule and name != a.rule:
            continue
        ok = bad = 0
        wrong = []
        for slot, mesh, f, ps, ks in rows:
            want = 'mask' in ks
            got = fn(f)
            if want == got:
                ok += 1
            else:
                bad += 1
                wrong.append('%d/%d %s (%s, game %s, rule %s)' % (slot, mesh, f['tex'], f['effect'], ','.join(sorted(ks)), got))
        print('rule %-26s match %d / %d = %.1f %%' % (name, ok, ok + bad, 100.0 * ok / max(ok + bad, 1)))
        if a.rule:
            for w in wrong:
                print('   WRONG ' + w)


if __name__ == '__main__':
    main()
