"""The SmartMenu list graph from a memory snapshot (Session 91 step 0, the game atlas).

  py tools/menus/flow_graph.py RUN NAME [--json OUT] [--text]
  py tools/menus/flow_graph.py --records DUMP [--json OUT] [--text]   (S110 CASENTRY: a raw record dump, --wwe2k14_cas_flow_dump)
  add --names to either: each item's label text (S110 CASNAV)

Reads the menu flow table (docs/systems/menus.md "The menu flow table": holder [0x839294DC], T = data + 4, 76-byte
records, T+84 / T+88 the index) from a `cvar wwe2k14_snapshot_now NAME` snapshot (logs/snap_RUN_NAME_D/H.bin) and
writes the list layer of the atlas: every menu id, its items in order, and where each item leads:
  - a submenu (record flags bit 4 clear; the submenu = the menu whose records name this record as parent, word +4),
  - a screen code (bit 4 set; word +44): 2000 = the match setup screen for match type word +52, 1001 = the MATCH RULES
    screen for match type word +52 (OPTIONS > MATCH RULES), other codes >= 1000 = screens / modes (screen controller).
Take the snapshot with the stock menus (`--wwe2k14_gfx_menu=false --wwe2k14_quit_item=false`): with our GRAPHICS /
QUIT TO DESKTOP patch on, T points at our own record array outside the snapshot ranges.
--records reads the same 76-byte records from a raw big-endian dump instead (src/cas_launch.h writes holder slot 1, the
Create a Superstar editor's table, there; in that table codes 1001-1009 are the editor's page classes, docs/research/s110/MENUTREE.md 3).
--names (S110 CASNAV, docs/research/s110/CASNAV.md) resolves every label text id (+24) to its language-0 string from the player's own
string.pac (+ the newest DLC string pack) through tools/q/qgx.py, the same table the game's text manager [0x836F3A50] slot 6 reads.

Word meanings beyond the above (+40 bits 6 / 7, +56 / +60) are unconfirmed: recorded as raw numbers for the readers.
Output is our own annotation (ids, positions, codes, text ids - no strings, no record bytes) unless --names adds the label strings
(game text): local under out/ anyway, never committed.
Session 91 run 5889: 61 menus, 271 records.
"""
import argparse
import collections
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from flow_dump import Snap  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
RECORD = 76


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
    words = [[s.u32(base + RECORD * i + 4 * k) for k in range(19)] for i in range(count)]
    return dict({'run': run, 'snapshot': name}, **graph_from_words(words))


def read_records_file(path):
    """S110 CASENTRY: a raw dump of whole 76-byte flow records, big-endian as in guest memory (--wwe2k14_cas_flow_dump=PATH)."""
    with open(path, 'rb') as f:
        data = f.read()
    if not data or len(data) % RECORD:
        raise SystemExit('%s: %d bytes is not a whole number of %d-byte records' % (path, len(data), RECORD))
    words = [list(struct.unpack_from('>19I', data, RECORD * i)) for i in range(len(data) // RECORD)]
    return dict({'run': None, 'snapshot': os.path.basename(path)}, **graph_from_words(words))


def graph_from_words(words):
    recs = []
    for w in words:
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
    return {'records': len(recs), 'menus': [menus[k] for k in sorted(menus)]}


def text_names(ids, lang=0):
    """S110 CASNAV: {label text id: string} for `ids`, language `lang`, from the player's own string.pac and the newest DLC string
    pack (base texts first), through tools/q/qgx.py (docs/formats/string_pac.md). Game text: the caller keeps it under out/."""
    qdir = os.path.join(ROOT, 'tools', 'q')
    if qdir not in sys.path:
        sys.path.insert(0, qdir)
    import qgx
    pac = qgx._wm30_pac()
    want, out = set(ids), {}
    for _label, path in qgx.string_pacs():
        if not os.path.exists(path):
            continue
        data = pac.load(path)
        for nm, st, sz in qgx.epac_dir(data):
            if nm[2:] != '%02d' % lang:
                continue
            for _p, leaf in pac.walk(qgx.epac_entry(data, st, sz)):
                for k, v in qgx.parse_text_table(leaf).items():
                    if k in want:
                        out.setdefault(k, v)
    return out


def add_names(g):
    """Adds 'label' (the string, or None when no table holds the id) to every item; returns how many resolved."""
    items = [i for m in g['menus'] for i in m['items']]
    names = text_names(int(i['label_id'], 16) for i in items)
    for i in items:
        i['label'] = names.get(int(i['label_id'], 16))
    return sum(1 for i in items if i['label'] is not None)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('run', nargs='?')
    ap.add_argument('name', nargs='?')
    ap.add_argument('--records', help='a raw dump of 76-byte records instead of a snapshot (S110 CASENTRY: --wwe2k14_cas_flow_dump=PATH)')
    ap.add_argument('--json', help='output file (default out/harness/atlas/flow_RUN.json; with --records: flow_<dump file stem>.json)')
    ap.add_argument('--text', action='store_true', help='print the graph')
    ap.add_argument('--names', action='store_true', help='add each label\'s text from the player\'s own string.pac (S110 CASNAV)')
    a = ap.parse_args()
    if a.records:
        g = read_records_file(a.records)
        tag = os.path.splitext(os.path.basename(a.records))[0]
    elif a.run and a.name:
        g = read_graph(a.run, a.name)
        tag = a.run
    else:
        ap.error('give RUN NAME (a snapshot) or --records DUMP (a raw record dump)')
    named = add_names(g) if a.names else None
    out = a.json or os.path.join(ROOT, 'out', 'harness', 'atlas', 'flow_%s.json' % tag)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'w', encoding='utf-8') as f:
        json.dump(g, f, indent=1)
    if a.text:
        for m in g['menus']:
            print('menu %d (from %s): %s' % (m['menu'], m['opened_from'],
                                             ', '.join('%d%s->%s' % (i['item'], ' rec %d %r' % (i['record'], i.get('label')) if a.names else '',
                                                                      i['leads_to']) for i in m['items'])))
    if named is not None:
        print('names: %d of %d labels resolved' % (named, sum(len(m['items']) for m in g['menus'])))
    print('%d menus, %d records -> %s' % (len(g['menus']), g['records'], out))


if __name__ == '__main__':
    main()
