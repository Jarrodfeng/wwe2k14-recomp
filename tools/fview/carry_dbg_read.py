"""Session 101 lane 210: reads carry_dbg.py's pictures. A skin pixel = a pixel of the wrestler's mask where the calibration variant 7 (constant 0.25, 0.5, 0.75)
reads (64, 127, 191) within 1 level (so gear / cloth / blended edge pixels drop out). Prints, per variant and group, mean / p10 / p50 / p90 of the 3 channels over those pixels,
and the share of skin pixels per group.   py -I tools\\fview\\carry_dbg_read.py DIR(the --out of carry_dbg) FRAME [variants]"""
import json, pathlib, sys
import numpy as np
from PIL import Image
d = pathlib.Path(sys.argv[1]) / 'dbg'; f = int(sys.argv[2]); vs = (sys.argv[3] if len(sys.argv) > 3 else '1,2,3,4,5,6').split(',')
ld = lambda n: np.asarray(Image.open(d / ('f%d_%s' % (f, n)) / 'picture.png').convert('RGB')).astype(np.float64)
A, M0, M1, V7 = ld('A'), ld('M0'), ld('M1'), ld('v7')
cal = np.abs(V7 - np.array([64, 127, 191])).max(axis=2) <= 1
res = {}
for g, mk in ((0, M0), (1, M1)):
    m = (np.abs(mk - A).max(axis=2) > 10) & cal
    print('group', g, 'skin px', int(m.sum()))
    for v in vs:
        px = ld('v' + v)[m]
        q = lambda p: [round(float(x), 1) for x in np.percentile(px, p, axis=0)]
        res['v%s_g%d' % (v, g)] = {'mean': [round(float(x), 1) for x in px.mean(axis=0)], 'p10': q(10), 'p50': q(50), 'p90': q(90)}
        print(' v' + v, json.dumps(res['v%s_g%d' % (v, g)]))
