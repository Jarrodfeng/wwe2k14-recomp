r"""Session 97: a wrestler's ENTRANCE light rig (act 25011 of evt\Entrance0N.pac), read from the player's own game files
(read-only). Claims and evidence: out\runtime\s97\entrance_light.md; C++ port src\assets\entrance_light.h.

    py -I tools\assets\entrance_light.py --char 161 [--cue 0] [--slot 1] [--t 68]      colours at cue / slot / time
    py -I tools\assets\entrance_light.py --char 161 --probe 6728 [--csv out\runtime\s97\drawprobe.csv]   best t per cue/slot vs the probe tuple

Layout: the EVP directory (0x800) lists the banks; the base banks (id < 1000) are followed by one "BPE " section each, in the
same order, from the sector after the directory's last byte.  Section = PACH; leaf 11 = act "25011NNN_A11_def" (NNN = bank id):
name table + YANM per motion named SSSSTTTT; sub = 10 cue + 11 + slot (cue 0 5 10 15 20), track 0 sky, 1 ground, 10..14 lights
(colour = flags-0x0100 RGB key track, direction = b14 rotation track [U]).
"""
import argparse
import glob
import math
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MAIN = REPO if os.path.isdir(os.path.join(REPO, 'game_data')) else r'C:\Users\Jarrod\wwe2k14-recomp'
sys.path.insert(0, os.path.join(MAIN, 'tools', 'wm30'))
sys.path.insert(0, os.path.join(MAIN, 'tools', 'assets'))
import pac  # noqa: E402
import yanm  # noqa: E402

PROBE = {'c0': (.416, .349, .254), 'c1': (.201, .201, .201), 'c2': (.396, .277, .241), 'sky': (.53, .372, .323),
         'ground': (.125, .097, .077)}  # Orton entrance, swap 6728 of drawprobe.csv (run 069)


def find_section(char):
    for f in sorted(glob.glob(os.path.join(MAIN, 'game_data', 'extracted', 'pac', 'evt', 'Entrance0*.pac'))):
        d = pac.load(f)
        names, last = [], 0
        o = 0x80C
        while o + 16 <= 0x4000:
            nm = d[o:o + 8].decode('latin1').strip(' \0')
            if not nm or not nm.isdigit():
                break
            st, sz = struct.unpack_from('<II', d, o + 8)
            last = max(last, 0x4000 + st * 0x800 + sz * 256)
            if int(nm) < 1000:
                names.append(int(nm))
            o += 16
        if char not in names:
            continue
        idx = names.index(char)
        p = (last + 0x7FF) & ~0x7FF
        while d[p:p + 4] != b'BPE ':
            p += 0x800
        for _ in range(idx):
            p += (struct.unpack_from('<I', d, p + 8)[0] + 16 + 0x7FF) & ~0x7FF
        sec = pac.bpe_decode(d[p:])
        return os.path.basename(f), dict(pac.walk(sec, 's', 8))['s/11']
    raise SystemExit('no entrance pac holds bank %d' % char)


class Rig:
    def __init__(self, char):
        self.file, self.leaf = find_section(char)
        x = self.leaf
        e, _, n, _f = struct.unpack_from('>4I', x, 0)
        self.names = [x[o + 4:o + 36].split(b'\0')[0].decode() for o in range(n, e, 40)][2:]
        self.ys = [m.start() for m in re.finditer(rb'YANM', x)]
        self.cache = {}

    def tracks(self, sub, track):
        nm = '%04d%04d' % (sub, track)
        if nm not in self.names:
            return None
        if nm not in self.cache:
            self.cache[nm] = yanm.parse_motion(self.leaf, self.ys[self.names.index(nm)])
        return self.cache[nm]

    def rgb(self, sub, track, t):
        tr = self.tracks(sub, track)
        if not tr:
            return None
        for T in tr:
            if T.flags == 0x100:
                kp = T.keys_pos
                keys = [(0, kp[1])] if kp[0] == 'const' else kp
                if t <= keys[0][0]:
                    return keys[0][1]
                for (t0, a), (t1, b) in zip(keys, keys[1:]):
                    if t0 <= t <= t1:
                        u = (t - t0) / (t1 - t0) if t1 > t0 else 0
                        return tuple(a[i] + (b[i] - a[i]) * u for i in range(3))
                return keys[-1][1]
        return None

    def slot_set(self, cue, slot, t):
        sub = 10 * cue + 11 + slot
        return {'sky': self.rgb(sub, 0, t), 'ground': self.rgb(sub, 1, t),
                'lights': [self.rgb(sub, 10 + i, t) for i in range(5)]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--char', type=int, required=True)
    ap.add_argument('--cue', type=int, default=0)
    ap.add_argument('--slot', type=int, default=1)
    ap.add_argument('--t', type=float, default=68)
    ap.add_argument('--probe', type=int)
    a = ap.parse_args()
    rig = Rig(a.char)
    print('%s leaf 11: %d B, %d motions' % (rig.file, len(rig.leaf), len(rig.names)))
    if a.probe:
        tar = [PROBE['sky'], PROBE['ground'], PROBE['c0'], PROBE['c1'], PROBE['c2']]
        best = None
        for cue in (0, 5, 10, 15, 20):
            for slot in range(9):
                if rig.rgb(10 * cue + 11 + slot, 0, 0) is None:
                    continue
                for t in range(0, 1300, 2):
                    s = rig.slot_set(cue, slot, t)
                    v = [s['sky'], s['ground']] + s['lights'][:3]
                    if any(c is None for c in v):
                        continue
                    err = sum(abs(p - q) for x, y in zip(v, tar) for p, q in zip(x, y))
                    if best is None or err < best[0]:
                        best = (err, cue, slot, t, v)
        print('best: err %.4f cue %d slot %d t %d' % best[:4])
        for nm, c, p in zip(('sky', 'ground', 'c0', 'c1', 'c2'), best[4], tar):
            print('  %-6s decoded %s probe %s' % (nm, [round(q, 3) for q in c], p))
        return 0
    s = rig.slot_set(a.cue, a.slot, a.t)
    print('cue %d slot %d t %g' % (a.cue, a.slot, a.t))
    for k, v in s.items():
        print(' ', k, v if k != 'lights' else [None if c is None else [round(q, 4) for q in c] for c in v])
    return 0


if __name__ == '__main__':
    sys.exit(main())
