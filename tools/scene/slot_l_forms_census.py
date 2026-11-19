"""S103 B4X fix F1: counts the slot-text light tokens l(N) / le(N) / lg(N) over every arena's entry 50001 (names / counts only; reads the player's own files).
The game's slot parser sub_824E8628 (token 'l', jump table 0x824E8948 entry 11 -> 0x824E89B0) accepts the three forms; src\\scene_live\\arena_contrib.cpp ParseSlotText reads l(N) only
and marks a row with an le( / lg( token as unreadable (SlotLightSet = kNone). Usage: py -I tools\\scene\\slot_l_forms_census.py"""
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'filecensus'))
import filecensus_flags as fc  # noqa: E402
pac = fc.pac

TOK = re.compile(r'(?:^|[\s;])(l[eg]?)\((\d*)\)?')


def main():
    forms = {'l': 0, 'le': 0, 'lg': 0}
    rows_with = {'le': [], 'lg': []}
    n_arenas = 0
    for aid in range(0, 200):
        try:
            ents = fc._entries(aid)
        except Exception:
            continue
        if 50001 not in ents:
            continue
        n_arenas += 1
        text = pac.decode(ents[50001]).decode('latin1').replace('\r', '')
        for line in text.split('\n'):
            m = re.match(r'\s*(\d+)\b(.*)$', line)
            if not m:
                continue
            for t in TOK.finditer(m.group(2)):
                forms[t.group(1)] += 1
                if t.group(1) != 'l':
                    rows_with[t.group(1)].append((aid, int(m.group(1))))
    print('arenas read: %d' % n_arenas)
    print('tokens: l( %d  le( %d  lg( %d' % (forms['l'], forms['le'], forms['lg']))
    for k in ('le', 'lg'):
        arenas = sorted({a for a, _ in rows_with[k]})
        print('%s( rows: %d in arenas %s' % (k, len(rows_with[k]), arenas))


if __name__ == '__main__':
    main()
