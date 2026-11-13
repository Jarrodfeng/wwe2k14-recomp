"""S101 lane 77: scene bank id -> the arena / set the game's own editor would show it in (docs/formats/scene_location_catalogue.md).

    py -I tools/goto/scene_sets.py [--summary]            counts per rule (no game data printed)
    py -I tools/goto/scene_sets.py --lookup ID...         id -> arena (or the show's arena) and why; exit 1 if an id is in no scene pac
    py -I tools/goto/scene_sets.py --out FILE             the whole table as json (game-derived: keep under out\\, never commit)
    py -I tools/goto/scene_sets.py --selftest             two known pairs + planted off-by-one reads (exit 1 on failure)

Two inputs, both read at run time from the player's own files (read-only), nothing is stored in the repo:
  1. the scene's own header: pac/evt/{Season,Universe,WSD,Winning}NN.pac, entry /EVP/<id> (BPE block -> '0FOP' scene file):
     BE u16 at +32 = the bank id, BE u16 at +68 (= in-memory entry+36) = the scene's own location (an arena number, 0 = none, 64-83 =
     backstage kinds). This is the "entry+36" of sub_82A2A2A8 [C: listing 0x82A2A33C-0x82A2A34C; fields read from the files].
  2. the event category table: pac/menu/sas.pac CData entry 0 (CSasEventCategoryDataReader::SetupEventCategoryData, sub_82A477C0):
     LE u32 group count, per group a 12-byte header {u8 group, u8, LE u16 count, u32, u32} + count x 40-byte entries; entry LE u16 at +12 =
     the bank id, LE u16 at +10 = the flags; bit 0 = "in the arena" (sub_82A2A2A8 0x82A2A370-0x82A2A37C) [C].
The rule (sub_82A2A2A8 0x82A2A354-0x82A2A450) [C listing]:
  no catalogue entry for the bank          -> the scene's own location, unchanged (Season banks: 28-55; Winning banks: 1 or 5 / 13)
  flags bit 0 set ("in the arena")         -> the SHOW's arena (a run-time value, not a property of the scene): returned as None
  flags bit 0 clear and location in LOC78  -> 78 (the backstage set); the editor also writes a backstage-position nibble [not modelled]
  flags bit 0 clear otherwise              -> the location itself (67, 68 stay; 0-56 stay)
LOC78 = the locations sub_82A262C0 flags: a jump table of 20 words at 0x82A262E4 for location - 64 (0..19), read from the guest image
[C]: {64, 66, 69, 70, 71, 78, 80, 81, 82, 83}.
Unconfirmed [U]: what the show path returns when the show has no date (300; only banks 4335-4338 are clamped to 1 by the caller).
"""
import argparse
import glob
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tools', 'wm30'))
import scene_ids  # noqa: E402
import pac  # noqa: E402

LOC78 = frozenset((64, 66, 69, 70, 71, 78, 80, 81, 82, 83))  # sub_82A262C0's table at 0x82A262E4 [C, guest image]
FAMILY = re.compile(r'^(Season|Universe|WSD|Winning)\d\d\.pac$', re.I)
HDR_BANK, HDR_LOC = 32, 68  # offsets in the decoded '0FOP' scene file


def decide(flags, loc):
    """flags = the catalogue entry's u16 (None = no entry), loc = the scene's own location -> (arena or None, rule)."""
    if flags is None:
        return loc, 'own'
    if flags & 1:
        return None, 'show'
    if loc in LOC78:
        return 78, 'backstage78'
    return loc, 'own'


def read_catalogue(game):
    sas = pac.load(os.path.join(game, 'pac', 'menu', 'sas.pac'))
    ents = dict(pac.entries(sas))
    d = ents['p0/0']
    n = struct.unpack_from('<I', d, 0)[0]
    p, out = 4, []
    for _ in range(n):
        cnt = struct.unpack_from('<H', d, p + 2)[0]
        p += 12
        for _ in range(cnt):
            out.append((struct.unpack_from('<H', d, p + 12)[0], struct.unpack_from('<H', d, p + 10)[0]))
            p += 40
    if p != len(d) or not out:
        raise RuntimeError('sas.pac entry 0 does not parse as the event category table (%d of %d bytes)' % (p, len(d)))
    return out  # [(bank, flags)] in file order


def read_headers(game, loc_off=HDR_LOC):
    """-> {bank: (pac, loc)} for every scene of the four scene families."""
    out = {}
    for f in sorted(glob.glob(os.path.join(game, 'pac', 'evt', '*.pac'))):
        nm = os.path.basename(f)
        if not FAMILY.match(nm):
            continue
        d = pac.load(f)
        if d[:4] != b'EPAC' or d[0x800:0x804] != b'EVP ':
            raise RuntimeError('%s: no EVP directory at 0x800' % nm)
        p = 0x800 + 12
        while d[p:p + 8].strip().isdigit():
            name = int(d[p:p + 8].strip())
            sector, size = struct.unpack_from('<II', d, p + 8)
            x = pac.decode(d[0x4000 + sector * 0x800:0x4000 + sector * 0x800 + size * 256])
            if x[:4] != b'0FOP':
                raise RuntimeError('%s /EVP/%d is not a 0FOP scene file' % (nm, name))
            if struct.unpack_from('>H', x, HDR_BANK)[0] != name:
                raise RuntimeError('%s /EVP/%d: header bank id %d differs' % (nm, name, struct.unpack_from('>H', x, HDR_BANK)[0]))
            out[name] = (nm[:-4], struct.unpack_from('>H', x, loc_off)[0])
            p += 16
    if not out:
        raise RuntimeError('no scene pacs under %s' % game)
    return out


def build(game=None, entry_shift=0, loc_off=HDR_LOC):
    """-> {bank: {'pac', 'loc', 'flags', 'arena', 'rule'}}. entry_shift / loc_off exist only for the selftest's planted faults."""
    game = game or scene_ids.default_game()
    cat = read_catalogue(game)
    banks = sorted(b for b, _ in cat)
    flags = dict(cat)
    shifted = {}
    for i, b in enumerate(banks):
        j = i + entry_shift
        shifted[b] = flags[banks[j]] if 0 <= j < len(banks) else None
    t = {}
    for bank, (pn, loc) in read_headers(game, loc_off).items():
        fl = shifted.get(bank)
        arena, rule = decide(fl, loc)
        t[bank] = {'pac': pn, 'loc': loc, 'flags': fl, 'arena': arena, 'rule': rule}
    return t


# Known pairs (the doc / the logs): the editor's BACKSTAGE ASSAULT (WSD04, bank 4670) loads BG78 (coverage run 012, fixture
# tools/goto/fixtures/editor_run012_bg_lines.txt: BG78.PAC after the A press on it); Universe 5000 is an in-the-arena scene: pushed with
# arena 78 it framed only the wall (run 7073, docs/systems/goto.md), and its catalogue flag says "the show's arena" (None).
KNOWN = ((4670, 78), (5000, None))


def selftest():
    bad = []
    t = build()
    for bank, want in KNOWN:
        got = t[bank]['arena']
        if got != want:
            bad.append('real read: bank %d -> %r, wanted %r' % (bank, got, want))
    for fl, loc, want in ((None, 30, 30), (1, 64, None), (0, 64, 78), (0, 68, 68), (0, 0, 0), (0, 66, 78)):
        if decide(fl, loc)[0] != want:
            bad.append('decide(%r,%r) wrong' % (fl, loc))
    for what, kw in (('catalogue entry read one entry late', dict(entry_shift=1)), ('catalogue entry read one entry early', dict(entry_shift=-1)),
                     ('location read one field early', dict(loc_off=HDR_LOC - 2)), ('location read one field late', dict(loc_off=HDR_LOC + 2))):
        tp = build(**kw)
        if all(tp[b]['arena'] == w for b, w in KNOWN):
            bad.append('PLANT NOT CAUGHT: %s still gives the known pairs' % what)
    n = len(t)
    if n < 600:
        bad.append('only %d scenes read' % n)
    for b in bad:
        print('FAIL', b)
    print('scene_sets selftest: %d scenes read, %d problems' % (n, len(bad)))
    return 1 if bad else 0


def summary(t):
    from collections import Counter
    c = Counter((v['pac'].rstrip('0123456789'), v['rule']) for v in t.values())
    for k in sorted(c):
        print('%-10s %-12s %d' % (k[0], k[1], c[k]))
    print('total %d scenes; %d distinct arenas named' % (len(t), len({v['arena'] for v in t.values() if v['arena'] is not None})))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game')
    ap.add_argument('--summary', action='store_true')
    ap.add_argument('--lookup', nargs='+', type=int)
    ap.add_argument('--out')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    t = build(a.game)
    if a.out:
        with open(a.out, 'w') as f:
            json.dump({str(k): v for k, v in t.items()}, f)
    if a.lookup:
        rc = 0
        for i in a.lookup:
            v = t.get(i)
            if v is None:
                print('%d: in no scene pac' % i)
                rc = 1
            else:
                print('%d: %s loc=%d flags=%s -> %s (%s)' % (i, v['pac'], v['loc'], v['flags'], "the show's arena" if v['arena'] is None else v['arena'], v['rule']))
        return rc
    summary(t)
    return 0


if __name__ == '__main__':
    sys.exit(main())
