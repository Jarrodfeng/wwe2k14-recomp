"""lane 177 (Session 101): prints the vertex bytes of draws (by PS hash and optionally VS hash) of one capture frame, with the layout's elements and the
vertex shader constants c0-c3, so a full-screen quad's corners can be read. Offline, 0 game runs.
usage: py -I tools/renderer/quad_corners.py CAPTURE_DIR FRAME PSHASH[,..] [VSHASH]"""
import struct, sys; sys.setrecursionlimit(20000)
import struct, sys, pathlib
cap = pathlib.Path(sys.argv[1]); fi = int(sys.argv[2]); want = {int(h, 16) for h in sys.argv[3].split(',')}
vwant = int(sys.argv[4], 16) if len(sys.argv) > 4 else None
b = cap.joinpath('frames', f'frame_{fi:06d}.bin').read_bytes()
geo = cap.joinpath('geometry.bin')
_, _, cf, sc, gf, pi, fl = struct.unpack_from('<IIIIQII', b, 0)
S = {}
for s in range(sc):
    sid, es, cnt, so = struct.unpack_from('<IIQQ', b, 32 + 24 * s)
    S[sid] = (es, cnt, so)
def rec(sid, i):
    es, cnt, so = S[sid]; return so + es * i
banks_es, _, banks_so = S.get(13, (4096, 0, 0))
defs = {}
def bank(i):
    if i in defs: return defs[i]
    if 17 not in S or S[17][1] == 0:
        defs[i] = list(struct.unpack_from("<1024I", b, banks_so + banks_es * i)); return defs[i]
    base, full, pf, pc = struct.unpack_from('<IIII', b, rec(17, i))
    d = list(struct.unpack_from('<1024I', b, banks_so + banks_es * full)) if base == 0xFFFFFFFF else list(bank(base))
    for p in range(pf, pf + pc):
        o = rec(18, p); blk = struct.unpack_from('<I', b, o)[0]
        d[16 * blk:16 * blk + 16] = struct.unpack_from('<16I', b, o + 4)
    defs[i] = d; return d
def vec(d, r): return tuple(round(struct.unpack('<f', struct.pack('<I', d[4 * r + k]))[0], 5) for k in range(4))
es, cnt, so = S[4]
n = 0
for i in range(cnt):
    o = so + es * i
    vs, ps, vsp, fixed, dyn, tf, tc, vb, pb, bl, layout = struct.unpack_from('<QQQIIIIIIII', b, o)
    voff, ioff, vbytes, ibytes, vcount, icount = struct.unpack_from('<QQIIII', b, o + 56)
    idx32, prim, flags = struct.unpack_from('<BBB', b, o + 88)
    if ps not in want or (vwant is not None and vs != vwant): continue
    n += 1
    do = rec(8, dyn)
    vp = struct.unpack_from('<6f', b, do)
    nd = struct.unpack_from('<12f', b, do + 136)
    lo = rec(11, layout)
    ef, ec, stride, gstride = struct.unpack_from('<IIII', b, lo)
    els = []
    for e in range(ef, ef + ec):
        eo = rec(12, e); vkf, hoff, sf, comp, fl2, mini, goff = struct.unpack_from('<HHBBBBi', b, eo)
        els.append((vkf, hoff, sf, comp))
    dv = bank(vb)
    print(f'draw {i}: vs_patched {vsp:016X} prim {prim} flags {flags} vcount {vcount} icount {icount} stride {stride} vp {tuple(round(v) for v in vp[:4])} elements {els}')
    print(f'   vs c0 {vec(dv,0)} c1 {vec(dv,1)} c2 {vec(dv,2)} c3 {vec(dv,3)}  ndc_scale {tuple(round(v,4) for v in nd[0:3])}')
    with open(geo, 'rb') as g:
        g.seek(voff); raw = g.read(vbytes)
        for v in range(min(vcount, 6)):
            ch = raw[v * stride:(v + 1) * stride]
            print('   v%d: %s' % (v, ' '.join('%g' % round(x, 4) for x in struct.unpack('<%df' % (len(ch) // 4), ch))))
        if icount:
            g.seek(ioff); ib = g.read(ibytes)
            print('   idx:', struct.unpack('<%d%s' % (min(icount, 8), 'I' if idx32 else 'H'), ib[:min(icount, 8) * (4 if idx32 else 2)]))
print('matched', n)
