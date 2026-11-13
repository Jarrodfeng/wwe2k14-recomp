"""S101 lane 32: the scene bank-id table. Which scene pac holds each EVP bank id ("/EVP/<id>", docs/formats/evt_scene_ids.md).

    py -I tools/goto/scene_ids.py [--game DIR] [--out FILE] [--summary]      build / print the table
    py -I tools/goto/scene_ids.py --lookup ID...                              which pac holds each id (exit 1 if one is in no pac)

Reads plist360.def (the pac list, line k after the comment line = pac k) and plist360.arc (big-endian rows of 12 bytes, the format of
docs/formats/catalog_dlc.md): a group header word 0xFF000000 (pac 0) or 0xFFFF00kk (pac kk), then directory rows {tag, flags, 0} each
followed by its entry rows {name, sector, size/256}; flags >> 20 = the entry bytes / 4, flags & 0x1000 = 8-character names (16-byte
rows). The scene pacs (pac\\evt\\Entrance|Season|Universe|Winning|WSD NN.pac) have an "EVP " directory whose entry names are the bank ids
as decimal text. The full table is made from the player's own files into out\\goto\\scene_ids.json (game data: never committed); the doc
holds ranges and counts only.
"""
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PAC_RE = re.compile(r'^pac\\evt\\(Entrance|Season|Universe|Winning|WSD)(\d\d)\.pac$', re.I)
# Range families (S-3 / S-2: one bank-id space across the scene pacs).
FAMILIES = ((0, 999, 'entrance'), (1010, 2110, 'season'), (4000, 4670, 'wsd'), (5000, 5661, 'universe'), (6100, 9500, 'winning'))


def main_checkout():
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return env
    if os.path.isdir(os.path.join(REPO, 'game_data')):
        return REPO
    up = os.path.abspath(os.path.join(REPO, '..', '..', '..'))
    return up if os.path.isdir(os.path.join(up, 'game_data')) else REPO


def default_game():
    return os.path.join(main_checkout(), 'game_data', 'extracted')


def default_out():
    return os.path.join(main_checkout(), 'out', 'goto', 'scene_ids.json')


def read_def(path):
    """The pac list: index k = the k-th line after the ';PAC_LIST...' comment line."""
    with open(path, 'rb') as f:
        lines = f.read().decode('latin-1').split('\r\n')
    if not lines or not lines[0].startswith(';'):
        raise RuntimeError('%s: the first line is not the ";PAC_LIST" comment (is this plist360.def?)' % path)
    return [ln for ln in lines[1:] if ln]


def read_arc(path):
    """-> {pac_index: {tag: [entry names]}}; raises on a malformed file (never returns a silently empty table)."""
    with open(path, 'rb') as f:
        d = f.read()
    if len(d) < 16 or len(d) % 4:
        raise RuntimeError('%s: not a plist360.arc (size %d)' % (path, len(d)))
    pos, pac, out = 0, None, {}
    while pos < len(d):
        w = struct.unpack_from('>I', d, pos)[0]
        if w == 0xFFFFFFFF:
            return out
        if w == 0xFF000000:
            pac, pos = 0, pos + 4
        elif (w >> 16) == 0xFFFF:
            pac, pos = w & 0xFFFF, pos + 4
        else:
            if pac is None:
                raise RuntimeError('%s: rows before the first group header' % path)
            tag = d[pos:pos + 4].decode('latin-1')
            flags = struct.unpack_from('>I', d, pos + 4)[0]
            pos += 12
            wide = bool(flags & 0x1000)
            rowsize = 16 if wide else 12
            nbytes = ((flags >> 20) & 0xFFF) * 4
            if nbytes % rowsize:
                raise RuntimeError('%s: directory %s of pac %d: %d entry bytes is not a multiple of %d' % (path, tag, pac, nbytes, rowsize))
            names = []
            for _ in range(nbytes // rowsize):
                names.append(d[pos:pos + (8 if wide else 4)].decode('latin-1').rstrip('\x00 '))
                pos += rowsize
            out.setdefault(pac, {}).setdefault(tag, []).extend(names)
    raise RuntimeError('%s: ends without the 0xFFFFFFFF terminator' % path)


def build(game=None):
    """-> {'ids': {id: pac_name}, 'pacs': {pac_name: {'count', 'min', 'max'}}}: every scene pac's EVP bank ids."""
    game = game or default_game()
    pacs = read_def(os.path.join(game, 'plist360.def'))
    arc = read_arc(os.path.join(game, 'plist360.arc'))
    ids, per = {}, {}
    for k, name in enumerate(pacs):
        m = PAC_RE.match(name)
        if not m:
            continue
        pac = '%s%s' % (m.group(1), m.group(2))
        evp = arc.get(k, {}).get('EVP ')
        if evp is None:
            raise RuntimeError('%s (pac %d) has no EVP directory in plist360.arc' % (name, k))
        for e in evp:
            if not e.isdigit():
                raise RuntimeError('%s: EVP entry "%s" is not a decimal bank id' % (name, e))
            i = int(e)
            if str(i) in ids:
                raise RuntimeError('bank id %d is in two pacs (%s, %s)' % (i, ids[str(i)], pac))
            ids[str(i)] = pac
        v = [int(e) for e in evp]
        per[pac] = {'count': len(v), 'min': min(v) if v else None, 'max': max(v) if v else None}
    if not ids:
        raise RuntimeError('no scene pacs found in %s: the table would be empty' % os.path.join(game, 'plist360.def'))
    return {'ids': ids, 'pacs': per}


_CACHE = {}


def table(game=None, out=None):
    """The table: out\\goto\\scene_ids.json, made from the game files if absent (a given `game` always rebuilds, writes nothing)."""
    key = (game, out)
    if key in _CACHE:
        return _CACHE[key]
    out = out or default_out()
    if game is None and os.path.isfile(out):
        with open(out) as f:
            t = json.load(f)
        if 'ids' not in t or 'pacs' not in t:
            raise RuntimeError('%s is not a scene id table: delete it' % out)
    else:
        t = build(game)
        if game is None:
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, 'w') as f:
                json.dump(t, f, sort_keys=True)
    _CACHE[key] = t
    return t


def pac_of(i, game=None):
    """The pac name that holds bank id i, or None."""
    return table(game)['ids'].get(str(int(i)))


def family(i):
    """The id's range family: 'entrance' < 1000, 'season' 1010-2110, 'wsd' 4000-4670, 'universe' 5000-5661, 'winning' 6100-9500, else None."""
    i = int(i)
    for lo, hi, name in FAMILIES:
        if lo <= i <= hi:
            return name
    return None


def summary(t):
    lines = []
    for pac in sorted(t['pacs']):
        p = t['pacs'][pac]
        lines.append('%-11s %3d ids  %s..%s' % (pac, p['count'], p['min'], p['max']))
    lines.append('total %d ids in %d pacs' % (len(t['ids']), len(t['pacs'])))
    return lines


def main(argv):
    game = None
    lookups, show = [], False
    it = iter(argv)
    for a in it:
        if a == '--game':
            game = next(it)
        elif a == '--summary':
            show = True
        elif a == '--lookup':
            lookups = list(it)
        else:
            print('unknown argument %s' % a)
            return 2
    t = table(game)
    bad = 0
    for x in lookups:
        pac = t['ids'].get(str(int(x)))
        print('%s  ->  %s' % (x, pac or 'NOT IN ANY PAC'))
        bad += pac is None
    if show or not lookups:
        print('\n'.join(summary(t)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
