"""Lane 117: raw vertex floats + layout of selected draws of one frame; and the PS-bank vec4 registers that differ from the frame's other banks. 0 game runs."""
import sys, os, struct
sys.path.insert(0, os.path.dirname(__file__))
from caw_text_read import frame, get_name, origins, expand_banks

cap, fr = sys.argv[1], sys.argv[2]
want = [int(x) for x in sys.argv[3:]]
b, gf, secs, S, runs = frame(os.path.join(cap, 'frames', 'frame_%06d.bin' % int(fr)))
geom = open(os.path.join(cap, 'geometry.bin'), 'rb').read()
banks = expand_banks(b, secs)
des, dc, do = secs[4]
lay = [struct.unpack_from('<IIII', b, secs[11][2] + i * secs[11][0]) for i in range(secs[11][1])]
els = [struct.unpack_from('<HHBBBBi', b, secs[12][2] + i * secs[12][0]) for i in range(secs[12][1])]
for i in want:
    o = do + i * des
    (vh, ph, vph, fx, dy, tf, tc, vb, pb, bl, lo, vo, io, vbytes, ib, vc, ic) = struct.unpack_from('<QQQIIIIIIIIQQIIII', b, o)
    L = lay[lo]
    print('draw', i, 'vs', hex(vh), 'ps', hex(ph), 'vb', vb, 'pb', pb, 'stride', L[2], 'elements', [(e[2], e[3], e[0], e[1]) for e in els[L[0]:L[0] + L[1]]])
    v = geom[vo:vo + vbytes]
    for k in range(vc):
        print('   ', struct.unpack_from('<%df' % (L[2] // 4), v, k * L[2]))
    pbank = banks[pb]
    nz = [(r, struct.unpack_from('<4f', pbank, r * 16)) for r in range(256) if any(pbank[r * 16:r * 16 + 16])]
    print('   ps bank nonzero regs (first 12):', nz[:12])
