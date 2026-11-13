#!/usr/bin/env python3
"""Lane 226 (Caster D3 E5, SKIP_GAME_WORK_MAP.md 17.11): where the plane mask's registers (VS c0-c16, c72 EyePos, c76.. palette) come from, measured on a kept capture, 0 game runs.

  py -I tools\\caster\\plane_regs_probe.py CAPTURE_DIR [--frames 100:340:40] [--detail]

For every plane-mask draw (pass_site 0x82BF4D38) of the sampled frames: per object, the number of draws, the draw's EyePos (c72, distinct values), its palette slots (c76 + 3k .. +2, k < 21) matched against
(a) the ring-rows record rows of the same frame (kScRingRows, row = 4x3 of a node), (b) the wrestler record's bones (rows j of a node), (c) the plane caster call's own r5 / n_nodes / flags.
"""
import argparse, collections, os, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'skip'))
import ledger as L  # noqa: E402

PLANE = 0x82BF4D38


def u(v):
    return v & 0xFFFFFFFF


def f32(w):
    return struct.unpack('<f', struct.pack('<I', w))[0]


def bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('--frames', default='100:340:40')
    ap.add_argument('--detail', action='store_true')
    a = ap.parse_args()
    cap = L.Capture(a.capture)
    lo, hi, st = [int(x) for x in a.frames.split(':')]
    for fi in range(lo, hi, st):
        f = cap.frame(fi)
        rec = cap.record(fi)
        ring = rec.ring_rows()
        ring_rows = set()
        for r in ring:
            for row in r['rows']:
                for j in range(3):
                    ring_rows.add(tuple(bits(x) for x in row[4 * j:4 * j + 4]))
        bone_rows = set()
        for b in rec.bones():
            for j in range(3):
                bone_rows.add(tuple(bits(b[c * 4 + j]) for c in range(4)))
        calls = [c for c in rec.caster_calls() if c['pass_'] == 2]
        by_obj = collections.defaultdict(lambda: dict(draws=0, eyes=collections.Counter(), slots=0, in_ring=0, in_bones=0, none=0, pos=collections.Counter(), parts=collections.Counter()))
        for i in range(f.ndraws):
            o = f.origin(i)
            if not o or o['pass_site'] != PLANE or not o['object']:
                continue
            vb = f.bank(f.draw(i)['vs_bank'])
            d = by_obj[o['object']]
            d['draws'] += 1
            d['parts'][o['part']] += 1
            d['eyes'][tuple(f32(vb[72 * 4 + k]) for k in range(4))] += 1
            d['pos'][tuple(f32(vb[7 * 4 + k]) for k in range(4))] += 1
            for s in range(21):
                row = [tuple(u(vb[(76 + 3 * s + j) * 4 + k]) for k in range(4)) for j in range(3)]
                d['slots'] += 1
                ir = all(r in ring_rows for r in row)
                ib = all(r in bone_rows for r in row)
                d['in_ring'] += ir
                d['in_bones'] += ib
                d['none'] += not (ir or ib)
        print('frame %d: ring record entries %d, plane caster calls %d (object, r5, n_nodes, flags: %s)' % (fi, len(ring), len(calls), [(hex(c['object']), hex(c['r5']), c['n_nodes'], c['flags']) for c in calls[:8]]))
        for ob, d in sorted(by_obj.items()):
            print('  object 0x%08X: %d draws, parts %s, EyePos %s, PlanePos %s, slots %d: in ring rows %d, in bones %d, neither %d' % (
                ob, d['draws'], dict(d['parts']), dict(d['eyes']) if a.detail or len(d['eyes']) < 6 else '%d distinct' % len(d['eyes']), dict(d['pos']), d['slots'], d['in_ring'], d['in_bones'], d['none']))


if __name__ == '__main__':
    main()
