"""Lane 124: what the rim light map's draws are (site 0x82BFBD58): objects, parts, layouts, and whether the same (object, part) has a shadow-map-site draw with identical bytes. Numbers only."""
import sys, os, collections
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'shadow'))
import shadow_census as sc
import numpy as np
path = sys.argv[1]; frames = [int(x) for x in sys.argv[2].split(',')]
cap = sc.Cap(path)
for fi in frames:
    f = cap.frame(fi); d = f.draws; o = f.origins
    ps = o['pass_site'].astype(np.int64); ob = o['object'].astype(np.int64); pt = sc.pcap.queue_part(o); kl = o['klass']
    rim = np.nonzero(ps == sc.SITE_RIM)[0]
    mp = np.nonzero(ps == sc.SITE_MAP)[0]
    col = np.nonzero((ps == sc.SITE_COLOUR) | (ps == sc.SITE_MIRROR))[0]
    print('frame', fi, 'rim draws', len(rim), 'map draws', len(mp), 'colour', len(col))
    mapkey = {}
    for i in mp: mapkey.setdefault((int(ob[i]), int(pt[i])), []).append(int(i))
    colkey = {}
    for i in col: colkey.setdefault((int(ob[i]), int(pt[i])), []).append(int(i))
    by = collections.Counter()
    for i in rim:
        dr = d[i]; stride, els = sc.layout_of(f, dr)
        key = (int(ob[i]), int(pt[i]))
        rows = sc.vertex_rows(cap, dr, stride).tobytes()
        same_map = any(sc.vertex_rows(cap, d[j], sc.layout_of(f, d[j])[0]).tobytes() == rows for j in mapkey.get(key, []))
        same_col = any(sc.layout_of(f, d[j])[0] == stride and sc.vertex_rows(cap, d[j], stride).tobytes() == rows for j in colkey.get(key, []))
        by[('%x' % key[0], 'klass%d' % int(kl[i]), stride, tuple(e[0] for e in els), 'inmap' if key in mapkey else 'nomap', 'bytes=map' if same_map else '-', 'bytes=col' if same_col else '-', 'incol' if key in colkey else 'nocol')] += 1
    for k, v in sorted(by.items()): print(v, k)
    print('rim eff', collections.Counter(cap.effs(d['vs_hash'][rim])))
