"""Lane 151: the rim chain over EVERY frame of a capture: which frames carry it, its pass order, the target / input images and sizes, whether the stream's constants / geometry ever change, who reads 223 in slot 15.
  py -I tools\rim\rim_chain_census.py CAPTURE_DIR [STEP]  (numbers only; reads the capture, no game)"""
import sys, os, collections, hashlib
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'shadow'))
import shadow_census as sc
import numpy as np

cap = sc.Cap(sys.argv[1]); step = int(sys.argv[2]) if len(sys.argv) > 2 else 1
sig = collections.Counter(); targets = collections.Counter(); inputs = collections.Counter(); consts = collections.defaultdict(collections.Counter); geom = collections.defaultdict(collections.Counter)
readers = collections.Counter(); nframes = 0; with_chain = 0; clears = collections.Counter(); alpha = collections.Counter(); samplers = collections.Counter()
for fi in range(0, len(cap.frames), step):
    f = cap.frame(fi)
    if not f.has_origins: continue
    nframes += 1
    d = f.draws; o = f.origins; cs = sc.pcap.queue_site(o); ps = o['pass_site'].astype(np.int64)
    ve, pe = cap.effs(d['vs_hash']), cap.effs(d['ps_hash'])
    copies = sc.table(f, sc.SEC_COPIES, sc.COPY_DT)
    kinds = []
    for p in sc.walk(f, copies):
        if not p.draws: continue
        ix = np.array(p.draws)
        if (ps[ix] == sc.SITE_RIM).any(): k = 'rim'
        elif ((cs[ix] == sc.CALL_RIMRED) & (pe[ix] == 'yBlur/flag_any')).any(): k = 'red'
        elif (cs[ix] == sc.CALL_MINBLUR).any() and pe[ix][cs[ix] == sc.CALL_MINBLUR][0] == 'yBlur/t2': k = 't2'
        else: continue
        kinds.append(k)
        t = cap.imgs[p.target]
        targets[(k, p.target, int(t['w']), int(t['h']), int(t['format']))] += 1
        clears[(k, bool(p.clear), len(p.draws), len(p.copies))] += 1
        if k == 'rim': continue
        for di in p.draws:
            dr = d[di]
            fx = f.raw[sc.capread.SEC['fixeds']][dr['fixed']]
            alpha[(k, 'alpha_test_byte', fx[8 * 0 + 0:0 + 0] if False else None)] += 0
            for s in f.slots[dr['tex_first']:dr['tex_first'] + dr['tex_count']]:
                inputs[(k, int(s['slot']), int(s['image']))] += 1
                samplers[(k, int(s['slot']), f.raw[sc.capread.SEC['samplers']][s['sampler']].hex())] += 1
            nreg = 4 if k == 'red' else 13
            b = f.bank(dr['ps_bank']); consts[k][hashlib.md5(b[0:4 * nreg].tobytes() + b[64:64 + 4 * nreg].tobytes()).hexdigest()[:8]] += 1
            geom[k][(int(dr['vertex_count']), int(dr['index_count']), hashlib.md5(f.vertices(dr).tobytes()).hexdigest()[:8])] += 1
    sig['>'.join(kinds)] += 1
    if 'red' in kinds: with_chain += 1
    for j in range(len(d)):
        for s in f.slots[d[j]['tex_first']:d[j]['tex_first'] + d[j]['tex_count']]:
            if s['slot'] == 15: readers[int(s['image'])] += 1
print('frames with origins', nframes, 'with the reductions', with_chain)
print('chain signatures', dict(sig))
for k, v in sorted(targets.items()): print('target', k, v)
for k, v in sorted(clears.items()): print('pass (kind, clear, draws, copies)', k, v)
for k, v in sorted(inputs.items()): print('draw input (kind, slot, image)', k, v)
for k, v in sorted(samplers.items()): print('sampler (kind, slot, bytes)', k, v)
for k in consts: print('constants distinct', k, len(consts[k]), consts[k].most_common(3))
for k in geom: print('geometry (verts, idx, md5)', k, dict(geom[k]))
print('slot 15 readers by image', dict(readers))
