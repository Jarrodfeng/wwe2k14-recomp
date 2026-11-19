#!/usr/bin/env python3
"""S103 WSL fix round: a file-level count of the `e` slots the LIVE arena model used to lose (asset_load.h deleted every e-without-g slot: the live load never set stage_media::Cfg().wrestler, so
hide = 1) and now keeps (load_detail::KeepLookSlots, set by live_assets.cpp). For each arena: the slots whose entry 50001 row carries `e` without `g` and that exist as an entry (0..399).
The per-frame mask (stage_slot_vis.h SlotLookSkipped rule 2) hides every one of them at any phase but 4; at phase 4 the game draws them.
  py -I tools\\arena\\slot_look_count.py [--arenas 0 1 2 ...]     (reads the player's pac\\bg files through tools\\filecensus; 0 game runs)
Prints one line per arena with such slots and the totals. Also lints the wiring (the keep flag is set by the live load, tested by the loader)."""
import os
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'filecensus'))
import filecensus_flags as F  # noqa: E402
import pac  # noqa: E402


def e_only_slots(arena):
    ents = F._entries(arena)
    if 50001 not in ents:
        return []
    flags = F.parse_slot_flags(pac.decode(ents[50001]).decode('latin1'))
    return sorted(s for s, t in flags.items() if s in ents and 0 <= s < 400 and 'e' in t and 'g' not in t)


def wiring_ok():
    def text(rel):
        with open(os.path.join(ROOT, rel), encoding='utf-8') as f:
            return f.read()
    bad = 0
    for rel, needle in (('src/renderer_scene/live_assets.cpp', 'KeepLookSlots() = true'), ('src/assets/asset_load.h', 'if (!load_detail::KeepLookSlots())'), ('src/assets/load_plants.h', 'KeepLookSlots()')):
        if needle not in text(rel):
            print('FAIL: wiring: %s lacks %s' % (rel, needle))
            bad += 1
    return bad == 0


def main():
    args = sys.argv[1:]
    arenas = [int(a) for a in args[1:]] if args and args[0] == '--arenas' else list(range(0, 90))
    total, n_arenas, shown = 0, 0, []
    for a in arenas:
        try:
            s = e_only_slots(a)
        except Exception:
            continue
        if s:
            total += len(s)
            n_arenas += 1
            shown.append('bg%02d: %d (%s)' % (a, len(s), ' '.join(map(str, s))))
    print('\n'.join(shown))
    print('e-without-g slots now kept in the live model: %d in %d arenas (masked at every phase but 4)' % (total, n_arenas))
    ok = wiring_ok()
    print('wiring', 'OK' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
