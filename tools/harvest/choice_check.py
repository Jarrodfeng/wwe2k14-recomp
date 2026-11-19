#!/usr/bin/env python3
r"""S103 lane J1: test the rules we derived against the harvested choice table. One line per rule: matches / total, and the misses listed. 0 game runs; reads the committed table + the player's
own files (the file side of each rule), writes nothing.

  py -I tools\harvest\choice_check.py [TABLE.json ...] [--rule NAME] [--arena N] [--plant] [--expect-fail] [--show N]
  (no TABLE: every table of tools/harvest/choice_table; a merged file from choice_merge.py works too)

Rules (each is "what the file side says" against "what the game's draws did", over every mesh / draw choice the table holds):
  p1_mask        P1  a yBG_ / yProp_ Default mesh draws with the highlight-MASK pair (*_alpha_spec / *_alpha_gloss) exactly when Material::alpha_mask says (variant_rule.final_rule: no
                     specular or normal map, BC2 / BC3 diffuse, < 2 % zero alpha texels, mean alpha >= 0.10).
  l5_highlight   L5  the highlight (*_specular* / mask pair) is drawn only for a mask mesh with a specular level > 0 (variant_rule.highlight_rule; the named exceptions of arena 4 stay named).
  l10_vertex_lm  L10 a yBG_ mesh draws a *_vertex_lm vertex program exactly when (mesh record word +0x90 & 9) == 1 (l10_rule.words).
  page_rule      L5 / L13b / L10  the stage light-map page is read ONCE, in the vertex stage (texture slot 16), by the vertex-LM class only; no yBG_Default colour draw binds a pixel page (slot 13; the
                     yBG_DefaultLM meshes bind their own light map there and are not part of the rule).
  lit_crowd      L4  every crowd figure draw (vertex program yAud*, not the far cards' T_Audience2D) is the LIT variant (name has `_lit_`, define FX_LIT): the crowd is never drawn unlit.
--plant flips each rule's prediction (the planted twin: every rule must then FAIL); --expect-fail exits 0 only when something fails (the twin's check).
Exit 0 when every rule listed has total > 0 and no miss."""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'arena'))
sys.path.insert(0, HERE)

TABLE_DIR = os.path.join(HERE, 'choice_table')
NAMED_EXCEPTIONS = {4: {(46, 1), (26, 0), (26, 1), (43, 1)}}   # variant_rule.py: ar_sak2 + the level-0 masks (P1's behaviour); [U] named, not hidden


def load(paths):
    if not paths:
        paths = sorted(os.path.join(TABLE_DIR, f) for f in os.listdir(TABLE_DIR) if f.endswith('.json'))
    return [json.load(open(p, encoding='utf-8')) for p in paths]


def mesh_rows(tables):
    """-> {(arena id, slot, mesh): {effect, tex, choices:[CHOICE...]}} over every table, scene and look."""
    out = {}
    for t in tables:
        for m in t.get('meshes', []):
            arena = int(m['file'][2:4])
            row = out.setdefault((arena, m['slot'], m['mesh']), dict(effect=m.get('effect'), tex=m.get('tex'), choices=[], file=m['file']))
            for v in (m.get('looks') or m.get('seen') or {}).values():
                row['choices'].extend(v['choices'])
    return out


def class_choices(tables):
    out = []
    for t in tables:
        for c in t.get('classes', []):
            for ch in c['choices']:
                out.append((c['class'], c['technique'], ch))
    return out


def klass(ps):
    import variant_rule as vr
    return vr.klass(ps)


def rule_p1_mask(tables, flip):
    import arena_files as af
    import variant_rule as vr
    ok, total, miss = 0, 0, []
    for (aid, slot, mesh), row in sorted(mesh_rows(tables).items()):
        ks = {klass(c['ps']) for c in row['choices'] if c['ps']}
        if not ks:
            continue
        f = vr.features(af.load_arena(aid), slot, mesh)
        pred = bool(vr.final_rule(f)) != flip
        total += 1
        if ('mask' in ks) == pred:
            ok += 1
        else:
            miss.append('bg%02d %d/%d %s (game %s, rule %s)' % (aid, slot, mesh, row['tex'], ','.join(sorted(ks)), pred))
    return ok, total, miss


def rule_l5_highlight(tables, flip):
    import arena_files as af
    import variant_rule as vr
    ok, total, miss = 0, 0, []
    for (aid, slot, mesh), row in sorted(mesh_rows(tables).items()):
        ks = {klass(c['ps']) for c in row['choices'] if c['ps']}
        if not ks:
            continue
        f = vr.features(af.load_arena(aid), slot, mesh)
        want = vr.highlight_rule(f)
        if want is None:
            continue
        if (slot, mesh) in NAMED_EXCEPTIONS.get(aid, set()) and not flip:
            continue
        total += 1
        if bool(ks & {'mask', 'specular'}) == (bool(want) != flip):
            ok += 1
        else:
            miss.append('bg%02d %d/%d %s (game %s, rule %s)' % (aid, slot, mesh, row['tex'], ','.join(sorted(ks)), bool(want) != flip))
    return ok, total, miss


def rule_l10_vertex_lm(tables, flip):
    import l10_rule as l10
    words = {}
    ok, total, miss = 0, 0, []
    for (aid, slot, mesh), row in sorted(mesh_rows(tables).items()):
        vss = {c['vs'] for c in row['choices'] if c['vs']}
        if not vss or not all(v.startswith('yBG_') for v in vss):
            continue
        if aid not in words:
            words[aid] = l10.words(aid)
        eff, word = words[aid][(slot, mesh)]
        if not eff.startswith('yBG_'):
            continue
        pred = ((word & 9) == 1) != flip
        got = any('vertex_lm' in v for v in vss)
        total += 1
        if got == pred:
            ok += 1
        else:
            miss.append('bg%02d %d/%d word %s (game vertex_lm %s, rule %s)' % (aid, slot, mesh, hex(word), got, pred))
    return ok, total, miss


def rule_page(tables, flip):
    """per draw choice of a yBG_ colour pass (a named pixel variant that is not a shadow / depth pass): vertex-LM program <=> vertex slot 16 bound; slot 13 never bound."""
    ok, total, miss = 0, 0, []
    seen = set()
    for (aid, slot, mesh), row in sorted(mesh_rows(tables).items()):
        if row['effect'] is not None and not row['effect'].startswith('yBG_'):
            continue
        for c in row['choices']:
            if not c['vs'].startswith('yBG_Default_') or not c['ps'] or '_shadow' in c['vs']:   # yBG_DefaultLM_ meshes bind their OWN light map at slot 13 (a file texture, not the stage page)
                continue
            key = (aid, slot, mesh, c['vs'], json.dumps(c['slots'], sort_keys=True))
            if key in seen:
                continue
            seen.add(key)
            vlm = 'vertex_lm' in c['vs']
            has16, has13 = '16' in c['slots'] or 16 in c.get('vtex', []), '13' in c['slots']
            pred_ok = (vlm == has16) and not has13
            if flip:
                pred_ok = not pred_ok
            total += 1
            if pred_ok:
                ok += 1
            else:
                miss.append('bg%02d %d/%d %s: slot16 %s slot13 %s' % (aid, slot, mesh, c['vs'], has16, has13))
    return ok, total, miss


def rule_lit_crowd(tables, flip):
    ok, total, miss = 0, 0, []
    seen = set()
    for kl, tech, c in class_choices(tables):
        if not c['vs'].startswith('yAud') or not c['ps'] or tech == 'T_Audience2D':   # the far cards (T_Audience2D) are baked sprites, lit by the bake, never by the card's own draw (L4 review)
            continue
        key = (kl, tech, c['vs'], c['ps'])
        if key in seen:
            continue
        seen.add(key)
        lit = '_lit_' in c['vs'] or 'FX_LIT' in c['defines']
        total += 1
        if lit != flip:
            ok += 1
        else:
            miss.append('%s/%s %s + %s' % (kl, tech or '-', c['vs'], c['ps']))
    return ok, total, miss


RULES = [('p1_mask', rule_p1_mask), ('l5_highlight', rule_l5_highlight), ('l10_vertex_lm', rule_l10_vertex_lm), ('page_rule', rule_page), ('lit_crowd', rule_lit_crowd)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('tables', nargs='*')
    ap.add_argument('--rule', default='')
    ap.add_argument('--arena', type=int, default=0)
    ap.add_argument('--plant', action='store_true')
    ap.add_argument('--expect-fail', action='store_true')
    ap.add_argument('--show', type=int, default=8)
    ap.add_argument('--expect', default='', help='a frozen result file: exit 1 when any rule matches / total / miss list differs from it (a new miss and a vanished one both)')
    ap.add_argument('--write-expect', default='', help='write the frozen result file')
    a = ap.parse_args()
    tables = load(a.tables)
    if a.arena:
        for t in tables:
            t['meshes'] = [m for m in t.get('meshes', []) if int(m['file'][2:4]) == a.arena]
    bad = False
    result = {}
    for name, fn in RULES:
        if a.rule and a.rule != name:
            continue
        ok, total, miss = fn(tables, a.plant)
        print('%-14s %4d / %-4d match  %d miss%s' % (name, ok, total, len(miss), ''.join('\n      MISS ' + m for m in miss[:a.show])))
        result[name] = dict(matches=ok, total=total, misses=sorted(miss))
        if miss or not total:
            bad = True
    if a.write_expect:
        with open(a.write_expect, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(result, f, indent=1, sort_keys=True)
            f.write('\n')
        print('wrote', a.write_expect)
    if a.expect:
        want = json.load(open(a.expect, encoding='utf-8'))
        diff = [k for k in sorted(set(want) | set(result)) if want.get(k) != result.get(k)]
        for k in diff:
            w, g = want.get(k, {}), result.get(k, {})
            print('EXPECT %s: frozen %s / %s, now %s / %s; new misses %s; gone %s' % (k, w.get('matches'), w.get('total'), g.get('matches'), g.get('total'), sorted(set(g.get('misses', [])) - set(w.get('misses', [])))[:5], sorted(set(w.get('misses', [])) - set(g.get('misses', [])))[:5]))
        print('expect: %s' % ('the result equals the frozen one' if not diff else '%d rule(s) differ' % len(diff)))
        return 1 if diff else 0
    if a.expect_fail:
        return 0 if bad else 1
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
