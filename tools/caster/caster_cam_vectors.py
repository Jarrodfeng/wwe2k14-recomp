#!/usr/bin/env python3
"""Lane 211 (Caster D3 E3): export the numbers the camera-row check needs from a KEPT capture, NUMBERS ONLY (written under out\\, never committed):
  py -I tools\\caster\\caster_cam_vectors.py CAPTURE_DIR OUT.txt [--frames 50,100,200,300]

Per sampled stream frame i, with the record of frame i - 1 (the +1 pairing of lane 152):
  STATE f k <48 words shadow_m> <16 words shadow_f>        the record's colour-call light states (kScLightStates), raw 32-bit words in hex
  DRAW  f 0x<pass_site> <c54 c56-59 c64-67 c162 of the VS bank = 4 + 16 + 16 + 4 = 40 words> <c168-c170 of the PS bank = 12 words>   for every draw at a caster pass site
The consumer is `d3_caster_test.exe cam OUT.txt` (the SHIPPED rdr_scene::BuildCasterRows against these draws). The kept captures hold NO pass-entry records (SKIP_GAME_WORK_MAP.md 17.4 item 3), so this checks the
rules against the colour-call states: what it can prove is the matrix -> register layout (which matrix, which column) where a colour-call state IS the camera (the plane mask, the props map); what it cannot is the
wrestlers' A / B maps (their camera is no colour-call state: the [U] of 17.2 item 2, settled only by the live verify run)."""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'skip'))
import ledger as L  # noqa: E402

SITES = (0x82C04634, 0x82BF4D38, 0x82BFBD58)


def words(floats):
    return struct.unpack('<%dI' % len(floats), struct.pack('<%df' % len(floats), *floats))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('out')
    ap.add_argument('--frames', default='')
    ap.add_argument('--offset', type=int, default=1, help='record index = stream frame - offset (1 = the kept captures pairing)')
    ap.add_argument('--caster', action='store_true', help='use the pass-entry camera states (kScCasterCalls cameras) instead of the colour-call light states')
    a = ap.parse_args()
    cap = L.Capture(a.capture)
    n = cap.nframes if hasattr(cap, 'nframes') else None
    frames = [int(x) for x in a.frames.split(',')] if a.frames else [50, 100, 150, 200, 250, 300]
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w') as fh:
        for fi in frames:
            f = cap.frame(fi)
            rec = cap.record(fi - a.offset)
            states = [c['camera'] for c in rec.caster_calls() if c.get('camera')] if a.caster else rec.light_states()
            for k, s in enumerate(states):
                w = []
                for m in s['shadow_m']:
                    w += words(m)
                w += words(s['shadow_f'])
                fh.write('STATE %d %d %s\n' % (fi, k, ' '.join('%08x' % x for x in w)))
            for i in range(f.ndraws):
                o = f.origin(i)
                if o['pass_site'] not in SITES:
                    continue
                d = f.draw(i)
                vb, pb = f.bank(d['vs_bank']), f.bank(d['ps_bank'])
                w = list(vb[54 * 4:54 * 4 + 4]) + list(vb[56 * 4:60 * 4]) + list(vb[64 * 4:68 * 4]) + list(vb[162 * 4:162 * 4 + 4]) + list(pb[168 * 4:171 * 4])
                fh.write('DRAW %d 0x%08x %s\n' % (fi, o['pass_site'], ' '.join('%08x' % (x & 0xFFFFFFFF) for x in w)))
    print('wrote', a.out)


if __name__ == '__main__':
    main()
