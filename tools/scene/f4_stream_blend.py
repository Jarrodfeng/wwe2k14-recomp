"""S103 F4: the blend[0] state, slot-0 image and draw count of every particle draw (origin klass 6) per frame of a capture (numbers only). py -I tools/scene/f4_stream_blend.py CAPTURE [STEP], from the repo root."""
import sys, collections
sys.path.insert(0, 'tools/particles'); sys.path.insert(0, 'tools/solver')
import numpy as np, pcap
cap = pcap.Capture(sys.argv[1])
print('frames', len(cap.frames))
step = int(sys.argv[2]) if len(sys.argv) > 2 else 1
tot = collections.Counter(); perframe = []
for i in range(0, len(cap.frames), step):
    f = cap.frame(i)
    if not f.has_origins: print('no origins'); break
    fx = f.raw[pcap.SEC['fixeds']]
    n = 0; st = collections.Counter(); imgs = set()
    for di in range(len(f.draws)):
        o = f.origins[di]
        if int(o['klass']) != 6: continue
        d = f.draws[di]
        b = fx[int(d['fixed'])]
        bl = tuple(b[24:32])  # blend[0]
        st[bl] += 1; n += 1
        for t in range(int(d['tex_first']), int(d['tex_first']) + int(d['tex_count'])):
            if f.slots[t]['slot'] == 0: imgs.add(int(f.slots[t]['image']))
    if n:
        perframe.append((i, f.guest_frame, n, dict(st), sorted(imgs)))
        tot.update(st)
print('blend[0] (enable src_c dst_c op src_a dst_a aop mask) counts:', dict(tot))
for r in perframe[:12] + perframe[-4:]: print(r)
print('particle frames', len(perframe), 'guest range', perframe[0][1] if perframe else None, perframe[-1][1] if perframe else None)
