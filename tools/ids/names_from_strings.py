"""Session 32: every character's name by id, read from the game's own string table.

    py tools\\ids\\names_from_strings.py [FILE ...] [--json OUT] [--compare]

FILE: a string pack, `pac\\string.pac` of the game data, or a DLC pack's `pac\\string_dlcNN.pac` (each DLC string pack
is cumulative: the base game's names plus every pack up to it). Default: the newest string_dlcNN.pac under game_data\\dlc,
else game_data\\extracted\\pac\\string.pac.

How it works (found in Session 32, docs\\systems\\dlc.md "Names"): a string pack holds zlib streams, one decoded string
table per language and text group. A table is u32 0, u32 COUNT, COUNT records of (u32 offset, u32 length, u32 text
id), then the UTF-8 strings. **A character's name is text id 60000 + its character id** (THE ROCK = 60100 = id 100,
checked against all 103 named ids in db\\ids, Session 32). The first table that has the 60000 range is used (the first
language set; the names are the same in the others as far as checked).

--compare: check against db\\ids\\characters.json and dlc_characters.json. --json OUT: write {id: name}. The output is
read from the player's own files at run time: the table itself is game content (never commit a decoded table; the
names of public wrestlers already sit in db\\ids).
"""

import argparse
import glob
import json
import os
import re
import struct
import sys
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NAME_BASE = 60000


def tables(data):
    """Every decoded string table in a pack: zlib streams found by their header bytes."""
    end = -1
    for m in re.finditer(rb'\x78[\x01\x5e\x9c\xda]', data):
        s = m.start()
        if s <= end:
            continue
        d = zlib.decompressobj()
        try:
            out = d.decompress(data[s:s + (8 << 20)])
        except zlib.error:
            continue
        if len(out) < 8:
            continue
        end = s + len(data[s:s + (8 << 20)]) - len(d.unused_data) - 1
        count = struct.unpack_from('<I', out, 4)[0]
        if struct.unpack_from('<I', out, 0)[0] != 0 or not 0 < count < 200000 or 8 + 12 * count > len(out):
            continue
        yield out, count


def names_from(path):
    """The first table (the first language set) with a real name list: at least 50 ids from 100 up (other tables hold
    a stray text id or two in the same range)."""
    data = open(path, 'rb').read()
    for out, count in tables(data):
        names = {}
        for i in range(count):
            off, ln, tid = struct.unpack_from('<III', out, 8 + 12 * i)
            if NAME_BASE + 100 <= tid < NAME_BASE + 1000 and off + ln <= len(out):
                names[tid - NAME_BASE] = out[off:off + ln].split(b'\0')[0].decode('utf-8', 'replace')
        if len(names) >= 50:
            return names
    return {}


def default_file():
    dlc = sorted(glob.glob(os.path.join(REPO, 'game_data', 'dlc', '**', 'string_dlc*.pac'), recursive=True),
                 key=lambda p: os.path.basename(p).lower())
    return dlc[-1] if dlc else os.path.join(REPO, 'game_data', 'extracted', 'pac', 'string.pac')


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('files', nargs='*')
    ap.add_argument('--json')
    ap.add_argument('--compare', action='store_true')
    a = ap.parse_args()
    names = {}
    for f in a.files or [default_file()]:
        got = names_from(f)
        print('%s: %d names (ids %s..%s)' % (f, len(got), min(got, default=None), max(got, default=None)))
        for k, v in got.items():
            names.setdefault(k, v)
    if a.compare:
        ok, diff = 0, []
        for fname in ('characters.json', 'dlc_characters.json'):
            for e in json.load(open(os.path.join(REPO, 'db', 'ids', fname), encoding='utf-8'))['entries']:
                cid, ours = e.get('id'), e.get('name')
                if not isinstance(cid, int) or not ours:
                    continue
                got = names.get(cid, '')
                x = ''.join(c for c in ours.upper() if c.isalnum())
                y = ''.join(c for c in got.upper() if c.isalnum())
                if x[:5] == y[:5] or (y and (x in y or y in x)):
                    ok += 1
                else:
                    diff.append((cid, ours, got))
        print('agree with db\\ids: %d; differ: %d' % (ok, len(diff)))
        for d in diff:
            print('  id %d: ours %r, game %r' % d)
    if a.json:
        json.dump({str(k): v for k, v in sorted(names.items())}, open(a.json, 'w', encoding='utf-8'), indent=1,
                  ensure_ascii=False)
        print('wrote', a.json)
    else:
        for k in sorted(names):
            print('%4d  %s' % (k, names[k]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
