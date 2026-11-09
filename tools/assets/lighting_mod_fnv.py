"""Session 98: the identity of an arena's light key for lighting mods (docs\\systems\\lighting_mod.md), from the player's own
game files (read-only).  Prints the key_size / key_fnv a mods\\lighting\\arena_NN.json must carry (the game hashes the same
decoded entry-30000 bytes in src\\lighting_mod.h), and can write a small example mod that tints one light set red.

    py -I tools\\assets\\lighting_mod_fnv.py --arena 5                      key_size, key_fnv (FNV-1a 64 of the decoded entry p0/30000 in the FILE layout; key_fnv_data = of the token streams alone)
    py -I tools\\assets\\lighting_mod_fnv.py --arena 5 --example 121 --out tools\\assets\\examples\\arena_red_example.json

The example file holds only the hash and OUR patch numbers (no game bytes).  Patch = overwrite the 25-byte payload of chunk
`chunk` (ordinal among the record's chunks of >= 25 bytes) of record (act, track); scale is int16 (colour * 4095).
It also re-checks the game's size rule: the key length is walked from the index (last record's token stream) and must equal
the entry length.
"""
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)  # -I drops the script folder from sys.path
import stagekey as sk  # noqa: E402


def fnv1a64(b):
    h = 0xcbf29ce484222325
    for c in b:
        h = ((h ^ c) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def walked_size(d, k):
    """The size rule of src\\lighting_mod.h WalkKeySize."""
    last = k.rec[-1]
    p, step = k.base + last[3], 0
    while step < last[4]:
        t = d[p]
        if t < 96:
            step += t
            p += 1
            continue
        if t == 96:
            ln, hd = d[p + 1] | d[p + 2] << 8, 3
        else:
            ln, hd = t - 96, 1
        p += hd + ln
        step += 1
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--arena', type=int, required=True)
    ap.add_argument('--example', type=int, help='light set id to tint red')
    ap.add_argument('--out')
    a = ap.parse_args()
    d = bytes(sk.arena_entries(a.arena)['p0/30000'])
    k = sk.YMKs(d)
    w = walked_size(d, k)
    print('arena %d: entry length %d, walked size %d (%s)' % (a.arena, len(d), w, 'ok' if w == len(d) else 'MISMATCH'))
    print('key_size %d key_fnv 0x%016X key_fnv_data 0x%016X' % (len(d), fnv1a64(d), fnv1a64(d[k.base:])))
    if a.example is None:
        return
    patches = []
    for track in (10, 11, 12, 13, 14):
        size, keys = sk.light_keys(k, a.example, track)
        for ordinal in range(len(keys)):
            patches.append({'act': a.example, 'track': track, 'chunk': ordinal, 'scale': [4095, 0, 0]})
    doc = {'format': 1, 'kind': 'arena', 'arena': a.arena, 'name': 'red light set %d (example)' % a.example,
           'key_size': len(d), 'key_fnv': '0x%016X' % fnv1a64(d),
           'key_fnv_data': '0x%016X' % fnv1a64(d[k.base:]), 'patches': patches}
    if a.out:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        with open(a.out, 'w') as f:
            json.dump(doc, f, indent=1)
        print('wrote %s (%d patches)' % (a.out, len(patches)))


main()
