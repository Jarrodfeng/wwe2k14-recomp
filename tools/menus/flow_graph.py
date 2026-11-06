"""The SmartMenu list graph from a memory snapshot (Session 91 step 0, the game atlas).

  py tools/menus/flow_graph.py RUN NAME [--json OUT] [--text]

Reads the menu flow table (docs/systems/menus.md "The menu flow table": holder [0x839294DC], T = data + 4, 76-byte
records, T+84 / T+88 the index) from a `cvar wwe2k14_snapshot_now NAME` snapshot (logs/snap_RUN_NAME_D/H.bin) and
writes the list layer of the atlas: every menu id, its items in order, and where each item leads:
  - a submenu (record flags bit 4 clear; the submenu = the menu whose records name this record as parent, word +4),
  - a screen code (bit 4 set; word +44): 2000 = the match setup screen for match type word +52, 1001 = the MATCH RULES
    screen for match type word +52 (OPTIONS > MATCH RULES), other codes >= 1000 = screens / modes (screen controller).
Take the snapshot with the stock menus (`--wwe2k14_gfx_menu=false --wwe2k14_quit_item=false`): with our GRAPHICS /
QUIT TO DESKTOP patch on, T points at our own record array outside the snapshot ranges.

Word meanings beyond the above (+40 bits 6 / 7, +56 / +60) are unconfirmed: recorded as raw numbers for the readers.
Output is our own annotation (ids, positions, codes, text ids - no strings, no record bytes); local under out/ anyway.
Session 91 run 5889: 61 menus, 271 records.
"""
import argparse
import collections
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from flow_dump import Snap  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))


def s32(v):
    return v - (1 << 32) if v is not None and v & 0x80000000 else v


def read_graph(run, name):
    s = Snap(run, name)
    holder = s.u32(0x839294DC)
    data = s.u32(holder + 20) or s.u32(holder + 16)
    T = data + 4
    count, base = s.u32(T), s.u32(T + 4)
    if s.u32(base) is None:
        raise SystemExit('record array %#x is outside the snapshot (our menu patch on? use the stock-menu flags)' % base)
    recs = []
    for i in range(count):
        w = [s.u32(base + 76 * i + 4 * k) for k in range(19)]
        recs.append({'record': s32(w[0]), 'parent': s32(w[1]), 'menu': s32(w[2]), 'item': w[3] >> 16,
                     'w12_low': w[3] & 0xFFFF, 'label_id': w[6], 'flags': w[10], 'code': s32(w[11]),
                     'type': s32(w[13]), 'w56': w[14], 'w60': w[15], 'w16': s32(w[4])})
    byrec = {r['record']: r for r in recs}
    kids = collections.defaultdict(set)
    for r in recs:
        kids[r['parent']].add(r['menu'])
    menus = {}
    for r in sorted(recs, key=lambda r: (r['menu'], r['item'])):
        m = menus.setdefault(r['menu'], {'menu': r['menu'], 'opened_from': None, 'items': []})
        opener = byrec.get(r['parent'])
        if opener:
            m['opened_from'] = {'menu': opener['menu'], 'item': opener['item']}
        item = {'item': r['item'], 'record': r['record'], 'label_id': '0x%X' % r['label_id'], 'flags': '0x%X' % r['flags']}
        if r['flags'] & 0x10:
            item['code'] = r['code']
            if r['code'] in (2000, 1001):
                item['match_type'] = r['type']
            item['leads_to'] = {2000: 'setup %d' % (2000 + r['type']), 1001: 'rules %d' % r['type']}.get(
                r['code'], 'screen %d' % r['code'])
        else:
            sub = sorted(kids.get(r['record'], ()))
            item['submenu'] = sub[0] if len(sub) == 1 else sub
            item['leads_to'] = 'menu %s' % item['submenu']
            if r['code'] == 2000:  # menu 13 item 3 SPECIAL REFEREE: a list that also carries a setup type
                item['match_type'] = r['type']
        item['raw'] = {'w16': r['w16'], 'w56': r['w56'], 'w60': r['w60']}
        m['items'].append(item)
    return {'run': run, 'snapshot': name, 'records': count, 'menus': [menus[k] for k in sorted(menus)]}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('run')
    ap.add_argument('name')
    ap.add_argument('--json', help='output file (default out/harness/atlas/flow_RUN.json)')
    ap.add_argument('--text', action='store_true', help='print the graph')
    a = ap.parse_args()
    g = read_graph(a.run, a.name)
    out = a.json or os.path.join(ROOT, 'out', 'harness', 'atlas', 'flow_%s.json' % a.run)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'w', encoding='utf-8') as f:
        json.dump(g, f, indent=1)
    if a.text:
        for m in g['menus']:
            print('menu %d (from %s): %s' % (m['menu'], m['opened_from'],
                                             ', '.join('%d->%s' % (i['item'], i['leads_to']) for i in m['items'])))
    print('%d menus, %d records -> %s' % (len(g['menus']), g['records'], out))


if __name__ == '__main__':
    main()
