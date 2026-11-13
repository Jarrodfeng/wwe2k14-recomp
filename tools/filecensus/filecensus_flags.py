"""Lane 21: the arena's r / y slot count read straight from the FILE (entry 50001 of pac\\bg\\bgNN.pac), with no help from the viewer's loader.

  arena_flagged_slots(arena_id) -> the number of stage slots (entries 0..399 of the arena's pac) that entry 50001 flags `r` (also drawn mirrored) or `y`
  (also drawn turned 180 degrees) and that exist as an entry. This is the independent side of the census's draw-list check: a loader that ignores
  the flags (S97's half arena) shortens the in-memory model and the draw list alike, but not this number.
The pac reader is tools\\wm30\\pac.py (read-only on the player's game files; XCTD unpacking cached under the main checkout's out\\wm30\\cache)."""
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'wm30'))
import pac  # noqa: E402


def _entries(arena_id):
    path = os.path.join(pac.GAME, 'pac', 'bg', 'bg%02d.pac' % arena_id)
    if not os.path.exists(path):
        raise RuntimeError('cannot read %s' % path)
    data = pac.load(path)
    p = 0x4000 if data[:4] == b'EPAC' else 0
    while p + 8 <= len(data) and data[p:p + 4] != b'PACH':
        p += 0x800
    if data[p:p + 4] != b'PACH':
        raise RuntimeError('no PACH in %s' % path)
    ents = {}
    for eid, raw in pac.pach(data, p):
        ents.setdefault(eid, raw)
    return ents


def parse_slot_flags(text):
    """Entry 50001 text: lines '<slot> <flags...> ; comment' -> {slot: set(whole-word flags)}."""
    out = {}
    for line in text.replace('\r', '').split('\n'):
        m = re.match(r'\s*(\d+)\b(.*)$', line)
        if m:
            out.setdefault(int(m.group(1)), set()).update(m.group(2).split(';', 1)[0].split())
    return out


def drawn_in_match(slot, toks, ents, look='match'):
    """The game's own rule for which slots a MATCH look draws (S98 stage fix, src/assets/asset_load.h `slot_when`): the file's legend flags `e` = shown only in
    events (entrances) and `g` = only in the game; the match look hides a slot with `e` and without `g`. And with the entrance stage present (keys 801 / 802)
    the screen-off stage 301 / 302 / 304 is not drawn. [L: file legend + loader rule; U at run time: nothing has tested that the match hides `e` slots. Arena 54: slots 216 and 220 carry `r` and `e`]"""
    hide, other = ('e', 'g') if look == 'match' else ('g', 'e')  # the entrance look hides `g` without `e`
    if hide in toks and other not in toks:
        return False
    if look == 'match' and slot in (301, 302, 304) and (801 in ents or 802 in ents):
        return False
    return True


def arena_flagged_slot_list(arena_id, look='match'):
    """The slots a `look` ('match', 'entrance' or 'all') draws more than once: entry 50001 flags `r` / `y`, the slot exists as an entry in 0..399 and the look shows it."""
    ents = _entries(arena_id)
    if 50001 not in ents:
        return []
    flags = parse_slot_flags(pac.decode(ents[50001]).decode('latin1'))
    return sorted(slot for slot, toks in flags.items()
                  if slot in ents and 0 <= slot < 400 and ({'r', 'y'} & toks) and (look == 'all' or drawn_in_match(slot, toks, ents, look)))


def arena_flagged_slots(arena_id, look='match'):
    return len(arena_flagged_slot_list(arena_id, look))


if __name__ == '__main__':
    ids = [int(x) for x in sys.argv[1:]] or [5]
    for i in ids:
        print(i, arena_flagged_slots(i), 'all-slots:', arena_flagged_slots(i, 'all'))
