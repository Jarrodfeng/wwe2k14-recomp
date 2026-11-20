"""qmenu_selftest: `q menu` re-finds known answers from the index, and refuses planted twins (S105 QM). 0 game runs.

    py -I tools/q/qmenu_selftest.py

Known answers (NOT copied from the lodge's own tables where an independent source exists):
  T5 Finisher path   : main menu item 3 -> menu 3 item 4 -> menu 15 item 0 -> menu 28 item 0 -> sequence 26   (BFS over the flow-table graph)
  T5 Streak          : sequence 36 is CStreakMeunSequence and the 30 Years sequence is 31 (the code index's own constructor strings)
  R3 CAW editor map  : sequence 76 'Create a Superstar Ex...'; every row label of the HEAD / CLOTHING / UPPER BODY pages is in the game's editor
                       text bank; the labels' text ids are loaded by editor code (the code sites); HAIR carousel 1/86 is lodged [C R3]
Planted twins (each must be refused): an item index moved in the edge list, a CAW row label renamed, sequence 26's name altered,
a CAW row id outside the editor bank, a missing page.
"""
import copy
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qmenu  # noqa: E402
import qmenu_lodge as L  # noqa: E402


def check(d, lodge=L):
    """-> list of failure strings (empty = every known answer re-found)."""
    bad = []
    # --- T5 finisher path
    path = qmenu._bfs(d, 'mode:finisher')
    got = [(f, e['item']) for f, e in path] if path else None
    want = [('list:1', 3), ('list:3', 4), ('list:15', 0), ('list:28', 0)]
    if got != want:
        bad.append('T5 finisher path: got %s want %s' % (got, want))
    elif path[-1][1].get('seq') != 26:
        bad.append('T5 finisher: last edge does not reach sequence 26')
    # --- T5 streak / 30 Years sequences (from the code index)
    sq = d['seqs']
    if 'Streak' not in sq.get('36', {}).get('name', ''):
        bad.append('T5 streak: sequence 36 name %r' % sq.get('36'))
    if 'WM30' not in sq.get('35', {}).get('name', '') or 'RTW' not in sq.get('31', {}).get('name', ''):
        bad.append('T5 wm30: sequences 31 / 35 %r %r' % (sq.get('31'), sq.get('35')))
    if 'Caf' not in sq.get('26', {}).get('name', ''):
        bad.append('T5 finisher: sequence 26 name %r' % sq.get('26'))
    p30 = qmenu._bfs(d, 'mode:wm30')
    if not p30 or p30[-1][1].get('seq') != 31:
        bad.append('T5 wm30: path does not end at sequence 31')
    # --- R3 CAW editor
    if 'Superstar' not in sq.get('76', {}).get('name', ''):
        bad.append('R3 caw: sequence 76 name %r' % sq.get('76'))
    for page in ('HEAD', 'CLOTHING', 'UPPER BODY'):
        rows = d['caw_rows'].get(page)
        if not rows:
            bad.append('R3 caw: page %s missing' % page)
            continue
        for r in rows:
            if not r['ids']:
                bad.append('R3 caw: %s / %s has no text id in the editor bank' % (page, r['label']))
            elif not all(qmenu.CAW_BANK[0] <= i <= qmenu.CAW_BANK[1] for i in r['ids']):
                bad.append('R3 caw: %s / %s id outside the editor bank %s' % (page, r['label'], r['ids']))
    if [r['label'] for r in d['caw_rows'].get('HEAD', [])][:4] != ['FACES', 'TEMPLATES', 'SKIN', 'HAIR']:
        bad.append('R3 caw: HEAD rows start %s' % [r['label'] for r in d['caw_rows'].get('HEAD', [])][:4])
    with_sites = sum(1 for p in ('HEAD', 'CLOTHING', 'UPPER BODY') for r in d['caw_rows'].get(p, []) if any(str(i) in d['sites'] for i in r['ids']))
    total = sum(len(d['caw_rows'].get(p, [])) for p in ('HEAD', 'CLOTHING', 'UPPER BODY'))
    if with_sites * 10 < total * 6:
        bad.append('R3 caw: only %d of %d editor rows have a code site (editor code region scan)' % (with_sites, total))
    if lodge.CAW['counts'].get('HEAD/HAIR carousel', (0,))[0] != 86:
        bad.append('R3 caw: HAIR carousel count is not 86')
    return bad


def main():
    d = qmenu.load()
    fails = []
    real = check(d)
    print('real data: %s' % ('PASS' if not real else 'FAIL ' + '; '.join(real)))
    if real:
        fails.append('real')
    twins = []
    t = copy.deepcopy(d)                       # 1: an item index moved
    for e in t['edges']['list:3']:
        if e['to'] == 'list:15':
            e['item'] = 5
    twins.append(('edge item moved', t))
    t = copy.deepcopy(d)                       # 2: a CAW row renamed (its id no longer resolves)
    t['caw_rows']['HEAD'][3] = {'label': 'HAIRZ', 'ids': []}
    twins.append(('caw row renamed', t))
    t = copy.deepcopy(d)                       # 3: sequence 26 name altered
    t['seqs']['26']['name'] = 'CSomethingElse'
    twins.append(('sequence 26 renamed', t))
    t = copy.deepcopy(d)                       # 4: an id outside the editor bank
    t['caw_rows']['CLOTHING'][1]['ids'] = [41000]
    twins.append(('caw id outside bank', t))
    t = copy.deepcopy(d)                       # 5: a page missing
    del t['caw_rows']['UPPER BODY']
    twins.append(('caw page missing', t))
    t = copy.deepcopy(d)                       # 6: the code sites gone (rows no longer tied to editor code)
    t['sites'] = {}
    twins.append(('code sites gone', t))
    for name, td in twins:
        r = check(td)
        print('twin %-22s %s' % (name, 'refused (' + r[0][:60] + ')' if r else 'ACCEPTED (selftest hole)'))
        if not r:
            fails.append(name)
    print('qmenu_selftest: %s' % ('PASS' if not fails else 'FAIL ' + ', '.join(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
