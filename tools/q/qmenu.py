"""qmenu: `q menu` = what a route lane needs to write a menu route with no exploring runs (S105 QM).

    py tools/q/q.py menu                      the screens and sequences, one line each
    py tools/q/q.py menu list [MENU|WORDS]    a list page: its rows in order, text id + label, what each row opens, the state words that say
                                              "this page is up" and "the cursor is on row K"
    py tools/q/q.py menu path WORDS|NODE      the input-script lines from the main menu to a screen (state-confirmed presses, the shape T5 proved)
    py tools/q/q.py menu caw [--labels]       the Create a Superstar editor: tabs, pages, rows (text id + label), what opens what, counts, waits
    py tools/q/q.py menu screen [NAME]        a non-list screen: finisher / title / streak / wm30 / universe / replay (state words + how in / out)
    py tools/q/q.py menu seq [ID|WORDS]       sequence ids -> the game's own sequence name + constructor (from the code index)
    py tools/q/q.py menu text WORDS|ID        a label -> its text id(s) and the editor code that loads each id (li / cmpwi sites)
    py tools/q/q.py menu build                (re)build out/harness/index/menu.json (also automatic when missing or older than its inputs)

Built from: the list layer of the game's menu flow table (docs/systems/game_atlas.json + tools/atlas/screens.json, read from memory by
tools/menus/flow_graph.py), the game's string tables (q text), the code index (sequence constructors, text-id immediates), and
tools/q/qmenu_lodge.py (the editor and the non-list screens: our annotations with marks). Game text lives only in menu.json under out/ (never committed).
Marks: [C] confirmed, [L] reasoning, [U] unknown.
"""
import json
import os
import re
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)

import qmenu_lodge as L  # noqa: E402

MAIN = r'C:\Users\Jarrod\wwe2k14-recomp'
ATLAS = os.path.join('docs', 'systems', 'game_atlas.json')
SCREENS = os.path.join('tools', 'atlas', 'screens.json')
STATES = os.path.join('tools', 'input_scripts', 'states.toml')
CAW_BANK = (6300, 6800)        # the editor's own text bank (string.pac 0500 leaf 3) [C: every row label of the R3 map is in it]
EDITOR_CODE = (0x83300000, 0x83440000)   # the editor's code region: text-id immediates here name the pages that load them [L]


class MenuError(Exception):
    pass


def _find(rel):
    for r in (REPO, MAIN):
        p = os.path.join(r, rel)
        if os.path.exists(p):
            return p
    raise MenuError('missing input ' + rel)


def _idx_dir():
    p = os.path.join(REPO, 'out', 'harness', 'index')
    return p if os.path.isdir(p) else os.path.join(MAIN, 'out', 'harness', 'index')


def _index_file(name):
    for r in (REPO, MAIN):
        p = os.path.join(r, 'out', 'harness', 'index', name)
        if os.path.exists(p) and os.path.getsize(p) > 0:
            return p
    raise MenuError('missing index file ' + name + ' (run build_index.py)')


def menu_json_path():
    return os.path.join(_idx_dir(), 'menu.json')


def _norm(s):
    return re.sub(r'[^A-Za-z0-9]+', ' ', ''.join(ch for ch in s if ord(ch) < 0x7F)).strip().upper()


# ----------------------------------------------------------------------------------------------------------------------- build
def _string_table():
    import qgx
    pac = qgx._wm30_pac()
    tab = {}
    for _label, path in qgx.string_pacs():
        if not os.path.exists(path):
            continue
        data = pac.load(path)
        for nm, st, sz in qgx.epac_dir(data):
            if nm[2:] != '00':
                continue
            for _p, leaf in pac.walk(qgx.epac_entry(data, st, sz)):
                for k, v in qgx.parse_text_table(leaf).items():
                    tab.setdefault(k, v)
    return tab


def _seqs(con):
    out = {}
    for src, site in con.execute('select src, site from edges where dst = 0x82C3A298'):
        s = con.execute('select s.text from strrefs r join strings s on s.addr = r.addr where r.func = ? and r.site between ? and ? '
                        'order by r.site desc limit 1', (src, site - 48, site)).fetchone()
        ids = [v for (v,) in con.execute("select value from imms where func = ? and site between ? and ? and op = 'li' order by site",
                                         (src, site - 48, site)) if 0 < v < 200]
        if s and ids:
            out[str(ids[-1])] = {'name': s[0], 'ctor': '0x%08X' % src}
    return out


def _sites(con):
    out = {}
    for f, s, v in con.execute("select func, site, value from imms where value between ? and ? and func between ? and ? "
                               "and op in ('li','cmpwi','cmplwi','addi') order by func, site",
                               (CAW_BANK[0], CAW_BANK[1], EDITOR_CODE[0], EDITOR_CODE[1])):
        lst = out.setdefault(str(v), [])
        if '0x%08X' % f not in lst:
            lst.append('0x%08X' % f)
    return out


def build(verbose=True):
    atlas = json.load(open(_find(ATLAS), encoding='utf-8'))
    scr = json.load(open(_find(SCREENS), encoding='utf-8'))
    tab = _string_table()
    import qpublish
    con = sqlite3.connect(qpublish.newest(_index_file('index.db')))      # S110 QREBUILD review: a copy beside a held index.db is the one to read
    by_norm = {}
    for k, v in tab.items():
        by_norm.setdefault(_norm(v), []).append(k)

    def ids_of(label, lo=0, hi=1 << 30):
        n = _norm(label)
        c = [n] + ([n[2:]] if n.startswith('W ') else [])
        res = []
        for x in c:
            res += [i for i in by_norm.get(x, []) if lo <= i <= hi]
        return sorted(set(res))

    lists = {}
    for nid, n in atlas['nodes'].items():
        if n['kind'] != 'list':
            continue
        m = int(nid.split(':')[1])
        rows = []
        for i, lab in enumerate(n.get('labels', [])):
            ids = ids_of(lab, 40960, 46000) or ids_of(lab)
            rows.append({'i': i, 'label': lab, 'ids': ids[:4]})
        lists[str(m)] = {'items': n['items'], 'rows': rows, 'opened_from': n.get('opened_from')}
    edges = {}
    for e in atlas['edges']:
        if 'item' in e['how']:
            edges.setdefault(e['from'], []).append({'item': e['how']['item'], 'to': e['to'], 'target': e.get('target'), 'seq': e.get('reaches_seq'),
                                                    'exit': e.get('exit_action'), 'status': e.get('status'), 'run': e.get('run')})
    nodes = {k: {kk: vv for kk, vv in v.items() if kk in ('kind', 'label', 'recognise', 'state_word', 'back', 'dead_end', 'evidence')}
             for k, v in list(atlas['nodes'].items()) + list(scr['nodes'].items()) if v.get('kind') != 'list'}
    caw_rows = {}
    for page, rows in L.CAW['pages'].items():
        caw_rows[page] = [{'label': r, 'ids': [] if r == '...' else ids_of(r, *CAW_BANK)[:4]} for r in rows]
    tabs = [{'label': t, 'ids': ids_of(t, *CAW_BANK)[:4]} for t, _ in L.CAW['tabs']]
    data = {'v': 1, 'text': {str(k): v for k, v in tab.items() if CAW_BANK[0] <= k <= CAW_BANK[1] or 40960 <= k <= 46000},
            'lists': lists, 'edges': edges, 'nodes': nodes, 'alias': scr.get('alias', {}), 'seqs': _seqs(con), 'sites': _sites(con),
            'caw_rows': caw_rows, 'caw_tabs': tabs, 'states': _states()}
    p = menu_json_path()
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p + '.tmp', 'w', encoding='utf-8') as f:
        json.dump(data, f)
    os.replace(p + '.tmp', p)
    if verbose:
        print('q menu: built %s (%d lists, %d sequences, %d text ids with editor sites)' % (p, len(lists), len(data['seqs']), len(data['sites'])))
    return data


def _states():
    out = {}
    cur = None
    for ln in open(_find(STATES), encoding='utf-8'):
        m = re.match(r'\[(\w+)\]', ln)
        if m:
            cur = m.group(1)
            out[cur] = {}
            continue
        m = re.match(r'(chain)\s*=\s*"([^"]+)"', ln)
        if m and cur:
            out[cur]['chain'] = m.group(2)
    return {k: v for k, v in out.items() if v.get('chain')}


def load():
    p = menu_json_path()
    stale = not os.path.exists(p)
    if not stale:
        t = os.path.getmtime(p)
        stale = any(os.path.getmtime(_find(r)) > t for r in (ATLAS, SCREENS)) or os.path.getmtime(os.path.join(HERE, 'qmenu_lodge.py')) > t
    return build(False) if stale else json.load(open(p, encoding='utf-8'))


# ----------------------------------------------------------------------------------------------------------------------- queries
def _label(d, ids):
    return '/'.join(str(i) for i in ids) if ids else '-'


def cmd_index(d):
    print('lists (menu id: rows, opened from):')
    for m in sorted(d['lists'], key=int):
        x = d['lists'][m]
        of = x['opened_from']
        print('  %-3s %d rows  %s  from %s' % (m, x['items'], ' | '.join(r['label'] for r in x['rows'][:4]) + (' ...' if x['items'] > 4 else ''),
                                           ('menu %s item %s' % (of['menu'], of['item'])) if of else '-'))
    print('screens / modes: ' + ', '.join(sorted(d['nodes'])))
    print('non-list screens (menu screen NAME): ' + ', '.join(sorted(L.SCREENS)) + ', caw (menu caw)')
    print('sequences: menu seq   |   labels: menu text WORDS   |   routes: menu path WORDS')
    return 0


def _node_of(d, words):
    """a list id, a node id, or words matched against list labels / node labels -> a node id."""
    w = words.strip()
    if re.fullmatch(r'\d+', w):
        return 'list:' + w
    if w in d['nodes'] or w.startswith('list:'):
        return w
    n = _norm(w)
    if w.lower() in L.SCREENS:
        return L.SCREENS[w.lower()].get('node')
    if w.lower() in ('caw', 'create a superstar'):
        return 'mode:caw'
    hits = []
    for k, v in d['nodes'].items():
        if n and (n in _norm(v.get('label', '')) or n == _norm(k)):
            hits.append(k)
    for m, x in d['lists'].items():      # a row's label -> where that row leads
        for r in x['rows']:
            if n and n in _norm(r['label']):
                for e in d['edges'].get('list:%s' % m, []):
                    if e['item'] == r['i']:
                        hits.append(d['alias'].get(e['to'], e['to']))
    return hits[0] if hits else None


def _item_target(d, m, row):
    for e in d['edges'].get('list:%s' % m, []):
        if e['item'] == row['i']:
            return e
    return None


def cmd_list(d, words):
    nid = _node_of(d, words) if words else 'list:1'
    if not nid or not nid.startswith('list:'):
        raise MenuError('no list page for %r (menu list shows ids)' % words)
    m = nid.split(':')[1]
    x = d['lists'][m]
    of = x['opened_from']
    print('menu %s: %d rows, opened from %s' % (m, x['items'], ('menu %s item %s' % (of['menu'], of['item'])) if of else 'nothing in the flow table (no opener)'))
    print('  up:     wait_state menu %s [menu = %s]   (and seq 75 [seq = %s])' % (m, d['states']['menu']['chain'], d['states']['seq']['chain']))
    print('  cursor: wait_state cursor K [%s]; press_until_state DOWN cursor >=K / UP cursor K' % d['states']['cursor']['chain'])
    for r in x['rows']:
        e = _item_target(d, m, r)
        if e:
            go = e['to'] + ((' (target %s' % e['target']) + (', exit %s' % e['exit'] if e['exit'] else '') + (', seq %s' % e['seq'] if e['seq'] else '') + ')'
                            if e['target'] else '')
            go += '  [%s%s]' % (e['status'] or '?', (' run ' + str(e['run'])) if e['run'] else '')
        else:
            go = '(no edge recorded)'
        print('  %2d  %-5s %-28s -> %s' % (r['i'], _label(d, r['ids']), r['label'], go))
    if x['items'] > len(x['rows']):
        print('  (%d rows carry no label in the flow table: types / generated rows; see menu path for the opener)' % (x['items'] - len(x['rows'])))
    return 0


def _bfs(d, goal):
    start = 'list:1'
    prev = {start: None}
    q = [start]
    while q:
        cur = q.pop(0)
        if cur == goal:
            break
        for e in d['edges'].get(cur, []):
            to = d['alias'].get(e['to'], e['to'])
            if to not in prev:
                prev[to] = (cur, e)
                q.append(to)
    if goal not in prev:
        return None
    path = []
    cur = goal
    while prev[cur]:
        cur, e = prev[cur]
        path.append((cur, e))
    return list(reversed(path))


def cmd_path(d, words):
    nid = _node_of(d, words)
    if not nid:
        raise MenuError('no screen matches %r (menu to see the lists and screens)' % words)
    path = _bfs(d, nid)
    if path is None:
        raise MenuError('no recorded route from the main menu to %s' % nid)
    print('# route to %s %s (from the main menu; every item edge below is in the atlas: [verified] = a crawl run walked it)' % (
        nid, d['nodes'].get(nid, {}).get('label', '')))
    print('include boot_to_menu')
    print('wait_state menu 1 1200 30')
    for frm, e in path:
        m = int(frm.split(':')[1])
        k = e['item']
        lab = next((r['label'] for r in d['lists'][str(m)]['rows'] if r['i'] == k), '?')
        if k > 0:
            print('press_until_state DOWN cursor >=%d 12 30 5 if_needed          # menu %d: item %d %s' % (k, m, k, lab))
            print('press_until_state UP cursor %d 12 30 5 if_needed' % k)
        else:
            print('press_until_state UP cursor 0 6 30 5 if_needed                # menu %d: item 0 %s' % (m, lab))
        to = d['alias'].get(e['to'], e['to'])
        if to.startswith('list:'):
            print('press_until_state A menu %s 8 120 5' % to.split(':')[1])
        elif e.get('seq'):
            print('press_until_state A seq %d 8 120 5                            # exit action %s' % (e['seq'], e.get('exit')))
        elif e.get('target') in (None, -1):
            print('press A                                                       # opens %s (the atlas holds no screen code; no menu word: wait on the next sequence)' % e['to'])
        else:
            print('press_until_state A menu %s 8 120 5                            # screen %s' % (e.get('target'), e.get('target')))
    for k, v in L.SCREENS.items():
        if v.get('node') == nid and (words.lower() == k or k == 'streak' and False):
            print('# then: ' + v['enter'])
            print('# up:   ' + v['up'])
    if words.lower() in ('caw', 'create a superstar'):
        print('# then: ' + L.CAW['enter'] + '  (menu caw for the pages)')
    nd = d['nodes'].get(nid, {})
    if nd.get('state_word'):
        print('# page-up word: %s (%s)' % (nd['state_word'], nd.get('recognise')))
    return 0


def cmd_caw(d, labels=False):
    C = L.CAW
    print('Create a Superstar editor: sequence %d (wait_state seq %d). Enter: %s' % (C['seq'], C['seq'], C['enter']))
    print('tabs (LB / RB): ' + ' | '.join('%d %s %s' % (i, t['label'], _label(d, t['ids'])) for i, t in enumerate(d['caw_tabs'])) + '   [C N2 dots 2 / 3; HEAD/BODY order L]')
    for page, rows in d['caw_rows'].items():
        print('page %s:' % page)
        for i, r in enumerate(rows):
            print('  %d %-6s %s' % (i, _label(d, r['ids']), r['label']))
    print('opens:')
    for k, v in C['opens'].items():
        print('  %s: %s' % (k, v))
    print('counts:')
    for k, (n, note) in C['counts'].items():
        print('  %s = %d %s' % (k, n, note))
    print('waits:')
    for w in C['waits']:
        print('  ' + w)
    print('open: ' + C['open'])
    if labels:
        print('editor bank (string table ids %d-%d):' % CAW_BANK)
        print(' | '.join('%s=%s' % (k, d['text'][k].strip()[:22]) for k in sorted(d['text'], key=int) if CAW_BANK[0] <= int(k) <= CAW_BANK[1]))
    return 0


def cmd_screen(d, name):
    if not name:
        for k, v in L.SCREENS.items():
            print('%-9s seq %-3s %s' % (k, v['seq'], v['title']))
        print('modes from the atlas:')
        for k, v in sorted(d['nodes'].items()):
            print('  %-16s %-36s recognise %s%s' % (k, v.get('label', ''), v.get('recognise'), ('  word ' + v['state_word']) if v.get('state_word') else ''))
        return 0
    if name.lower() in ('caw', 'create a superstar'):
        return cmd_caw(d)
    if name in L.SCREENS:
        v = L.SCREENS[name]
        print('%s: %s (sequence %s)' % (name, v['title'], v['seq']))
        for k in ('enter', 'up', 'back'):
            print('  %-5s %s' % (k, v[k]))
        return 0
    nid = _node_of(d, name)
    if nid and nid in d['nodes']:
        v = d['nodes'][nid]
        print('%s: %s\n  recognise %s%s\n  back %s\n  %s' % (nid, v.get('label', ''), v.get('recognise'), ('  state word ' + v['state_word']) if v.get('state_word') else '',
                                                              v.get('back'), v.get('evidence', '')))
        return 0
    raise MenuError('unknown screen %r' % name)


def cmd_seq(d, words):
    w = (words or '').strip()
    for k in sorted(d['seqs'], key=int):
        v = d['seqs'][k]
        if not w or w == k or _norm(w) in _norm(v['name']):
            print('%3s  %-40s ctor %s' % (k, v['name'], v['ctor']))
    return 0


def cmd_text(d, words):
    w = words.strip()
    hits = []
    for k, v in d['text'].items():
        if (re.fullmatch(r'\d+', w) and k == w) or (not re.fullmatch(r'\d+', w) and _norm(w) in _norm(v)):
            hits.append((int(k), v))
    hits.sort()
    for k, v in hits[:40]:
        s = d['sites'].get(str(k), [])
        print('%5d  %-34s %s' % (k, v.strip()[:34].replace('\n', '/'), ('editor code: ' + ' '.join(s[:6])) if s else ''))
    if not hits:
        print('no label matches %r in the editor bank or the list labels' % w)
        return 1
    return 0


def main(argv):
    a = list(argv)
    try:
        if a[:1] == ['build']:
            build(True)
            return 0
        d = load()
        c = a[0] if a else ''
        rest = ' '.join(x for x in a[1:] if not x.startswith('--'))
        if c == '':
            return cmd_index(d)
        if c == 'list':
            return cmd_list(d, rest)
        if c == 'path':
            return cmd_path(d, rest)
        if c == 'caw':
            return cmd_caw(d, '--labels' in a)
        if c == 'screen':
            return cmd_screen(d, rest)
        if c == 'seq':
            return cmd_seq(d, rest)
        if c == 'text':
            return cmd_text(d, rest)
        raise MenuError('menu: unknown command %r (list / path / caw / screen / seq / text / build)' % c)
    except MenuError as e:
        print('q menu: %s' % e, file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
