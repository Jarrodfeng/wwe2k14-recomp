r"""qasset_census (S111 QCENSUS): `q asset params` as a census of the asset index (replaces the ASSETCOV params in qasset_cov.py).

  q asset params EFFECT [--has N] [--lacks N] [--values N] [--like] [--source S] [--pac P] [--by G] [--sets] [--all]
      The materials (= meshes, one material row per mesh) of one effect (EFFECT exact, case-insensitive; --like = substring; `*` = every effect), counted by the parameter /
      sampler names they carry, with the value histogram of every --has / --values name.
        --by effect|family|pac|source|technique   every count and histogram once per group (family = qasset.family(); source = game | mod:NAME; technique = the technique word)
        --sets                                    a histogram of each mesh's FULL set of names (a signature per effect)
        --pac P                                   only the pac(s) P names (stem, path tail or substring; repeatable)
      Pseudo-names, usable in --has / --lacks / --values, read from the material columns: @technique and @cull (the words, 0x hex), @verts, @pal0 (the first bone-palette node
      index; -1 = none). Every other bone-palette entry is NOT in the index (needs a schema bump).
      Every list ends, when cut, with the total and the exact call that gives the rest (qcap; --all = no cut; --json is never cut).
"""
import json

import qasset_mods
import qcap

PSEUDO = {'@technique': 'technique_word', '@cull': 'cull_word', '@verts': 'verts', '@pal0': 'pal0'}
HEX = ('@technique', '@cull')
BY = ('effect', 'family', 'pac', 'source', 'technique')


def _effect_cond(effect, like):
    e = effect.lower()
    if effect == '*':
        return '1=1', []
    if like:
        return 'material.effect_lc LIKE ?', ['%' + e + '%']
    return 'material.effect_lc = ?', [e]


def _pseudo(row):
    """-> {'@technique': '0x..', ...} of the columns that are not NULL (the key is lower case, as a name match is)"""
    out = {}
    for k, col in PSEUDO.items():
        v = row[col] if col in row.keys() else None
        if v is not None:
            out[k] = ('0x%X' % (v & 0xFFFFFFFF),) if k in HEX else (v,)
    return out


class Acc:
    def __init__(self, want):
        self.matched = self.selected = 0
        self.eff = {}
        self.names = {}
        self.vals = {w: {} for w in want}
        self.sets = {}


def cmd_params(con, args, a, family, pac_rows):
    effect = args[0]
    by = a.by
    has = [h.lower() for h in a.has]
    lacks = [h.lower() for h in a.lacks]
    want = list(dict.fromkeys(list(a.has) + list(a.values)))
    ec, ea = _effect_cond(effect, a.like)
    sw, sa = qasset_mods.where(a.source)
    pw, pa = '1=1', []
    pacs_hit = []
    if a.pac:
        ids = []
        for p in a.pac:
            rows = pac_rows(con, p)
            pacs_hit.append(dict(pac=p, matches=len(rows)))
            ids += [r['id'] for r in rows]
        ids = sorted(set(ids))
        pw = 'pacs.id IN (%s)' % ','.join('?' * len(ids)) if ids else '0=1'
        pa = ids
    names_all = [x.lower() for x in list(a.has) + list(a.lacks) + list(a.values)]
    cols = ['material.technique_word AS technique_word'] if (by == 'technique' or '@technique' in names_all) else []
    for k, col in (('@cull', 'cull_word'), ('@verts', 'verts'), ('@pal0', 'pal0')):
        if k in names_all:
            cols.append('material.%s AS %s' % (col, col))
    gb = ''.join(', ' + c.split(' AS ')[0] for c in cols)
    sql = ('SELECT pacs.path AS path, material.effect AS effect, material.params AS params, material.samplers AS samplers%s, COUNT(*) AS n FROM material '
           'JOIN entry ON material.entry_id = entry.id JOIN pach ON entry.pach_id = pach.id JOIN pacs ON pach.pac_id = pacs.id WHERE %s AND %s AND %s '
           'GROUP BY pacs.path, material.effect_lc, material.params, material.samplers%s' % (''.join(', ' + c for c in cols), ec, sw, pw, gb))
    total = Acc(want)
    groups = {}
    fam_cache = {}
    for r in con.execute(sql, ea + sa + pa):
        try:
            params, samp = json.loads(r['params']), json.loads(r['samplers'])
        except ValueError:
            continue
        path = r['path']
        if by == 'effect':
            g = r['effect']
        elif by == 'family':
            g = fam_cache.get(path) or fam_cache.setdefault(path, family(path))
        elif by == 'pac':
            g = path
        elif by == 'source':
            g = qasset_mods.source(path)
        elif by == 'technique':
            g = '0x%X' % (r['technique_word'] & 0xFFFFFFFF) if r['technique_word'] is not None else '(no technique word)'
        else:
            g = None
        accs = [total] + ([groups.setdefault(g, Acc(want))] if by else [])
        n = r['n']
        have = {k.lower(): k for k in list(params) + list(samp)}
        ps = _pseudo(r)
        ok = all(h in have or h in ps for h in has) and not any(l in have or l in ps for l in lacks)
        sig = ' '.join(sorted(have.values(), key=str.lower))
        for acc in accs:
            acc.matched += n
            e = acc.eff.setdefault(r['effect'], [0, 0])
            e[0] += n
            if not ok:
                continue
            acc.selected += n
            e[1] += n
            for k in have.values():
                acc.names[k] = acc.names.get(k, 0) + n
            if a.sets:
                acc.sets[(r['effect'], sig)] = acc.sets.get((r['effect'], sig), 0) + n
            for w in want:
                k = have.get(w.lower())
                if k is not None:
                    v = tuple(params[k]) if k in params else (samp[k] or '(none)',)
                elif w.lower() in ps:
                    v = ps[w.lower()]
                else:
                    continue
                acc.vals[w][v] = acc.vals[w].get(v, 0) + n
    res = dict(effect=effect, like=a.like, source=a.source, has=a.has, lacks=a.lacks, pac=a.pac, pacs_matched=pacs_hit, by=by, sets_on=a.sets,
               call=call_text(a, effect), **_dump(total, a))
    if by:
        res['groups'] = [dict(group=g, **_dump(x, a)) for g, x in sorted(groups.items(), key=lambda kv: (-kv[1].selected, str(kv[0]))) if x.selected]
        res['groups_none_passed'] = sum(1 for x in groups.values() if not x.selected)
    return res


def _dump(x, a):
    d = dict(matched=x.matched, selected=x.selected,
             effects=[dict(effect=k, matched=v[0], selected=v[1]) for k, v in sorted(x.eff.items(), key=lambda kv: (-kv[1][1], kv[0])) if v[1]],
             names=[dict(name=k, meshes=v) for k, v in sorted(x.names.items(), key=lambda kv: (-kv[1], kv[0]))],
             values={w: [dict(value=list(v), meshes=n) for v, n in sorted(h.items(), key=lambda kv: (-kv[1], kv[0]))] for w, h in x.vals.items()})
    if a.sets:
        d['sets'] = [dict(effect=e, names=s.split(' ') if s else [], meshes=n) for (e, s), n in sorted(x.sets.items(), key=lambda kv: (-kv[1], kv[0]))]
    return d


def call_text(a, effect):
    """the exact call that prints every list whole (the cut line names it)"""
    s = 'py tools\\q\\q.py asset params %s' % ('"*"' if effect == '*' else effect)
    s += ' --like' if a.like else ''
    s += '' if a.source == 'all' else ' --source %s' % a.source
    s += ''.join(' --pac %s' % p for p in a.pac) + ''.join(' --has %s' % h for h in a.has) + ''.join(' --lacks %s' % h for h in a.lacks) + ''.join(' --values %s' % h for h in a.values)
    s += (' --by %s' % a.by if a.by else '') + (' --sets' if a.sets else '')
    return s + ' --all'


def _val(v):
    return ' '.join('%g' % x if isinstance(x, float) else str(x) for x in v)


def _block(d, a, r, pad, limit, names_limit):
    if d['selected']:
        print('%sparameter / sampler names carried by those %d meshes (%d distinct):' % (pad, d['selected'], len(d['names'])))
        shown, cut = qcap.cap(d['names'], names_limit, a.all)
        for x in shown:
            print('%s  %-26s %6d  %5.1f%%' % (pad, x['name'], x['meshes'], 100.0 * x['meshes'] / d['selected']))
        if cut:
            print('%s  %s' % (pad, qcap.trim_line(len(shown), len(d['names']), 'names', r['call'], narrow=False)))
    for w, h in d['values'].items():
        print('%svalues of %s: %d distinct%s' % (pad, w, len(h), '' if h else ' (no selected mesh has it)'))
        shown, cut = qcap.cap(h, limit, a.all)
        for x in shown:
            print('%s  %-30s %6d meshes' % (pad, _val(x['value']), x['meshes']))
        if cut:
            print('%s  %s' % (pad, qcap.trim_line(len(shown), len(h), 'values', r['call'], narrow=False)))
    if 'sets' in d:
        multi = len(d['effects']) > 1
        print('%sname sets (%d distinct, one signature per effect):' % (pad, len(d['sets'])))
        shown, cut = qcap.cap(d['sets'], limit, a.all)
        for x in shown:
            print('%s  %6d meshes  %s%s' % (pad, x['meshes'], (x['effect'] + ': ') if multi else '', ' '.join(x['names']) or '(none)'))
        if cut:
            print('%s  %s' % (pad, qcap.trim_line(len(shown), len(d['sets']), 'sets', r['call'], narrow=False)))


def print_params(r, a, header):
    flt = ''.join(' --has %s' % h for h in r['has']) + ''.join(' --lacks %s' % h for h in r['lacks'])
    print('asset params %s%s%s%s%s: %d of %d meshes%s  %s' % (r['effect'], ' (substring)' if r['like'] else '', ' [source %s]' % r['source'] if r['source'] != 'all' else '',
                                                           ''.join(' [pac %s: %d]' % (p['pac'], p['matches']) for p in r['pacs_matched']), ' [by %s]' % r['by'] if r['by'] else '',
                                                           r['selected'], r['matched'], (' pass' + flt) if flt else ' (no --has / --lacks)', header))
    if (len(r['effects']) > 1 or r['effect'] == '*') and r['by'] != 'effect':
        shown, cut = qcap.cap(r['effects'], a.limit, a.all)
        for x in shown:
            print('  effect %-22s %6d of %d meshes' % (x['effect'], x['selected'], x['matched']))
        if cut:
            print('  ' + qcap.trim_line(len(shown), len(r['effects']), 'effects', r['call'], narrow=False))
    if not r['by']:
        _block(r, a, r, '  ', a.limit, a.limit * 2)
        return
    shown, cut = qcap.cap(r['groups'], a.limit, a.all)
    for g in shown:
        print('  == %s %s: %d of %d meshes' % (r['by'], g['group'], g['selected'], g['matched']))
        if g['selected']:
            _block(g, a, r, '    ', a.limit, a.limit)
    if cut:
        print('  ' + qcap.trim_line(len(shown), len(r['groups']), 'groups', r['call'], narrow=False))
    if r['groups_none_passed']:
        print('  (%d more %s group(s) have meshes of this effect but none passes --has / --lacks)' % (r['groups_none_passed'], r['by']))
