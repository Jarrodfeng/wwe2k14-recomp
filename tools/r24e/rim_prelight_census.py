r"""S102 lane R24-E: how often are the rim chain's inputs and the pre-light bake's constants UNCHANGED from the previous frame? (numbers only; reads a capture, no game)
  py -I tools\r24e\rim_prelight_census.py CAPTURE_DIR [--max N]
Rim: per frame, a hash of every rim-site draw's (vertex bank, pixel bank) in order (the bone palette rows are in the vertex bank). Pre-light: the bake draws' (call site 0x82C20B90) pixel bank (the lights) and vertex bank.
A frame is 'same' when its hash equals the previous frame's."""
import sys, os, hashlib
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'shadow'))
import shadow_census as sc
import numpy as np

CALL_PRELIGHT = 0x82C20B90
cap = sc.Cap(sys.argv[1])
mx = int(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else len(cap.frames)
prev = {}
stat = {k: dict(chain=0, same=0, frames=0, longest=0, cur=0) for k in ('rim', 'light')}
for fi in range(min(mx, len(cap.frames))):
    f = cap.frame(fi)
    if not f.has_origins:
        continue
    d, o = f.draws, f.origins
    cs = sc.pcap.queue_site(o)
    ps = o['pass_site'].astype(np.int64)
    sel = {'rim': np.nonzero(ps == sc.SITE_RIM)[0], 'light': np.nonzero(cs == CALL_PRELIGHT)[0]}
    for k, ix in sel.items():
        s = stat[k]
        s['frames'] += 1
        if len(ix) == 0:
            prev[k] = None
            s['cur'] = 0
            continue
        h = hashlib.md5()
        for di in ix:
            dr = d[di]
            h.update(bytes(f.bank(dr['vs_bank']).tobytes()))
            h.update(bytes(f.bank(dr['ps_bank']).tobytes()))
        hv = h.hexdigest()
        s['chain'] += 1
        if prev.get(k) == hv:
            s['same'] += 1
            s['cur'] += 1
            s['longest'] = max(s['longest'], s['cur'])
        else:
            s['cur'] = 0
        prev[k] = hv
for k, s in stat.items():
    print('%s: frames %d, with the chain %d, input equal to the previous frame %d (%.1f%% of the frames with the chain), longest run of equal %d' % (k, s['frames'], s['chain'], s['same'], 100.0 * s['same'] / max(1, s['chain']), s['longest']))
