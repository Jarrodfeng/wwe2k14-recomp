r"""q uipr: a menu layout's pages with their record words, the root page, its items, and which branch sub_82B44548 takes per page (S111 QFIX5).

    py tools/q/q.py uipr LAYOUT [LEAF] [--pages] [--root] [--items] [--elements] [--commands OPCODE] [--json]

LAYOUT = a pac of the game's menu folder (`MnUIHD`, `MnUIHD.pac`, `menu/MnUISD.pac`) with the leaf in the next word or after a colon (`MnUIHD p2/0`,
`MnUIHD:p2/0`); without a leaf every key-0 UIPR leaf of the pac is shown. A dumped leaf file (`py -I tools\wm30\pac.py ... --dump`) also works as LAYOUT.
Reads the player's pac through tools\menus\uipr.py (the decoder; docs\formats\uipr.md); nothing is written.

  default / --pages  every page: index, name, root flag (rec+16), kind, parent, items, and the record words rec+144 / +148 / +152 / +160, then the
                     root pages' branch.
  --root             only the root pages (rec+16 == 1) with the branch and their items (item name -> target page).
  --items            the items under every page shown (uipr.py --items).
  --elements         the nodes' elements (uipr.py --elements).    --commands OPCODE   the page command records of that opcode (uipr.py --commands).

The branch (sub_82B44548 +AC..+E8, rule [C] in docs\research\s111\M100_W3A.md Q5): a ROOT page whose record word +152 (page +716) is non-zero makes the layout
replay the SAVED RETURN (no fixed page; the saved-return words +328..+340); with +152 == 0 it takes the FIXED link: layout rec+144 (an index of
uipr.LAYOUT_ORDER) and page rec+148, with rec+160 the flag. A page that is not a root page has no branch (the rule runs when a root page is left).
Re-find: MnUIHD p2/0 (MatchCreator): root page 4, rec+152 = 1 -> saved return.
"""
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, 'tools', 'menus'))
import uipr  # noqa: E402


class QuiprError(Exception):
    pass


def branch_of(p):
    """-> {'kind': 'saved_return' | 'fixed_link' | None, ...}: which side of sub_82B44548's page +716 test a page takes"""
    if p['root'] != 1:
        return {'kind': None, 'why': 'not a root page (rec+16 != 1)'}
    if p['link_152'] != 0:
        return {'kind': 'saved_return', 'rec152': p['link_152']}
    leaf = uipr.LAYOUT_ORDER.get(p['link_layout'])
    return {'kind': 'fixed_link', 'layout': p['link_layout'], 'leaf': leaf, 'page': p['link_page'], 'flag160': p['link_flag']}


def branch_text(b):
    if b['kind'] is None:
        return b['why']
    if b['kind'] == 'saved_return':
        return 'SAVED RETURN (rec+152 = %d, non-zero: no fixed page)' % b['rec152']
    return 'FIXED LINK -> layout %d (%s) page %d, flag rec+160 = %d (rec+152 = 0)' % (b['layout'], b['leaf'] or '?', b['page'], b['flag160'])


def page_rec(lay, p, items):
    d = {'index': p['index'], 'name': p['name'], 'id': p['id'], 'node': p['node'], 'root': p['root'], 'kind': p['kind'],
         'parent_page': p['parent_page'], 'parent_item': p['parent_item'], 'item_count': p['item_count'], 'class': p['class'],
         'rec144': p['link_layout'], 'rec148': p['link_page'], 'rec152': p['link_152'], 'rec160': p['link_flag'], 'branch': branch_of(p)}
    if items:
        d['items'] = []
        for it in p['items']:
            tp = lay.pages[it['target_page']] if 0 <= it['target_page'] < len(lay.pages) else None
            d['items'].append({'index': it['index'], 'name': it['name'], 'target_page': it['target_page'],
                               'target_name': tp['name'] if tp else None, 'hidden': it['hidden']})
    return d


def resolve(words):
    """-> (leaves [(name, bytes)], pac label)"""
    if not words:
        raise QuiprError('give LAYOUT (a pac name such as MnUIHD) and optionally the leaf (p2/0)')
    w0 = words[0]
    leaf = words[1] if len(words) > 1 else None
    if os.path.isfile(w0) and not w0.lower().endswith('.pac'):
        with open(w0, 'rb') as f:
            return [(w0, f.read())], w0
    if ':' in w0 and not os.path.exists(w0):
        w0, _, l2 = w0.rpartition(':')
        leaf = leaf or l2
    if len(words) > 2:
        raise QuiprError('too many words: LAYOUT [LEAF]')
    path = w0
    if not os.path.exists(path):
        name = w0.replace('\\', '/')
        if '/' not in name:
            name = 'menu/' + name
        if not name.lower().endswith('.pac'):
            name += '.pac'
        path = os.path.join(uipr.pac.GAME, 'pac', *name.split('/'))
    if not os.path.exists(path):
        raise QuiprError(f'{w0}: no such pac (looked at {path})')
    try:
        every = list(uipr.leaves_of_pac(path))
    except Exception as e:  # noqa: BLE001 - a pac the unpacker rejects
        raise QuiprError(f'{path}: {e}')
    if leaf:
        every_names = [n for n, _ in every]
        if leaf not in every_names:
            raise QuiprError(f'{leaf}: not a UIPR layout leaf of {os.path.basename(path)} (has: {", ".join(every_names)})')
        return [(n, d) for n, d in every if n == leaf], os.path.basename(path)
    return every, os.path.basename(path)


def main(argv):
    ap = argparse.ArgumentParser(prog='q uipr', description=__doc__.split('\n')[0])
    ap.add_argument('layout', nargs='*', help='PAC [LEAF]')
    ap.add_argument('--pages', action='store_true')
    ap.add_argument('--root', action='store_true')
    ap.add_argument('--items', action='store_true')
    ap.add_argument('--elements', action='store_true')
    ap.add_argument('--commands', type=int, metavar='OPCODE')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    leaves, label = resolve(a.layout)
    outs = []
    for name, leaf in leaves:
        try:
            lay = uipr.Layout(leaf)
        except (uipr.UiprError, struct.error, IndexError) as e:
            raise QuiprError(f'{name}: {e}')
        o = {'leaf': name, 'version': lay.version, 'nodes': lay.node_count, 'pages': lay.page_count}
        if a.commands is not None:
            o['commands'] = uipr.commands(lay, a.commands)
        elif a.elements:
            ns = argparse.Namespace(node=None, items=False, elements=True, labels=False)
            o['node_list'] = uipr.summary(lay, name, ns)['node_list']
        else:
            pgs = [p for p in lay.pages if p['root'] == 1] if a.root else lay.pages
            o['page_list'] = [page_rec(lay, p, a.items or a.root) for p in pgs]
        outs.append(o)
    if a.json:
        print(json.dumps(outs, indent=1))
        return 0
    for o in outs:
        print(f"== {label} {o['leaf']}  version {o['version']}, {o['nodes']} nodes, {o['pages']} pages")
        for c in o.get('commands', []):
            print(f"  page {c['page']} {c['name']} cmd {c['cmd']} opcode {c['opcode']} operands {c['operands']}")
        for n in o.get('node_list', []):
            print(f"  node {n['index']:3d}  {len(n['elements'])} elements")
            for e in n['elements']:
                print(f"      element {e['index']:3d}  kind {e['kind']:2d}  {e['name']}  {e['class']}")
        for p in o.get('page_list', []):
            print(f"  page {p['index']:3d}  id {p['id']:3d}  node {p['node']:3d}  root {p['root']} kind {p['kind']}  parent {p['parent_page']}/{p['parent_item']}  "
                  f"items {p['item_count']:3d}  rec+144 {p['rec144']} +148 {p['rec148']} +152 {p['rec152']} +160 {p['rec160']}  {p['name']}")
            if p['root'] == 1:
                print('      branch: ' + branch_text(p['branch']))
            for it in p.get('items', []):
                print(f"      item {it['index']:3d}  {it['name'] or '-':<30s} -> page {it['target_page']} {it['target_name'] or ''}" + ('  (hidden)' if it['hidden'] else ''))
    return 0
