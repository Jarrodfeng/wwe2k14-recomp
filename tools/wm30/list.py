"""Session 34: every 30 Years of WrestleMania match, read from the player's own game files at run time.

    py tools\\wm30\\list.py [--era N] [--json OUT] [--full] [--game DIR]

Reads (read-only; nothing from the game is written into the repo):
  pac\\evt\\evtdata.pac   (XCTD-compressed; tools\\wm30\\pac.py, out\\tools\\xctd_unpack.exe): the mode's tables, one PACH
                         of BPE-packed tables by kind (docs\\systems\\wm30.md): kind 4 = the 46-match index (24 fields per
                         match), kinds 15-19 = the five eras' match records (458 fields each)
  pac\\string.pac         the texts (text table with the mode's ids 18000-19999) and the wrestler names (60000 + id)
  db\\ids\\arenas.json, match_types.json (our id tables)

Prints one line per match (index, era, WrestleMania, date, venue, title, match type, arena, participants); --full adds
the stipulations, historical objectives and the story text id; --json OUT writes everything (for the launcher's 30
YEARS list; keep OUT under out\\: it holds the game's texts). The jump (--wwe2k14_wm30=INDEX, src\\wm30_jump.h) takes
the index printed first.
"""

import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tools', 'ids'))
import pac  # noqa: E402
from names_from_strings import tables  # noqa: E402

ERA_TEXT = 19980          # 19980 + era = the era's name
KIND_INDEX = 4            # the 46-match index
KIND_ERA0 = 15            # kinds 15-19 = eras 0-4


def table_rows(data):
    """A mode table: LE16 count, LE16 fields per row, then count rows of fields x LE16 (signed)."""
    count, fields = struct.unpack_from('<hh', data, 0)
    return [list(struct.unpack_from('<%dh' % fields, data, 4 + 2 * fields * i)) for i in range(count)]


def mode_tables(evtdata_path):
    """{kind: rows} from the PACH that holds the index (kind 4) and the era tables (kinds 15-19)."""
    data = pac.load(evtdata_path)
    found = {}
    for name, dec in pac.entries(data, depth=0):
        if '/' not in name:
            continue
        top, eid = name.rsplit('/', 1)
        if not eid.isdigit():
            continue
        found.setdefault(top, {})[int(eid)] = dec
    for top, kinds in found.items():
        if KIND_INDEX in kinds and all(KIND_ERA0 + e in kinds for e in range(5)):
            return {k: table_rows(pac.decode(v)) for k, v in kinds.items() if k == KIND_INDEX or k >= KIND_ERA0}
    raise SystemExit('no 30 Years of WrestleMania tables in %s' % evtdata_path)


def texts(string_path):
    """(mode texts {id: text}, names {character id: name}) from the first table that has each."""
    data = open(string_path, 'rb').read()
    mode, names = {}, {}
    for out, count in tables(data):
        ids = {}
        for i in range(count):
            off, ln, tid = struct.unpack_from('<III', out, 8 + 12 * i)
            if off + ln <= len(out):
                ids[tid] = out[off:off + ln].split(b'\0')[0].decode('utf-8', 'replace')
        if not mode and 18100 in ids and ERA_TEXT in ids:
            mode = {k: v for k, v in ids.items() if 16000 <= k < 22000}
        if not names and sum(1 for k in ids if 60100 <= k < 61000) >= 50:
            names = {k - 60000: v for k, v in ids.items() if 60100 <= k < 61000}
        if mode and names:
            break
    return mode, names


def id_names(fname):
    try:
        d = json.load(open(os.path.join(REPO, 'db', 'ids', fname), encoding='utf-8'))
    except OSError:
        return {}
    return {e['id']: e.get('name') for e in d.get('entries', []) if isinstance(e.get('id'), int)}


def catalogue(game):
    t = mode_tables(os.path.join(game, 'pac', 'evt', 'evtdata.pac'))
    mode, names = texts(os.path.join(game, 'pac', 'string.pac'))
    arenas, types = id_names('arenas.json'), id_names('match_types.json')
    records = {}
    for era in range(5):
        for r in t[KIND_ERA0 + era]:
            records[r[0]] = (era, r)

    def text(tid):
        return mode.get(tid, '') if tid >= 0 else ''

    out = []
    for x in sorted(t[KIND_INDEX], key=lambda r: r[0]):
        # Index fields (docs/systems/wm30.md): 0 index, 1 era, 2 moment code, 3 title number, 4 show number,
        # 5 title, 6 date, 7 venue, 8 show, 9 championship, 10 match type name, 11-15 stipulations,
        # 16-21 historical objectives, 22 story text, 23 record id (all texts are text ids, -1 = none).
        era, rec = records.get(x[23], (x[1], None))
        m = {
            'index': x[0], 'era': x[1], 'era_name': text(ERA_TEXT + x[1]), 'code': x[2], 'record': x[23],
            'title': text(x[5]), 'date': text(x[6]), 'venue': text(x[7]), 'show': text(x[8]),
            'championship': text(x[9]), 'match_type_name': text(x[10]),
            'stipulations': [text(v) for v in x[11:16] if v >= 0],
            'historical_objectives': [text(v) for v in x[16:22] if v >= 0],
            'story_text_id': x[22],
        }
        if rec:
            m['match_type'] = rec[2]
            m['match_type_db'] = types.get(rec[2])
            m['arena'] = rec[1]
            m['arena_name'] = arenas.get(rec[1])
            m['participants'] = [{'id': c, 'name': names.get(c, '?')} for c in rec[5:45] if c >= 0]
        out.append(m)
    return out


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game', default=os.path.join(REPO, 'game_data', 'extracted'), help='the game files folder')
    ap.add_argument('--era', type=int)
    ap.add_argument('--json')
    ap.add_argument('--full', action='store_true')
    a = ap.parse_args()
    cat = catalogue(a.game)
    if a.json:
        os.makedirs(os.path.dirname(os.path.abspath(a.json)), exist_ok=True)
        json.dump({'matches': cat}, open(a.json, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
        print('wrote %s (%d matches)' % (a.json, len(cat)))
        return 0
    era = None
    for m in cat:
        if a.era is not None and m['era'] != a.era:
            continue
        if m['era'] != era:
            era = m['era']
            print('\n== era %d: %s' % (era, m['era_name']))
        who = ', '.join(p['name'] for p in m.get('participants', []))
        mt = m['match_type_name'] or m.get('match_type_db') or ''
        print('%2d  %-50s %-15s %-14s %-17s type %3s %-28s arena %2s %-20s | %s' % (
            m['index'], m['title'][:50], m['show'], m['date'], m['venue'], m.get('match_type', '?'), mt[:28],
            m.get('arena', '?'), (m.get('arena_name') or '')[:20], who))
        if a.full:
            if m['championship']:
                print('      title: %s' % m['championship'])
            for s in m['stipulations']:
                print('      stipulation: %s' % s)
            for s in m['historical_objectives']:
                print('      historical: %s' % s)
            print('      record %d, moment code %d, story text %d' % (m['record'], m['code'], m['story_text_id']))
    print('\n%d matches' % len(cat))
    return 0


if __name__ == '__main__':
    sys.exit(main())
