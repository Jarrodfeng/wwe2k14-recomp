"""Lane 117: which vec4 registers of the VS/PS constant banks differ between the given draws of one frame (the highlight colour hunt). 0 game runs."""
import sys, os, struct
sys.path.insert(0, os.path.dirname(__file__))
from caw_text_read import frame, expand_banks

cap, fr = sys.argv[1], sys.argv[2]
ds = [int(x) for x in sys.argv[3:]]
b, gf, secs, S, runs = frame(os.path.join(cap, 'frames', 'frame_%06d.bin' % int(fr)))
banks = expand_banks(b, secs)
des, dc, do = secs[4]
info = []
for i in ds:
    o = do + i * des
    t = struct.unpack_from('<QQQIIIIIIIIQQIIII', b, o)
    info.append((i, banks[t[7]], banks[t[8]]))
for which, nm in ((1, 'VS'), (2, 'PS')):
    print(nm)
    for r in range(256):
        vals = [struct.unpack_from('<4f', x[which], r * 16) for x in info]
        if len(set(vals)) > 1:
            print('  reg', r, [tuple(round(f, 4) for f in v) for v in vals])
