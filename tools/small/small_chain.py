"""Lane 135: the copy chain around the small passes: for a frame, the copies whose source is a small-pass target (and where they go), and who samples the destinations.
  py -I tools\\small\\small_chain.py CAPTURE FRAME"""
import collections
import os
import sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'shadow'))
import numpy as np
import shadow_census as sc

cap = sc.Cap(sys.argv[1])
fi = int(sys.argv[2])
f = cap.frame(fi)
d, o = f.draws, f.origins
cs = o['call_site'].astype(np.int64)
copies = sc.table(f, sc.SEC_COPIES, sc.COPY_DT)
passes = sc.walk(f, copies)
small = {0x8279A19C, 0x82799CB4, 0x82799DC8, 0x82C20B90}
targets = set()
for p in passes:
    if any(int(cs[i]) in small for i in p.draws):
        targets.add(p.target)
print('small-pass targets', sorted(targets))
dests = set()
for k, c in enumerate(copies):
    s, dd = int(c['s']['image']), int(c['d']['image'])
    if s in targets or dd in targets:
        im = cap.imgs
        print('copy', k, 'src', s, (int(im[s]['w']), int(im[s]['h'])) if s in im else None, 'mip', int(c['s']['mip']), 'layer', int(c['s']['layer']), '->', dd, (int(im[dd]['w']), int(im[dd]['h']), int(im[dd]['d']), int(im[dd]['layers'])) if dd in im else None, 'layer', int(c['d']['layer']), 'srect', c['sr'].tolist(), 'drect', c['dr'].tolist())
        dests.add(dd)
        if s in targets:
            dests.add(dd)
# the images the small passes SAMPLE
ins = set()
for p in passes:
    for i in p.draws:
        if int(cs[i]) in small:
            for s in f.slots[int(d[i]['tex_first']):int(d[i]['tex_first']) + int(d[i]['tex_count'])]:
                ins.add(int(s['image']))
print('images the passes sample', sorted(ins))
watch = dests | ins | targets
tf, tc = d['tex_first'].astype(np.int64), d['tex_count'].astype(np.int64)
readers = collections.Counter()
for i in range(len(d)):
    for s in f.slots[int(tf[i]):int(tf[i]) + int(tc[i])]:
        if int(s['image']) in watch:
            readers[(int(s['image']), int(s['slot']), cap.eff(int(d[i]['vs_hash'])).split('/')[0] + '/' + cap.eff(int(d[i]['ps_hash'])).split('/')[-1])] += 1
for k, v in sorted(readers.items()):
    print('reader image %d slot %d %s x%d' % (k[0], k[1], k[2], v))
for im in sorted(watch):
    if im in cap.imgs:
        r = cap.imgs[im]
        print('image', im, 'w', int(r['w']), 'h', int(r['h']), 'layers', int(r['layers']), 'mips', int(r['mips']), 'format', int(r['format']), 'usage', int(r['usage']), 'source', int(r['source']), 'role', int(r['role']), 'flags', int(r['flags']))
