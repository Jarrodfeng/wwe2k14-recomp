"""S101 lane R1: the 30 Years in-match scene table, read from the player's own files at run time (nothing is written into the repo).

    py -I tools\\wm30\\scene_table.py [--game DIR] [--json OUT] [--md]

For each of the 46 matches (tools\\wm30\\list.py's index) joins its record's Lua chunk (tools\\wm30\\lua51_list.py) with the Season scene bank
names (pac\\evt\\Season0N.pac, EVP directory: bank id -> EV name): which banks the chunk queues with CallFunction(bank, flag, ..., 1003), at which
Lua step, and the conditions that enter that step. The objectives printed are the index texts (kind 4 fields 16-21). The condition names
are lua51_list.IDS (docs\\systems\\wm30.md "Objective scenes (lane R1)": [C] handler read, [L] meaning). --json writes everything under out\\
(it holds the game's texts: never commit it).
"""

import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), 'tools', 'ids'))
import list as wmlist  # noqa: E402  (tools\wm30\list.py)
import lua51_list as lua  # noqa: E402
import pac  # noqa: E402


def season_names(game, era):
    """{bank id: EV name} of pac\\evt\\Season0<era>.pac: EVP directory rows from +0x80C {name[8] decimal, u32 sector, u32 size/256}; the bank id
    (BE u16 at +0x20) and name (+0x22) are inside each BPE-packed EV block (docs\\formats\\season_pac.md)."""
    d = pac.load(os.path.join(game, 'pac', 'evt', 'Season0%d.pac' % era))
    if d[0x800:0x804] != b'EVP ':
        raise SystemExit('Season0%d.pac: no EVP directory' % era)
    pos, out = 0x80C, {}
    while pos + 16 <= len(d):
        nm = d[pos:pos + 8].rstrip(b'\0 ')
        if not nm.isdigit():
            break
        start, size = struct.unpack_from('<II', d, pos + 8)
        raw = d[0x4000 + start * 0x800:0x4000 + start * 0x800 + size * 256]
        dec = pac.bpe_decode(raw) if raw[:4] == b'BPE ' else raw
        out[int(nm)] = dec[0x22:0x32].split(b'\0')[0].decode('latin-1').strip()
        pos += 16
    return out


def build(game):
    cat = wmlist.catalogue(game)
    by_rec = {m['record']: m for m in cat}
    banks = {}
    for e in range(5):
        banks.update(season_names(game, e))
    rows = []
    seen = set()
    for era in range(5):
        for rec, blob in sorted(lua.era_chunks(era).items()):
            if len(blob) <= 394:
                continue
            f = lua.parse(blob)
            scenes, pre = lua.scene_rows(f)
            m = by_rec.get(rec)
            seen.add(rec)
            rows.append(dict(record=rec, era=era, index=m['index'] if m else None,
                             names=[p['name'] for p in m.get('participants', [])][:2] if m else [],
                             objectives=m['historical_objectives'] if m else [],
                             preloads=sorted(set(int(x) for x in pre if x.lstrip('-').isdigit())),
                             scenes=[dict(bank=int(s['bank']) if s['bank'].isdigit() else s['bank'], name=banks.get(int(s['bank']), '?') if s['bank'].isdigit() else '?',
                                          flag=s['flag'], a3=s['a3'], a4=s['a4'], a5=s['a5'], step=s['step'], extra=[g for g in s['guards'] if not g.startswith('step ==')],
                                          entry=s['entry']) for s in scenes]))
    missing = [m['record'] for m in cat if m['record'] not in seen]
    return rows, missing


EXPECT = dict(chunks=59, matches=46, matches_with_play=26, plays=50, flag1=3, banks_named=True)


def counts(rows, missing):
    idx = [r for r in rows if r['index'] is not None]
    plays = [s for r in rows for s in r['scenes']]
    return dict(chunks=len(rows), matches=len(idx), matches_with_play=sum(1 for r in idx if r['scenes']), plays=len(plays),
                flag1=sum(1 for s in plays if s['flag'] == '1'), banks_named=all(s['name'] not in ('?', '') for s in plays) and not missing)


def mismatches(got, expect):
    return {k: (got[k], v) for k, v in expect.items() if got[k] != v}


def check(rows, missing, expect=EXPECT):
    got = counts(rows, missing)
    bad = mismatches(got, expect)
    if bad:
        print('CHECK FAIL (got, expected): %s' % bad)
        return 1
    # planted fault: a wrong expectation must be caught by the same comparison
    if not mismatches(got, dict(expect, plays=expect['plays'] + 1)):
        print('CHECK FAIL: the planted wrong count was not caught')
        return 1
    print('scene_table check PASS: %s; planted wrong count caught' % got)
    return 0


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game', default=pac.GAME)
    ap.add_argument('--json')
    ap.add_argument('--check', action='store_true', help='gate check: the pinned counts hold, and a planted wrong count must fail')
    a = ap.parse_args()
    rows, missing = build(a.game)
    if a.check:
        return check(rows, missing)
    for r in rows:
        head = 'match %s' % r['index'] if r['index'] is not None else 'UNINDEXED (no match row)'
        print('%s  record %d  era %d  %s' % (head, r['record'], r['era'], ' vs '.join(r['names'])))
        for o in r['objectives']:
            print('    objective: %s' % o)
        print('    preloads: %s' % (r['preloads'] or 'none'))
        for s in r['scenes']:
            print('    PLAY %s (%s) flag %s step %s%s' % (s['bank'], s['name'], s['flag'], s['step'], '  extra ' + str(s['extra']) if s['extra'] else ''))
            for e in s['entry']:
                print('        entered when: %s' % (' and '.join(e) or '(unconditional)'))
    print('records of the 46 matches with no non-stub chunk: %s' % (missing or 'none'))
    if a.json:
        os.makedirs(os.path.dirname(os.path.abspath(a.json)), exist_ok=True)
        json.dump(dict(rows=rows, missing=missing), open(a.json, 'w'), indent=1)
        print('wrote %s' % a.json)
    return 0


if __name__ == '__main__':
    sys.exit(main())
