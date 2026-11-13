"""Session 100: prints the draws of one capture frame whose pixel shader is in a list: blend, viewport / scissor, ndc mapping, texture slots
(image, view format, swizzle, sign) and chosen constants (constant banks expanded from BankDef / BankPatch).
usage: py tools/renderer/cap_draws.py CAPTURE_DIR FRAME PSHASH[,PSHASH] [PS_REGS e.g. 0-4] [VS_REGS]"""
import struct, sys, pathlib
cap = pathlib.Path(sys.argv[1]); fi = int(sys.argv[2]); want = {int(h, 16) for h in sys.argv[3].split(',')}
def rng(s):
    a, b = s.split('-'); return range(int(a), int(b) + 1)
psr = rng(sys.argv[4]) if len(sys.argv) > 4 else range(0, 8)
vsr = rng(sys.argv[5]) if len(sys.argv) > 5 else range(0, 0)
b = cap.joinpath('frames', f'frame_{fi:06d}.bin').read_bytes()
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
    base, full, pf, pc = struct.unpack_from('<IIII', b, rec(17, i))
    if base == 0xFFFFFFFF:
        d = list(struct.unpack_from('<1024I', b, banks_so + banks_es * full))
    else:
        d = list(bank(base))
    for p in range(pf, pf + pc):
        o = rec(18, p); blk = struct.unpack_from('<I', b, o)[0]
        d[16 * blk:16 * blk + 16] = struct.unpack_from('<16I', b, o + 4)
    defs[i] = d; return d
def vec(d, r):
    return tuple(round(struct.unpack('<f', struct.pack('<I', d[4 * r + k]))[0], 4) for k in range(4))
es, cnt, so = S[4]
for i in range(cnt):
    o = so + es * i
    vs, ps, vsp, fixed, dyn, tf, tc, vb, pb = struct.unpack_from('<QQQIIIIII', b, o)
    if ps not in want: continue
    print(f'draw {i}: ps {ps:016X} vs {vs:016X}')
    fo = rec(7, fixed)
    blend = struct.unpack_from('<8B', b, fo + 24)
    wm = blend[7]
    print(f'  blend0 enable {blend[0]} src_color {blend[1]} dst_color {blend[2]} op {blend[3]} src_a {blend[4]} dst_a {blend[5]} op_a {blend[6]} mask {wm:04b}')
    do = rec(8, dyn)
    vp = struct.unpack_from('<6f', b, do); sc_ = struct.unpack_from('<iiII', b, do + 24)
    print(f'  viewport {tuple(round(v,1) for v in vp[:4])} scissor {sc_}')
    gso = do + 136
    nd = struct.unpack_from('<12f', b, gso)
    print(f'  ndc_scale {nd[0:3]} ndc_offset {nd[3:6]} guest_scale {nd[6:9]} guest_offset {nd[9:12]}')
    for t in range(tf, tf + tc):
        to = rec(10, t)
        image, sampler, slot, vf = struct.unpack_from('<IIBB', b, to)
        sw = struct.unpack_from('<4B', b, to + 10); sg = struct.unpack_from('<4B', b, to + 14)
        print(f'  slot {slot}: image {image} view {vf} swizzle {sw} sign {sg}')
    d = bank(pb)
    for r in psr: print(f'  ps c{r} = {vec(d, r)}')
    dv = bank(vb)
    for r in vsr: print(f'  vs c{r} = {vec(dv, r)}')
