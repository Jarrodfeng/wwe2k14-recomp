"""Lane 117: list a frame's 2D-class draws in order: index, class, technique, bbox of first quad, ps bank hash, text run. 0 game runs."""
import sys, os, struct, hashlib
sys.path.insert(0, os.path.dirname(__file__))
from caw_text_read import frame, get_name, origins, expand_banks

CLS = ["unnamed", "model_part", "crowd", "sprite", "text", "movie", "particle", "shadow", "post", "clear", "other"]


def table(cap, fr):
    b, gf, secs, S, runs = frame(os.path.join(cap, 'frames', 'frame_%06d.bin' % int(fr)))
    geom = open(os.path.join(cap, 'geometry.bin'), 'rb').read()
    des, dc, do = secs[4]
    lay = [struct.unpack_from('<IIII', b, secs[11][2] + i * secs[11][0]) for i in range(secs[11][1])]
    bes, bn, bo = secs[13]
    runmap = {d: get_name(S, s) for d, s, u, f in runs}
    org = origins(b, secs, S)
    banks = expand_banks(b, secs)
    out = []
    for i in range(dc):
        o = do + i * des
        (vh, ph, vph, fx, dy, tf, tc, vb, pb, bl, lo, vo, io, vbytes, ib, vc, ic) = struct.unpack_from('<QQQIIIIIIIIQQIIII', b, o)
        k = org[i]
        cl = CLS[k[0]] if k[0] < len(CLS) else '?'
        if cl not in ('sprite', 'text', 'other', 'unnamed'):
            continue
        pos = None
        if lo != 0xFFFFFFFF and vc:
            stride = lay[lo][2]
            v = geom[vo:vo + vbytes]
            xs = []; ys = []
            for q in range(min(vc, 4)):
                f = struct.unpack_from('<3f', v, q * stride)
                xs.append(f[0]); ys.append(f[1])
            pos = (round(min(xs), 1), round(min(ys), 1), round(max(xs), 1), round(max(ys), 1))
        bank = banks[pb]
        bh = hashlib.md5(bank).hexdigest()[:6]
        out.append(dict(i=i, cls=cl, tech=get_name(S, k[7]), pos=pos, bank=pb, bh=bh, vs=vh, ps=ph, run=runmap.get(i), tf=tf, tc=tc,
                        widget=get_name(S, k[9]), bankbytes=bank))
    return out, b, secs, S


if __name__ == '__main__':
    out, b, secs, S = table(sys.argv[1], sys.argv[2])
    lo = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    hi = int(sys.argv[4]) if len(sys.argv) > 4 else 10**9
    last = None
    for d in out:
        if not lo <= d['i'] <= hi:
            continue
        key = (d['cls'], d['tech'], d['run'], d['bh'])
        if d['cls'] == 'text' and last and last[0] == key[0] and last[2] == key[2]:
            continue
        last = key
        print(d['i'], d['cls'], d['tech'], d['pos'], 'bank', d['bank'], d['bh'], hex(d['ps'] & 0xffffffff), repr(d['run']), 'tex', d['tc'], d['widget'])
