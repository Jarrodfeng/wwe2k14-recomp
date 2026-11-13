"""lane 177: survey of T_Rect-shaped draws (prim 13, 4 vertices of 12 bytes, 6 indices) over a frame range of a capture: distinct (vs, ps, bbox, colour) with the frames
they occur in, and whether the quad covers the view (clip-space corners from the stream's own placement: clip = (xy - 0.5) * PosScale - 1 ... see UI_QUADS_OURS.md).
usage: py -I tools/renderer/rect_survey.py CAPTURE_DIR FIRST LAST"""
import struct, sys; sys.setrecursionlimit(20000)
import struct, sys, pathlib, collections
cap = pathlib.Path(sys.argv[1]); f0 = int(sys.argv[2]); f1 = int(sys.argv[3])
geo = cap.joinpath('geometry.bin').open('rb')
res = collections.OrderedDict()
for fi in range(f0, f1 + 1):
    p = cap.joinpath('frames', f'frame_{fi:06d}.bin')
    if not p.exists(): continue
    b = p.read_bytes()
    _, _, cf, sc, gf, pi, fl = struct.unpack_from('<IIIIQII', b, 0)
    S = {}
    for s in range(sc):
        sid, es, cnt, so = struct.unpack_from('<IIQQ', b, 32 + 24 * s); S[sid] = (es, cnt, so)
    def rec(sid, i):
        es, cnt, so = S[sid]; return so + es * i
    banks_es, _, banks_so = S.get(13, (4096, 0, 0))
    defs = {}
    def bank(i):
        if i in defs: return defs[i]
        if 17 not in S or S[17][1] == 0:  # older captures: no bank defs, the index is the full bank
            defs[i] = list(struct.unpack_from('<1024I', b, banks_so + banks_es * i)); return defs[i]
        base, full, pf, pc = struct.unpack_from('<IIII', b, rec(17, i))
        d = list(struct.unpack_from('<1024I', b, banks_so + banks_es * full)) if base == 0xFFFFFFFF else list(bank(base))
        for q in range(pf, pf + pc):
            o = rec(18, q); blk = struct.unpack_from('<I', b, o)[0]
            d[16 * blk:16 * blk + 16] = struct.unpack_from('<16I', b, o + 4)
        defs[i] = d; return d
    def vec(d, r): return tuple(round(struct.unpack('<f', struct.pack('<I', d[4 * r + k]))[0], 4) for k in range(4))
    es, cnt, so = S[4]
    for i in range(cnt):
        o = so + es * i
        vs, ps, vsp, fixed, dyn, tf, tc, vb, pb, bl, layout = struct.unpack_from('<QQQIIIIIIII', b, o)
        voff, ioff, vbytes, ibytes, vcount, icount = struct.unpack_from('<QQIIII', b, o + 56)
        idx32, prim, flags = struct.unpack_from('<BBB', b, o + 88)
        if prim != 13 or vcount != 4 or icount != 6 or layout == 0xFFFFFFFF: continue
        ef, ec, stride, gs = struct.unpack_from('<IIII', b, rec(11, layout))
        if stride != 12 or ec != 1: continue
        geo.seek(voff); raw = geo.read(48)
        v = [struct.unpack_from('<3f', raw, 12 * k) for k in range(4)]
        dv = bank(vb)
        c0, c1, c28 = vec(dv, 0), vec(dv, 1), vec(dv, 28)
        xs = [q[0] for q in v]; ys = [q[1] for q in v]
        key = (f'{vs:016X}', f'{ps:016X}', min(xs), max(xs), min(ys), max(ys), c0[:2], c1, c28[0])
        res.setdefault(key, []).append(fi)
for k, fr in res.items():
    print('vs %s ps %s x %g..%g y %g..%g PosScale %s Color %s fUseProj %g : %d draws, frames %d..%d' % (*k, len(fr), fr[0], fr[-1]))
