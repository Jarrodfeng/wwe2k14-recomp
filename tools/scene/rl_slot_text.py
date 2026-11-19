#!/usr/bin/env python3
r"""Lane RL (Session 103): print an arena's entry-50001 slot-text lines for a slot range (default the ring / stage keys 800-999) and the 30002 records' camera-attach flags.

  py -I tools\scene\rl_slot_text.py --arena 4 [--from 800] [--to 999]

What it answers: which `l(N)` light set the slot text gives the ring entries (slots numbered from 800, docs\formats\light_set.md "Slot text"), and per light set the
784-byte record's follow flags (+0x120 + 4 i). Reads the player's own pac (game_data\extracted, main checkout when run from a worktree); prints text only, writes nothing.
"""
import argparse, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'arena'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'wm30'))
import arena_files  # noqa: E402
import pac  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--arena', type=int, required=True)
    ap.add_argument('--from', dest='lo', type=int, default=800)
    ap.add_argument('--to', dest='hi', type=int, default=999)
    a = ap.parse_args()
    path = os.path.join(pac.GAME, 'pac', 'bg', 'bg%02d.pac' % a.arena)
    data = pac.load(path)
    p = 0x4000 if data[:4] == b'EPAC' else 0
    while p + 8 <= len(data) and data[p:p + 4] != b'PACH':
        p += 0x800
    raw = {}
    for eid, b in pac.pach(data, p):
        raw.setdefault(eid, b)
    if 50001 not in raw:
        print('no 50001 text in', path)
        return 1
    text = pac.decode(raw[50001]).decode('latin1')
    for line in text.splitlines():
        s = line.strip()
        if not s or not s[0].isdigit():
            continue
        n = int(''.join(c for c in s.split()[0] if c.isdigit()) or -1)
        if a.lo <= n <= a.hi:
            print('slot text:', s)
    return 0


if __name__ == '__main__':
    sys.exit(main())
