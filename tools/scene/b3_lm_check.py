r"""S103 B3 (docs\research\s103\T3b_lightmap_writer.md): the stage LightMapParam (vs c184) is computed from the game's own arena table, not held by us. Offline, 0 game runs.

  py -I tools\scene\b3_lm_check.py [--plant shift|swap|gain01] [--frames N] [LABEL ...]     (default: the 13 captured arenas a00 a01 a02 a04 a08 a09 a16 a30 a41 a44 a52 a54 a55)

NOT circular: the value under test is computed from the player's `pac\misc02_ingame.pac` leaf p0/102/5 by the loader's own parser (src\assets\arena_lm.h, tools\scene\b3_unit.cpp) for the arena id,
and compared BIT FOR BIT with what each capture's draws carry in vs bank register 184 (what the game uploaded, read from the stream): x, y, z of the lit-LM draws, w = 1. The cards' z is checked against
the game's rule z = (float)word * 0.001 * (G * 0.01) with the word the captures imply (850; the record carries no word yet): the rule must reproduce the captured card z exactly.
--plant (planted twins, the gate inverts the exit code): shift = the record one index off, swap = X and Z exchanged, gain01 = G * 0.1 instead of G * 0.01: every one must FAIL on the captures.
Numbers only. Nothing from the game is written except the decoded leaf under out\ (never committed)."""
import argparse
import os
import struct
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'crowd'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'particles'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'solver'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'wm30'))
import cards_census as cc  # noqa: E402
import pac  # noqa: E402

ARENAS = ['a00', 'a01', 'a02', 'a04', 'a08', 'a09', 'a16', 'a30', 'a41', 'a44', 'a52', 'a54', 'a55']
CARD_WORD = 850   # [L] the word the captures imply (2.125 / 2.5 = 0.85); the writer of [0x8376383C] is unfound


def bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def f32(x):
    return float(np.float32(x))


def sample(cap, fi):
    """-> set of (x, y, z, w) of every distinct vs bank whose c184 looks like a stage span (x, y in 1/9000..1/3000)."""
    f = cap.frame(fi)
    D = f.draws
    out = set()
    if len(D) == 0:
        return out
    for i in np.unique(D['vs_bank']):
        try:
            v = cc.fl(f.bank(int(i)), 184, 1)
        except IndexError:
            continue   # a bank id the capture does not define
        x, y, z, w = (float(t) for t in v[:4])
        if 1 / 9000.0 < x < 1 / 3000.0 and 1 / 9000.0 < y < 1 / 3000.0 and z > 0:
            out.add((x, y, z, w))
    return out


def dump_leaf(path):
    d = pac.load(os.path.join(pac.GAME, 'pac', 'misc02_ingame.pac'))
    for n, b in pac.entries(d):
        if n == 'p0/102/5':
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, 'wb').write(b)
            return len(b)
    raise SystemExit('FAIL  misc02_ingame.pac has no p0/102/5')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('labels', nargs='*')
    ap.add_argument('--plant', choices=('shift', 'swap', 'gain01'))
    ap.add_argument('--frames', type=int, default=3)
    ap.add_argument('--unit', default=os.path.join(ROOT, 'out', 'tools', 'b3_unit.exe'))
    a = ap.parse_args()
    if not os.path.exists(a.unit):
        print('FAIL  %s is not built (the gate lane builds it)' % a.unit)
        return 1
    leaf = os.path.join(ROOT, 'out', 'tools', 'b3_leaf.bin')
    size = dump_leaf(leaf)
    labels = a.labels or ARENAS
    ids = [int(l[1:3]) for l in labels]
    cmd = [a.unit, leaf] + (['--plant', a.plant] if a.plant else []) + [str(i) for i in ids]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print('FAIL  unit: %s' % (r.stdout.strip() or r.stderr.strip()))
        return 1
    ours = {}
    for line in r.stdout.splitlines():
        t = line.split()
        if len(t) == 5 and t[0].lstrip('-').isdigit():
            ours[int(t[0])] = tuple(int(x, 16) for x in t[1:])
    fails = 0
    print('      leaf p0/102/5: %d bytes; the unit computed %d arenas%s' % (size, len(ours), '' if not a.plant else '  (plant: %s)' % a.plant))
    for label, aid in zip(labels, ids):
        paths = [p for p in (os.path.join(cc.pcap.MAIN, 'out', 'captures', label + s) for s in ('_match', '_entrance', '')) if os.path.exists(p)]
        if not paths:
            print('FAIL  %-14s no capture on disk' % label)
            fails += 1
            continue
        seen = set()
        for path in paths:   # the match capture and the entrance capture of the arena, where both exist (the cards are drawn in the entrance)
            cap = cc.Cap(path)
            n = len(cap.frames)
            for fi in sorted({int(round(k * (n - 1) / max(1, a.frames - 1))) for k in range(a.frames)}):
                seen |= sample(cap, fi)
        if not seen:
            print('FAIL  %-14s no light-mapped draw' % label)
            fails += 1
            continue
        gain = max(z for x, y, z, w in seen)
        body = sorted(s for s in seen if abs(s[2] - gain) < 1e-6)
        cards = sorted({s[2] for s in seen if abs(s[2] - gain) > 1e-6})
        spans = {(bits(s[0]), bits(s[1]), bits(s[2]), bits(s[3])) for s in body}
        got = ours[aid]
        ok = spans == {got}
        print('%s  %-14s arena %-2d c184 ours (1/%.0f, 1/%.0f, %.4g, %g) vs the stream %s' % ('PASS' if ok else 'FAIL', label, aid, 1 / struct.unpack('<f', struct.pack('<I', got[0]))[0],
              1 / struct.unpack('<f', struct.pack('<I', got[1]))[0], struct.unpack('<f', struct.pack('<I', got[2]))[0], struct.unpack('<f', struct.pack('<I', got[3]))[0],
              ' bit-exact (%s)' % ', '.join(p.rsplit(os.sep, 1)[-1] for p in paths) if ok else 'DIFFERS: stream %s' % [tuple(struct.unpack('<f', struct.pack('<I', b))[0] for b in s) for s in sorted(spans)]))
        fails += 0 if ok else 1
        if cards:
            g = np.float32(struct.unpack('<f', struct.pack('<I', got[2]))[0])
            rule_f32 = float(np.float32(CARD_WORD) * np.float32(0.001) * g)      # (float)word * 0.001f * z, all single
            rule_mix = float(np.float32(float(np.float32(CARD_WORD)) * 0.001) * g)   # the 0.001 as a double, rounded to single before the multiply
            okc = any(bits(c) == bits(rr) for c in cards for rr in (rule_f32, rule_mix))
            print('%s  %-14s cards z %s: the game rule (word %d x 0.001 x gain) gives %.9g / %.9g' % ('PASS' if okc else 'FAIL', label, ['%.9g' % c for c in cards], CARD_WORD, rule_f32, rule_mix))
            fails += 0 if okc else 1
    print('%d FAIL%s' % (fails, '' if not a.plant else '  (plant: %s)' % a.plant))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
