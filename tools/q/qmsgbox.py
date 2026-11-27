r"""q msgbox [--json] [--follow-callers K]   (S111 QEVENT): the whole message-box catalogue in one call.

Every post site of the box (the indirect callers of sub_8291FCA0, slot +108: q callers' tables) is read with `q slice --struct` (tools/q/qevstruct.py):
the event struct on the poster's stack, initialised by sub_8291F5D8 (defaults: kind 0, id / title 99999, B 0, count 0), its fields
kind +0, text id +4, title +12, B +16, count +28 as the stores that reach the post; an argument of the poster is followed into its callers
(a wrapper gives one row per caller site), a struct pointer passed on is followed to the caller that builds it.
Rows with every id a constant go to the catalogue: id | text (the game's string table) | kind | B | post sites (`<` the caller that passed the id).
A row whose id stays unknown, or whose struct a callee filled, is counted and listed apart (never a guessed id).
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import qslice  # noqa: E402
import qevstruct  # noqa: E402

BOX = 0x8291FCA0
INIT = 0x8291F5D8
FIELDS = {0: 'kind', 4: 'id', 12: 'title', 16: 'B', 28: 'count'}
DEFAULTS = {0: 0, 4: 99999, 12: 99999, 16: 0, 28: 0}
SIZE = 56


class MsgBoxError(Exception):
    pass


def texts():
    import qgx
    pac = qgx._wm30_pac()
    out = {}
    for label, path in qgx.string_pacs():
        if not os.path.exists(path):
            continue
        data = pac.load(path)
        for nm, st, sz in qgx.epac_dir(data):
            if nm[2:] != '00':
                continue
            for p, leaf in pac.walk(qgx.epac_entry(data, st, sz)):
                for k, v in qgx.parse_text_table(leaf).items():
                    out.setdefault(k, v)
    return out


def post_sites(con, addr=BOX):
    import q as qmod
    d = qmod.indirect_of(con, addr)
    if 'error' in d:
        raise MsgBoxError('indirect callers not known: ' + d['error'])
    return [(int(s['func'][4:], 16), int(s['site'], 16)) for s in d['sites']]


def scan(src, sites, K=3, box=BOX):
    """-> rows: {'func','rel','site','via','kind':[..],'id':[..],'title','B','count','err'} one per post site and caller binding"""
    rows = []
    for faddr, site in sites:
        F = src.facts(faddr)
        base = {'func': src.name(faddr), 'rel': site - faddr, 'site': '0x%08X' % site}
        c = F and F['calls'].get(site - faddr)
        if not c:
            rows.append(dict(base, err='no call', via=[]))
            continue
        for r in qevstruct.struct_rows(src, faddr, site - faddr, 'r4', sorted(FIELDS), K, {INIT: DEFAULTS}, SIZE):
            if 'err' in r:
                rows.append(dict(base, err=r['err'], via=r['via']))
                continue
            o = dict(base, via=r['via'], base=r['base'])
            for off, nm in FIELDS.items():
                o[nm] = [x['v'] for x in r['fields'].get(off, [])]
            rows.append(o)
    return rows


LAYOUT_PACS = ('MnUIHD.pac', 'MnUISD.pac')
KIND_SEL = {0: 0, 1: 2, 2: 4}        # opcode 120's operand 0 -> the box kind


def layout_rows():
    """the posts that live in layout data: UIPR page command records of opcode 120 (handler sub_82B58080 through the table at 0x820664F0),
    operand 0 = kind selector, operand 1 = text id (tools/menus/uipr.py --commands 120). -> rows shaped like scan()'s"""
    sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'menus'))
    import uipr
    rows = []
    for pname in LAYOUT_PACS:
        path = os.path.join(uipr.pac.GAME, 'pac', 'menu', pname)
        if not os.path.exists(path):
            continue
        for leaf, dec in uipr.leaves_of_pac(path):
            try:
                lay = uipr.Layout(dec)
            except uipr.UiprError:
                continue
            for c in uipr.commands(lay, 120, 2):
                ops = c['operands']
                if len(ops) < 2:
                    continue
                rows.append({'func': 'UIPR %s %s' % (pname[:-4], leaf), 'rel': c['page'], 'site': 'page %d cmd %d' % (c['page'], c['cmd']),
                             'via': [], 'layout': True, 'kind': [KIND_SEL.get(ops[0][1], '?')], 'id': [ops[1][1]], 'title': [99999],
                             'B': [0], 'count': [0], 'page_name': c['name']})
    return rows


def constant(r):
    """a row is catalogued when its text id is constant (a kind / B that stays unknown prints as its text)"""
    return 'err' not in r and r['id'] and all(isinstance(v, int) for v in r['id'])


def catalogue(rows, tx):
    by = {}
    for r in rows:
        if not constant(r):
            continue
        site = '%s+%X' % (r['func'][4:], r['rel'])
        if r.get('layout'):
            site = '%s:%s' % (r['func'].replace(' ', ':'), r['site'].replace('page ', 'p').replace(' cmd ', 'c'))
        elif r['via']:
            site += '<' + r['via'][-1].split('+')[0].replace('sub_', '')
        for i in r['id']:
            if i in (99999, -1):
                continue
            e = by.setdefault(i, {'id': i, 'text': tx.get(i, '?'), 'kind': set(), 'B': set(), 'sites': []})
            e['kind'].update(qevstruct.leaf_text(x) if isinstance(x, tuple) else x for x in r['kind'])
            e['B'].update(qevstruct.leaf_text(x) if isinstance(x, tuple) else x for x in r['B'])
            if site not in e['sites']:
                e['sites'].append(site)
    return [by[i] for i in sorted(by)]


def clean(t):
    return t.replace('\n', ' ').replace('|', '/')[:60]


def main(argv, con):
    import argparse
    ap = argparse.ArgumentParser(prog='q msgbox', description=__doc__.split('\n')[0])
    ap.add_argument('--follow-callers', type=int, default=3, metavar='K')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--no-text', action='store_true', help='no string-table lookup (ids only)')
    ap.add_argument('--no-layout', action='store_true', help='code posts only (no UIPR layout command rows)')
    ap.add_argument('--no-cache', action='store_true')
    a = ap.parse_args(argv)
    src = qslice.QSource(con, use_cache=not a.no_cache)
    sites = post_sites(con)
    rows = scan(src, sites, a.follow_callers)
    n_code = len(rows)
    n_code_const = sum(1 for r in rows if constant(r))
    n_layout = 0
    if not a.no_layout:
        try:
            lr = layout_rows()
            rows += lr
            n_layout = len(lr)
        except Exception as ex:  # noqa: BLE001 - no game pacs on this machine: code rows only, said
            print(f'(no layout rows: {ex})', file=sys.stderr)
    try:
        tx = {} if a.no_text else texts()
    except Exception as ex:  # noqa: BLE001 - no game string tables on this machine: ids without text, said
        print(f'(no text: {ex})', file=sys.stderr)
        tx = {}
    cat = catalogue(rows, tx)
    n_const = sum(1 for r in rows if constant(r))
    summary = {'sites': len(sites), 'rows': n_code, 'constant_rows': n_code_const, 'layout_rows': n_layout, 'ids': len(cat),
               'unresolved_rows': len(rows) - n_const}
    if a.json:
        print(json.dumps({'summary': summary,
                          'catalogue': [dict(e, kind=sorted(e['kind'], key=str), B=sorted(e['B'], key=str)) for e in cat],
                          'unresolved': [dict(r, **{k: [qevstruct.leaf_text(x) if isinstance(x, tuple) else x for x in r[k]]
                                                    for k in ('kind', 'id', 'title', 'B', 'count') if k in r})
                                         for r in rows if not constant(r)]}, indent=1, ensure_ascii=False))
        return 0
    print('| id | text (first 60) | kind | B | post sites (sub_ dropped; `<` = the caller that passed the id) |')
    print('|---|---|---|---|---|')
    for e in cat:
        print('| %d | %s | %s | %s | %s |' % (e['id'], clean(e['text']), '/'.join(str(x) for x in sorted(e['kind'], key=str)),
                                              '/'.join(str(x) for x in sorted(e['B'], key=str)), ' '.join(e['sites'])))
    print()
    print('post sites %(sites)d, code rows %(rows)d (a wrapper gives one per caller), of them every id constant %(constant_rows)d, '
          'layout rows (UIPR opcode 120) %(layout_rows)d, distinct ids %(ids)d, unresolved rows %(unresolved_rows)d (q msgbox --json lists them)' % summary)
    return 0
