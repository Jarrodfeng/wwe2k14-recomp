r"""S102 lane SW2: census of consecutive draws with identical state inputs (R23 cut 5). Offline, 0 game runs. Reads a case zip or capture folder (tools\particles\pcap.py).

  py -I tools\draw_lite\census.py CAPTURE [--frames A:B] [--name NAME]

For each pair of consecutive draws in a frame's op list (non-draw ops between them counted as a gap), compares what the reader derives per draw:
  shaders (vs / ps / patched hash), prim + inline flag, fixed, dynamic, layout, bool_loop, the texture slot records (with the samplers' own bytes) -> "state same".
Then what is still per draw even when the state is the same: constant banks (vs / ps bytes) and geometry (the same vertex / index source).
Output is counts only (no game bytes). Class names from the origins section."""
import hashlib
import os
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'particles'))
import pcap  # noqa: E402


def dig(b):
    return hashlib.blake2b(b, digest_size=8).digest()


def census(path, a=None, b=None):
    cap = pcap.Capture(path)
    n = len(cap.frames)
    lo, hi = (0, n) if a is None else (a, min(b, n))
    tot = defaultdict(int)
    cls = defaultdict(lambda: defaultdict(int))
    for i in range(lo, hi):
        fr = cap.frame(i)
        if not fr.has_origins:
            raise pcap.PError('frame %d: no origins' % i)
        prev = None
        gap = False
        for kind, idx in zip(fr.ops['kind'], fr.ops['index']):
            if kind != pcap.OP_DRAW:
                gap = True
                continue
            d, o = fr.draws[idx], fr.origins[idx]
            c = pcap.CLASS_NAMES[int(o['klass'])] if int(o['klass']) < len(pcap.CLASS_NAMES) else '?'
            sl = dig(fr.slot_bytes(d))
            vsb, psb = dig(fr.bank(int(d['vs_bank'])).tobytes()), dig(fr.bank(int(d['ps_bank'])).tobytes())
            cur = dict(sh=(int(d['vs_hash']), int(d['ps_hash']), int(d['vs_patched'])), pr=(int(d['source_prim']), int(d['flags']) & 1), fx=int(d['fixed']), dy=int(d['dynamic']),
                       lay=int(d['layout']), bl=int(d['bool_loop']), sl=sl, vb=vsb, pb=psb, geo=(int(d['vertex_offset']), int(d['index_offset']), int(d['vertex_bytes']), int(d['index_bytes'])), inline=int(d['flags']) & 1)
            for bucket in (tot, cls[c]):
                bucket['draws'] += 1
                if prev is not None:
                    bucket['pairs'] += 1
                    same_sh = cur['sh'] == prev['sh']
                    same_lay = same_sh and cur['lay'] == prev['lay'] and cur['pr'] == prev['pr']
                    same_state = same_lay and cur['fx'] == prev['fx'] and cur['dy'] == prev['dy'] and cur['bl'] == prev['bl']
                    same_all = same_state and cur['sl'] == prev['sl']
                    bucket['shader_pair'] += same_sh
                    bucket['sh_layout'] += same_lay
                    bucket['state_noslots'] += same_state
                    bucket['state_same'] += same_all
                    bucket['state_same_nogap'] += same_all and not gap
                    if same_all:
                        bucket['ss_vbank_eq'] += cur['vb'] == prev['vb']
                        bucket['ss_pbank_eq'] += cur['pb'] == prev['pb']
                        bucket['ss_banks_eq'] += cur['vb'] == prev['vb'] and cur['pb'] == prev['pb']
                        if not cur['inline']:
                            bucket['ss_indexed'] += 1
                            bucket['ss_geo_same_vb'] += cur['geo'][0] == prev['geo'][0] and cur['geo'][2] == prev['geo'][2]
                            bucket['ss_geo_same_ib'] += cur['geo'][1] == prev['geo'][1] and cur['geo'][3] == prev['geo'][3]
                        else:
                            bucket['ss_inline'] += 1
            prev, gap = cur, False
    return hi - lo, tot, cls


def pct(x, n):
    return '%5.1f%%' % (100.0 * x / n) if n else '  n/a '


def show(name, frames, tot, cls):
    print('== %s: %d frames, %d draws (%.0f a frame), %d consecutive pairs' % (name, frames, tot['draws'], tot['draws'] / max(frames, 1), tot['pairs']))
    hdr = '%-11s %8s %7s %7s %7s %7s %7s %7s | of state-same: %7s %7s %7s %7s'
    print(hdr % ('class', 'pairs', 'shader', 'sh+lay', '+fx/dy/bl', '+slots', 'nogap', 'draws%', 'vbank=', 'pbank=', 'both=', 'geo=(ib)'))
    rows = sorted(cls.items(), key=lambda kv: -kv[1]['pairs']) + [('ALL', tot)]
    for c, t in rows:
        p = t['pairs']
        if not p:
            continue
        ss = t['state_same']
        print('%-11s %8d %7s %7s %7s %7s %7s %7s | %24s %7s %7s %7s' % (c, p, pct(t['shader_pair'], p), pct(t['sh_layout'], p), pct(t['state_noslots'], p), pct(ss, p), pct(t['state_same_nogap'], p),
              pct(t['draws'], tot['draws']), pct(t['ss_vbank_eq'], ss), pct(t['ss_pbank_eq'], ss), pct(t['ss_banks_eq'], ss), pct(t['ss_geo_same_ib'], t['ss_indexed'])))


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    a = b = None
    name = os.path.basename(args[0])
    if '--frames' in args:
        k = args.index('--frames')
        a, b = [int(x) for x in args[k + 1].split(':')]
        del args[k:k + 2]
    if '--name' in args:
        k = args.index('--name')
        name = args[k + 1]
        del args[k:k + 2]
    try:
        frames, tot, cls = census(args[0], a, b)
    except Exception as e:
        print('FAIL: %s' % e)
        return 1
    if not tot['draws']:
        print('FAIL: no draws read')
        return 1
    show(name, frames, tot, cls)
    return 0


if __name__ == '__main__':
    sys.exit(main())
