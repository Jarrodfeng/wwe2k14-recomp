"""lane 177 (Session 101): sibling audit of the full-view rule. Lists, over kept captures, every four-vertex quad-list draw (prim 13: the game's T_Rect, stride 12, and
T_Sprite, stride 20) whose corners reach >= 90 % of the view in width OR height, grouped by (shape, VS, PS, covers), with what the rule does with it:
  FULL      reaches all four edges, a flat rectangle (stride 12): left unsqueezed (the lane 177 rule)
  sprite    a textured quad (stride 20) that reaches all four edges: NOT touched (a picture is a picture: it keeps the squeeze rule), listed for the census
  bar       reaches the full width or height but not both: not touched
usage: py -I tools\\renderer\\fullview_audit.py CAPTURE_DIR [CAPTURE_DIR ...]   (prints one block per capture)"""
import struct, sys; sys.setrecursionlimit(20000)
import struct, sys, pathlib, collections


def audit(cap):
    cap = pathlib.Path(cap)
    geo = cap.joinpath('geometry.bin').open('rb')
    res = collections.OrderedDict()
    frames = sorted(cap.joinpath('frames').glob('frame_*.bin'))
    for p in frames:
        fi = int(p.stem.split('_')[1])
        b = p.read_bytes()
        _, _, cf, sc, gf, pi, fl = struct.unpack_from('<IIIIQII', b, 0)
        S = {}
        for s in range(sc):
            sid, es, cnt, so = struct.unpack_from('<IIQQ', b, 32 + 24 * s); S[sid] = (es, cnt, so)
        if 4 not in S: continue
        def rec(sid, i):
            es, cnt, so = S[sid]; return so + es * i
        banks_es, _, banks_so = S.get(13, (4096, 0, 0))
        defs = {}
        def bank(i):
            if i in defs: return defs[i]
            if 17 not in S or S[17][1] == 0:
                defs[i] = list(struct.unpack_from('<1024I', b, banks_so + banks_es * i)); return defs[i]
            base, full, pf, pc = struct.unpack_from('<IIII', b, rec(17, i))
            d = list(struct.unpack_from('<1024I', b, banks_so + banks_es * full)) if base == 0xFFFFFFFF else list(bank(base))
            for q in range(pf, pf + pc):
                o = rec(18, q); blk = struct.unpack_from('<I', b, o)[0]
                d[16 * blk:16 * blk + 16] = struct.unpack_from('<16I', b, o + 4)
            defs[i] = d; return d
        f32 = lambda w: struct.unpack('<f', struct.pack('<I', w))[0]
        es, cnt, so = S[4]
        for i in range(cnt):
            o = so + es * i
            vs, ps, vsp, fixed, dyn, tf, tc, vb, pb, bl, layout = struct.unpack_from('<QQQIIIIIIII', b, o)
            voff, ioff, vbytes, ibytes, vcount, icount = struct.unpack_from('<QQIIII', b, o + 56)
            idx32, prim, flags = struct.unpack_from('<BBB', b, o + 88)
            if prim != 13 or vcount != 4 or layout == 0xFFFFFFFF or flags & 8: continue
            ef, ec, stride, gs = struct.unpack_from('<IIII', b, rec(11, layout))
            if stride not in (12, 20): continue
            geo.seek(voff); raw = geo.read(4 * stride)
            if len(raw) < 4 * stride: continue
            xs = [struct.unpack_from('<f', raw, stride * k)[0] for k in range(4)]
            ys = [struct.unpack_from('<f', raw, stride * k + 4)[0] for k in range(4)]
            try:
                dv = bank(vb)
            except RecursionError:
                continue
            px, py_ = f32(dv[0]), f32(dv[1])
            if not (px > 0 and py_ > 0): continue
            W, H = 2 / px, 2 / py_
            x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
            fw, fh = (x1 - x0) / W, (y1 - y0) / H
            if not (fw >= 0.9 or fh >= 0.9) or not (fw > 0 and fh > 0): continue
            full = x0 <= 1.5 and x1 >= W - 0.5 and y0 <= 1.5 and y1 >= H - 0.5
            kind = ('FULL' if stride == 12 else 'sprite') if full else 'bar'
            key = (kind, 'stride%d' % stride, '%016X' % vs, '%016X' % ps, round(fw, 2), round(fh, 2))
            res.setdefault(key, []).append(fi)
    return res, len(frames)


if __name__ == '__main__':
    for c in sys.argv[1:]:
        res, n = audit(c)
        print('== %s (%d frames)' % (c, n))
        for k, fr in res.items():
            print('  %-6s %s VS %s PS %s width %.0f%% height %.0f%%: %d draws, frames %d..%d' % (k[0], k[1], k[2], k[3], 100 * k[4], 100 * k[5], len(fr), fr[0], fr[-1]))
