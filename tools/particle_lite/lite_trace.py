r"""S102 lane T18: export a capture's op sequence as a small text trace for tools\particle_lite\particle_lite_test.cpp. Offline, 0 game runs.

  py -I tools\particle_lite\lite_trace.py CAPTURE OUT.txt [--frames A:B]

CAPTURE = a case zip or a capture folder (tools\particles\pcap.py). OUT is a text file under out\ (hashes and counts of the recorded tables only: no game bytes are written, only 64-bit digests).
Format, one line per op in each frame's op list:
  F <capture frame> <guest frame>
  X                       an op that is not a draw (a pass, a copy, a clear, a present)
  D <cand> <vs_hash> <ps_hash> <prim> <flags> <fixed> <dynamic> <layout> <bool_loop> <slots> <site> <pass> <object> <part> <widget> <vs_bank> <ps_bank> <geo>
cand = 1 for a hard-yParticle draw (the two measured shaders, op 19 inline) at the sprite or quad call site: the draws the live gate looks at.
slots / vs_bank / ps_bank / geo are 16-hex-digit digests: the draw's texture slot records (with each sampler's own bytes), its two constant banks (bytes, never floats), its inline vertices.
Banks are read only for candidate draws (a frame holds up to ~5,000 banks)."""
import hashlib
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'particles'))
import pcap  # noqa: E402

KVS, KPS = 0x5F1EF76E69256C9C, 0x34B29856453FE506
SITES = (0x82A0DA58, 0x82A0E338)
FLAG_INLINE = 1  # cap::kDrawInline (adapter_frame.h DrawFlags) - checked below against the capture


def dig(b):
    return int.from_bytes(hashlib.blake2b(b, digest_size=8).digest(), 'little')


def export(path, out, a=None, b=None):
    cap = pcap.Capture(path)
    n = len(cap.frames)
    lo, hi = (0, n) if a is None else (a, min(b, n))
    rows = cands = 0
    with open(out, 'w') as f:
        for i in range(lo, hi):
            fr = cap.frame(i)
            f.write('F %d %d\n' % (fr.capture_frame, fr.guest_frame))
            if not fr.has_origins:
                raise pcap.PError('frame %d: no draw origins (a version-1 capture): the call sites are unknowable' % i)
            for kind, idx in zip(fr.ops['kind'], fr.ops['index']):
                if kind != pcap.OP_DRAW:
                    f.write('X\n')
                    continue
                d, o = fr.draws[idx], fr.origins[idx]
                site = pcap.queue_site(o)
                cand = int(d['vs_hash']) == KVS and int(d['ps_hash']) == KPS and int(o['op']) == 19 and site in SITES
                slots = dig(fr.slot_bytes(d))
                if cand:
                    cands += 1
                    vsb = dig(fr.bank(int(d['vs_bank'])).tobytes())
                    psb = dig(fr.bank(int(d['ps_bank'])).tobytes())
                    geo = dig(fr.vertices(d).tobytes())
                else:
                    vsb, psb, geo = int(d['vs_bank']), int(d['ps_bank']), int(d['vertex_offset'])
                f.write('D %d %x %x %d %d %d %d %d %d %x %x %x %x %d %d %x %x %x %d %d %x\n' % (
                    cand, int(d['vs_hash']), int(d['ps_hash']), int(d['source_prim']), int(d['flags']), int(d['fixed']), int(d['dynamic']), int(d['layout']),
                    int(d['bool_loop']), slots, site, int(o['pass_site']), int(o['object']), int(pcap.queue_part(o)), int(o['widget']), vsb, psb, geo, int(o['klass']), int(o['op']), int(o['technique'])))
                rows += 1
    return hi - lo, rows, cands


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        return 2
    a = b = None
    if '--frames' in args:
        k = args.index('--frames')
        a, b = [int(x) for x in args[k + 1].split(':')]
        del args[k:k + 2]
    try:
        frames, rows, cands = export(args[0], args[1], a, b)
    except Exception as e:  # a tool that reads nothing must fail loudly
        print('FAIL: %s' % e)
        return 1
    if not rows:
        print('FAIL: no draws read')
        return 1
    print('trace: %d frames, %d draws, %d candidates -> %s' % (frames, rows, cands, args[1]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
