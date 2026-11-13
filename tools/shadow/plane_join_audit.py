r"""Lane 215: why do plane-chain draws stay the stream's (mismatch constants / late)? Offline, 0 game runs.

  py -I tools\shadow\plane_join_audit.py CASE_ZIP_OR_CAPTURE [--frames A:B]

Pools the capture's records (sections 18 PlaneFrame / 19 PlaneDraw of capture\scene\*.rsc) by their own stamp and, for every captured frame with header guest frame G, compares the stream's plane-chain
constants with the record entries of stamp G-1 (the live hook's join offset 1): per frame (the module's 10-float fade array as the sliding rows c0-c6, the fit c9-c12, the camera c13-c16, by BITS, so a NaN
that is bit-equal counts equal) and per blur draw (c36-c39 matrix; also c33.x, c35.x, c40). It prints, per kind of register group, how many frames/draws differ and, for the differing ones, the nearest stamp
(G-3..G+1) at which they ARE equal (the 'where did this value come from' answer) or 'nowhere'. Numbers only; nothing is written."""
import collections
import os
import struct
import sys
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import shadow_census as sc  # noqa: E402

SEC_PLANE_FRAMES, SEC_PLANE_DRAWS = 18, 19
FSZ, DSZ = 200, 160


def records(path):
    frames = collections.defaultdict(list)   # stamp -> [(fade[10], normal, fit[16], camera[16], have, file)]
    draws = collections.defaultdict(list)    # stamp -> [(kind, object, part, ordinal, plane_pos, blur_scale, blur_weight, blur_matrix[16], blur_point, have, file)]
    if os.path.isdir(path):
        d = path if os.path.isdir(os.path.join(path, 'scene')) else os.path.join(path, 'capture')
        names = sorted(os.path.join(d, 'scene', n) for n in os.listdir(os.path.join(d, 'scene')) if n.endswith('.rsc'))
        read = lambda n: open(n, 'rb').read()
    else:
        zf = zipfile.ZipFile(path)
        names = sorted(n for n in zf.namelist() if n.endswith('.rsc'))
        read = zf.read
    for n in names:
        b = read(n)
        if len(b) < 40 or b[:4] != b'RSCN':
            continue
        count = struct.unpack_from('<I', b, 8)[0]
        for i in range(count):
            sid, esz, cnt, off = struct.unpack_from('<IIQQ', b, 40 + 24 * i)
            if sid == SEC_PLANE_FRAMES and esz == FSZ:
                for e in range(cnt):
                    a = off + e * FSZ
                    ver, frame, flags, have = struct.unpack_from('<IIII', b, a)
                    f = np.frombuffer(b, '<f4', 46, a + 16)
                    frames[frame].append((f[0:10].copy(), f[10:14].copy(), f[14:30].copy(), f[30:46].copy(), have, n))
            elif sid == SEC_PLANE_DRAWS and esz == DSZ:
                for e in range(cnt):
                    a = off + e * DSZ
                    ver, frame, kind, flags, obj, part, have, ordinal = struct.unpack_from('<IIIIIIII', b, a)
                    f = np.frombuffer(b, '<f4', 28, a + 32)
                    draws[frame].append((kind, obj, part, ordinal, f[0:4].copy(), f[4:8].copy(), f[8:12].copy(), f[12:28].copy(), np.frombuffer(b, '<f4', 4, a + 144).copy(), have, n))
    return frames, draws


def bits(x):
    return np.ascontiguousarray(x, np.float32).view(np.uint32).tobytes()


def main():
    a = sys.argv[1:]
    path = a[0]
    lo = hi = None
    if '--frames' in a:
        p = a[a.index('--frames') + 1].split(':')
        lo, hi = int(p[0] or 0), int(p[1]) if p[1] else None
    fr, dr = records(path)
    print('plane frames stamps %d (%s..%s), draw stamps %d' % (len(fr), min(fr) if fr else '-', max(fr) if fr else '-', len(dr)))
    cap = sc.Cap(path)
    n = len(cap.frames)
    stat = collections.Counter()
    where = collections.Counter()
    shown = 0
    for fi in range(lo or 0, min(n, hi) if hi else n):
        f = cap.c.frame(fi)
        if not f.has_origins:
            continue
        d, o = f.draws, f.origins
        cs = sc.pcap.queue_site(o)
        ps = o['pass_site'].astype(np.int64)
        mi = np.nonzero(ps == sc.SITE_PLANE)[0]
        bi = np.nonzero(cs == sc.CALL_PBLUR)[0]
        if not len(mi) and not len(bi):
            continue
        G = int(f.guest_frame) & 0xFFFFFFFF
        stat['frames'] += 1
        notes = []
        if len(mi):
            vb = f.bank(int(d[mi[0]]['vs_bank']))
            rows = sc.regs(vb, 0, 17).astype('<f4')
            cand = {s: fr.get(s) for s in range(G - 3, G + 2)}
            fade_rows = np.array([rows[r] for r in range(7)])
            okset = {}
            for s, lst in cand.items():
                if not lst:
                    continue
                e = lst[-1]
                fd = e[0]
                eq_fade = all(bits(fade_rows[r]) == bits(fd[r:r + 4]) for r in range(7))
                eq_fit = bits(rows[9:13]) == bits(e[2])
                eq_cam = bits(rows[13:17]) == bits(e[3])
                okset[s] = (eq_fade, eq_fit, eq_cam)
            at = okset.get(G - 1)
            stat['mask frame groups'] += 1
            if at and all(at):
                stat['mask frame ok'] += 1
            else:
                stat['mask frame DIFF'] += 1
                eqs = [s - G for s, v in okset.items() if all(v)]
                where[('mask', tuple(eqs) if eqs else 'nowhere', 'stamp G-1 held' if at else 'stamp G-1 MISSING', tuple(i for i, v in enumerate(at or ()) if not v))] += 1
                notes.append('mask frame vs stamp G-1: fade/fit/camera equal = %s; equal at offsets %s' % (at, eqs))
        for bn, i in enumerate(bi):
            vb = f.bank(int(d[i]['vs_bank']))
            br = sc.regs(vb, 33, 8).astype('<f4')
            res = {}
            for s in range(G - 3, G + 2):
                bl = [x for x in dr.get(s, []) if x[0] == 1]
                if bn < len(bl):
                    res[s] = bl[bn]
            at = res.get(G - 1)
            stat['blur draws'] += 1
            ok = at is not None and bits(br[3:7]) == bits(at[7])
            if ok:
                stat['blur ok'] += 1
            else:
                stat['blur DIFF'] += 1
                eqs = [s - G for s, v in res.items() if bits(br[3:7]) == bits(v[7])]
                where[('blur', tuple(eqs) if eqs else 'nowhere', 'stamp G-1 held' if at is not None else 'stamp G-1 MISSING', ())] += 1
                notes.append('blur %d c36-39 vs stamp G-1 differ; equal at offsets %s' % (bn, eqs))
        if notes and shown < 40:
            shown += 1
            print('frame %d G %d: %s' % (fi, G, ' | '.join(notes)))
    print(dict(stat))
    for k, v in where.most_common():
        print('  ', k, v)


if __name__ == '__main__':
    main()
