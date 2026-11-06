"""Session 91: build docs/systems/game_atlas.json from the read list graph, the crawls and the hand-kept screen table.

  py tools/atlas/build_atlas.py [--flow out/harness/atlas/flow_5889.json] [--crawls DIR ...] [--out docs/systems/game_atlas.json]

Inputs (all our own annotations; the atlas holds ids, memory chains, short labels the player sees, press counts and
the run that verified each edge - no game data):
  - the list graph (tools/menus/flow_graph.py): every list menu, its items in order and their targets;
  - crawler v2 results (tools/atlas/crawl2.py edges.json): per edge the verdict, the run, the back presses, labels;
  - tools/atlas/screens.json: the nodes outside the list graph (modes, their recognisers, cursor chains, how to leave),
    kept by hand from the readers / hunts / Jarrod's recordings, each with its evidence.
Node ids: `list:N` (SmartMenu list N; `list:1` = the main menu), `setup:T` (setup screen of match type T, menu id
2000 + T), `rules:T` (OPTIONS > MATCH RULES for type T, menu 1001 + T), `screen:C` (a screen code C inside the main
menu sequence), `mode:NAME` (another sequence: screens.json). Edges: {from, to, how: {item: i} | {press: B, times: n},
status: verified (run) | read | refused (run) | unconfirmed}.
"""
import argparse
import glob
import json
import os

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def clean(label):
    return label.replace('??? ', 'W ').replace('???', 'W').strip() if label else label


def node_for(it):
    if 'submenu' in it:
        return 'list:%s' % it['submenu']
    code = it['code']
    if code == 2000:
        return 'setup:%d' % it['match_type']
    if code == 1001:
        return 'rules:%d' % it['match_type']
    return 'screen:%d' % code


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--flow', default=os.path.join(REPO, 'out', 'harness', 'atlas', 'flow_5889.json'))
    ap.add_argument('--crawls', nargs='*', default=sorted(glob.glob(os.path.join(REPO, 'out', 'harness', 'atlas',
                                                                               'crawl', '*', 'edges.json'))))
    ap.add_argument('--screens', default=os.path.join(REPO, 'tools', 'atlas', 'screens.json'))
    ap.add_argument('--out', default=os.path.join(REPO, 'docs', 'systems', 'game_atlas.json'))
    a = ap.parse_args()
    with open(a.flow, encoding='utf-8') as f:
        flow = json.load(f)
    seen = {}  # (list, item) -> crawl edge (latest wins), with its run
    labels = {}
    for path in a.crawls:
        with open(path, encoding='utf-8') as f:
            c = json.load(f)
        for k, v in (c.get('labels') or {}).items():
            if v:
                labels[int(k)] = v
        for e in c['edges']:
            if e.get('verdict') in ('skipped', 'lost', None):
                continue
            e = dict(e, run=c.get('run'))
            seen[(e['list'], e['item'])] = e
    screens = {}
    if os.path.exists(a.screens):
        with open(a.screens, encoding='utf-8') as f:
            screens = json.load(f)
    nodes, edges = {}, []
    for m in flow['menus']:
        if m['menu'] in (0,):
            continue
        nid = 'list:%d' % m['menu']
        names = [clean(x) for x in (labels.get(m['menu']) or '').split(' | ') if x.strip()]
        nodes[nid] = {'kind': 'list', 'recognise': {'seq': 75, 'menu': m['menu']},
                      'cursor': '0x8392945C>352', 'items': len(m['items']),
                      'labels': names if len(names) <= len(m['items']) else names[:len(m['items'])],
                      'opened_from': m['opened_from']}
        for it in m['items']:
            to = node_for(it)
            e = {'from': nid, 'to': to, 'how': {'item': it['item']}, 'status': 'read'}
            c = seen.get((m['menu'], it['item']))
            if c:
                v = c['verdict']
                if v == 'ok':
                    e['status'] = 'verified'
                elif v == 'exit':
                    e['status'] = 'verified'
                    e['reaches_seq'] = c['after'].get('seq')
                    e['exit_action'] = c['after'].get('exit_action')
                else:
                    e['status'] = v
                e['run'] = c['run']
                if c.get('target') is not None:  # SM+368 with the cursor on this row (the navigator checks it)
                    e['target'] = c['target']
                back = c.get('back') or {}
                if back:
                    edges.append({'from': to, 'to': nid if back.get('cursor_kept') is not None else 'list:1',
                                  'how': {'press': 'B', 'times': back.get('presses')},
                                  'status': 'verified' if back.get('landed') != 'lost' else 'failed',
                                  'run': c['run'], 'cursor_kept': back.get('cursor_kept')})
            if it.get('flags') and int(it['flags'], 16) & 0x401:
                e['hidden_flag'] = True
            edges.append(e)
            if to.startswith(('setup:', 'rules:', 'screen:')) and to not in nodes:
                code = it['code'] + it.get('match_type', 0) if it['code'] in (2000, 1001) else it['code']
                nodes[to] = {'kind': to.split(':')[0], 'recognise': {'seq': 75, 'menu': code}}
                if to.startswith('setup:'):
                    nodes[to]['recognise']['screen_mode'] = 1
                if to.startswith('rules:'):
                    nodes[to]['recognise']['screen_mode'] = 6
    alias = screens.get('alias', {})
    for old, new in alias.items():
        nodes.pop(old, None)
    for e in edges:
        e['from'] = alias.get(e['from'], e['from'])
        e['to'] = alias.get(e['to'], e['to'])
    # a mode's back edge comes from screens.json (the crawl's B count for one entry item would be duplicated)
    edges = [e for e in edges if not (e['from'].startswith('mode:') and e['how'].get('press') == 'B')]
    for nid, n in screens.get('nodes', {}).items():
        nodes.setdefault(nid, {}).update(n)
        b = n.get('back') or {}
        if b.get('press'):
            edges.append({'from': nid, 'to': b['to'], 'how': {'press': b['press'], 'times': b['times']},
                          'status': 'verified', 'run': b.get('run')})
    edges += screens.get('edges', [])
    atlas = {'about': 'Session 91 game atlas (tools/atlas/build_atlas.py; docs/systems/game_atlas.md). Our own '
                      'annotations: ids, memory chains, short on-screen labels, press counts, verifying runs.',
             'states': {'seq': '0x839400F8>0>48', 'menu': '0x8392945C>364', 'cursor': '0x8392945C>352',
                        'target': '0x8392945C>368', 'screen_mode': '0x8392945C>11412>32',
                        'exit_action': '0x83929434'},
             'nodes': nodes, 'edges': edges}
    with open(a.out, 'w', encoding='utf-8') as f:
        json.dump(atlas, f, indent=1)
    st = {}
    for e in edges:
        st[e['status']] = st.get(e['status'], 0) + 1
    print('%d nodes, %d edges %s -> %s' % (len(nodes), len(edges), st, a.out))


if __name__ == '__main__':
    main()
