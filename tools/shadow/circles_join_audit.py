r"""Lane 215: why do circle draws stay the stream's (mismatch constants / late)? Offline, 0 game runs.

  py -I tools\shadow\circles_join_audit.py CASE_ZIP_OR_CAPTURE [--frames A:B]

For every captured frame: the stream's circle draws (call site 0x828131E4, in draw order) as their 29 constant rows, and every record entry (section 16 of capture\scene\frame_*.rsc, all records pooled by
the entry's own guest-frame stamp). For each stream draw k of a frame with header guest frame G it searches ALL held (stamp, ordinal) for the entry whose rows equal the draw's, and prints a histogram of
(stamp - G, ordinal - k), then each frame whose draws do not all join at the modal (delta, ordinal k): the frame, G, the draws, where each one did join (or 'nowhere'), and for 'nowhere' the first differing row of the
nearest entry. Numbers only; nothing is written."""
import os
import struct
import sys
import collections
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import shadow_census as sc  # noqa: E402

SEC_CIRCLES = 16
ESZ = 432


def records(path):
    """{stamp: [entry rows (29x4 f32), slot, character, flags]} pooled from every .rsc of the capture."""
    out = collections.defaultdict(dict)
    if os.path.isdir(path):
        d = path if os.path.isdir(os.path.join(path, 'scene')) else os.path.join(path, 'capture')
        names = sorted(os.path.join(d, 'scene', n) for n in os.listdir(os.path.join(d, 'scene')) if n.endswith('.rsc'))
        read = lambda n: open(n, 'rb').read()
    else:
        zf = zipfile.ZipFile(path)
        names = sorted(n for n in zf.namelist() if n.endswith('.rsc'))
        read = zf.read
    nrec = 0
    for n in names:
        b = read(n)
        if len(b) < 40 or b[:4] != b'RSCN':
            continue
        nrec += 1
        count = struct.unpack_from('<I', b, 8)[0]
        for i in range(count):
            sid, esz, cnt, off = struct.unpack_from('<IIQQ', b, 40 + 24 * i)
            if sid != SEC_CIRCLES or esz != ESZ:
                continue
            for e in range(cnt):
                a = off + e * ESZ
                frame, ch, slot, flags = struct.unpack_from('<IIII', b, a)
                f = np.frombuffer(b, '<f4', 104, a + 16)
                rows = np.zeros((29, 4), np.float32)
                rows[0] = f[0:4]
                rows[4:8] = f[4:20].reshape(4, 4)
                rows[8:29] = f[20:104].reshape(21, 4)
                out[frame][(slot, ch)] = (rows, slot, ch, flags, n)
    return out, nrec


def main():
    a = sys.argv[1:]
    path = a[0]
    spec = a[a.index('--frames') + 1] if '--frames' in a else ''
    rec, nrec = records(path)
    stamps = sorted(rec)
    print('records %d, stamps %d (%s..%s)' % (nrec, len(stamps), stamps[0] if stamps else '-', stamps[-1] if stamps else '-'))
    cap = sc.Cap(path)
    lo, hi = 0, len(cap.frames)
    if spec:
        p = spec.split(':')
        lo, hi = int(p[0] or 0), min(hi, int(p[1] or hi))
    hist = collections.Counter()
    bad = []
    nfr = ndr = 0
    held = {s: sorted(v.values(), key=lambda t: (t[1], t[2])) for s, v in rec.items()}
    for fi in range(lo, hi):
        f = cap.c.frame(fi)
        if not f.has_origins:
            continue
        o = f.origins
        idx = np.nonzero(sc.pcap.queue_site(o) == sc.CALL_CIRCLE)[0]
        if not len(idx):
            continue
        G = int(f.guest_frame) & 0xFFFFFFFF
        nfr += 1
        res = []
        for k, i in enumerate(idx):
            vb = f.bank(int(f.draws[i]['vs_bank']))
            rows = np.zeros((29, 4), np.float32)
            rows[0] = sc.regs(vb, 0, 1)[0]
            rows[4:8] = sc.regs(vb, 4, 4)
            rows[8:29] = sc.regs(vb, 8, 21)
            found = []   # every (stamp - G, ordinal shift, slot) whose rows equal this draw's (a still scene matches at many stamps)
            for s in range(G - 6, G + 7):
                for kk, e in enumerate(held.get(s, [])):
                    if np.all(e[0] == rows):
                        found.append((s - G, kk - k, e[1]))
            res.append(found)
            ndr += 1
            hist[(tuple(sorted(set(x[0] for x in found))) if found else 'nowhere')] += 1
        # the live hook's offset d = 1 means stamp = G - 1, i.e. (stamp - G) == -1 here; only report draws that do NOT equal the entry of that stamp and ordinal when that stamp is held
        if any(not any(x[0] == -1 and x[1] == 0 for x in r) for r in res) and (G - 1) in held:
            bad.append((fi, G, len(idx), res))
    print('frames with circle draws %d, draws %d' % (nfr, ndr))
    print('histogram (the set of stamp-G offsets at which a draw equals an entry): count')
    for k, v in sorted(hist.items(), key=lambda kv: -kv[1]):
        print('  %s: %d' % (k, v))
    print('frames where a draw has no entry at (stamp G+1... i.e. offset 1, same ordinal): %d' % len(bad))
    for fi, G, n, res in bad[:60]:
        print('  frame %d G %d draws %d -> %s' % (fi, G, n, ['nowhere' if not r else [x[0] for x in r] for r in res]))


if __name__ == '__main__':
    main()


def detail(path, fi_lo, fi_hi):
    """Frames fi_lo..fi_hi: for each stream draw, the count of equal float words against every entry of stamps G-3..G+1 (which rows differ)."""
    rec, _ = records(path)
    cap = sc.Cap(path)
    for fi in range(fi_lo, fi_hi + 1):
        f = cap.c.frame(fi)
        o = f.origins
        idx = np.nonzero(sc.pcap.queue_site(o) == sc.CALL_CIRCLE)[0]
        G = int(f.guest_frame) & 0xFFFFFFFF
        print('frame %d G %d: %d circle draws; entries at stamps %s' % (fi, G, len(idx), {s: len(rec.get(s, {})) for s in range(G - 3, G + 2)}))
        for k, i in enumerate(idx):
            vb = f.bank(int(f.draws[i]['vs_bank']))
            rows = np.zeros((29, 4), np.float32)
            rows[0] = sc.regs(vb, 0, 1)[0]
            rows[4:8] = sc.regs(vb, 4, 4)
            rows[8:29] = sc.regs(vb, 8, 21)
            for s in (G - 1,):
                es = sorted(rec.get(s, {}).values(), key=lambda t: (t[1], t[2]))
                for kk, e in enumerate(es):
                    diff = [r for r in list(range(0, 1)) + list(range(4, 29)) if not np.all(e[0][r] == rows[r])]
                    print('   draw %d vs stamp %d entry %d (slot %d char %08x flags %d): differing rows %s%s' % (k, s, kk, e[1], e[2], e[3], diff[:12], ' EQUAL' if not diff else ''))


def vp_diff(path, fi):
    rec, _ = records(path)
    cap = sc.Cap(path)
    f = cap.c.frame(fi)
    G = int(f.guest_frame) & 0xFFFFFFFF
    idx = np.nonzero(sc.pcap.queue_site(f.origins) == sc.CALL_CIRCLE)[0]
    vb = f.bank(int(f.draws[idx[0]]['vs_bank']))
    st = sc.regs(vb, 4, 4)
    e = sorted(rec[G - 1].values(), key=lambda t: (t[1], t[2]))[0][0][4:8]
    np.set_printoptions(precision=6, suppress=True, linewidth=200)
    print('stream ViewProj\n', st, '\nrecord ViewProj (stamp G-1)\n', e)
    # the props map's light in the same frame (Trapezoidal x ViewProj), for the independent reference
    d = f.draws; o = f.origins
    pj = next((j for j in range(len(d)) if int(o[j]['pass_site']) == sc.SITE_MAP and float(abs(sc.regs(f.bank(int(d[j]['vs_bank'])), sc.R_VIEWPROJ, 4)[3][1] - 1.0)) < 1e-6), None)
    if pj is not None:
        vb2 = f.bank(int(d[pj]['vs_bank']))
        tv = sc.regs(vb2, sc.R_TRAP, 4).astype(np.float64) @ sc.regs(vb2, sc.R_VIEWPROJ, 4).astype(np.float64)
        print('Trap x ViewProj of the props light this frame\n', tv)
        print('max rel err stream vs ref %.2e, record vs ref %.2e' % (np.abs(st - tv).max() / np.abs(tv).max(), np.abs(e - tv).max() / np.abs(tv).max()))
    else:
        print('no props map draw in this frame')


def split_view(path, lo, hi):
    """Frames lo..hi: the header guest frame, the circle draws, the stamps in the frame's own record file, and whether the wanted stamp (G-1) is held from the files up to this one (what a live board holds)."""
    import re
    rec, _ = records(path)
    cap = sc.Cap(path)
    perfile = collections.defaultdict(set)
    for s, v in rec.items():
        for e in v.values():
            perfile[int(re.search(r'frame_(\d+)', e[4]).group(1))].add(s)
    held = set()
    for j in range(0, lo):
        held |= perfile.get(j, set())
    for j in range(lo, hi + 1):
        f = cap.c.frame(j)
        G = int(f.guest_frame) & 0xFFFFFFFF
        idx = np.nonzero(sc.pcap.queue_site(f.origins) == sc.CALL_CIRCLE)[0]
        held |= perfile.get(j, set())
        print('frame %d G %d circle draws %d, file stamps %s, wanted %d %s, draws in frame %d' % (j, G, len(idx), sorted(perfile.get(j, [])), G - 1, 'held' if (G - 1) in held else 'MISSING', len(f.draws)))


def scan(path):
    """One line: the capture's circle draws, those not bit-equal to the entry of stamp G-1 / ordinal k (when held), of those how many hold a NaN in the stream's rows, and the first few frames."""
    rec, _ = records(path)
    cap = sc.Cap(path)
    n = bad = nan = 0
    first = []
    for fi in range(len(cap.frames)):
        f = cap.c.frame(fi)
        if not f.has_origins:
            continue
        idx = np.nonzero(sc.pcap.queue_site(f.origins) == sc.CALL_CIRCLE)[0]
        G = int(f.guest_frame) & 0xFFFFFFFF
        es = sorted(rec.get(G - 1, {}).values(), key=lambda t: (t[1], t[2]))
        for k, i in enumerate(idx):
            n += 1
            vb = f.bank(int(f.draws[i]['vs_bank']))
            rows = np.zeros((29, 4), np.float32)
            rows[0] = sc.regs(vb, 0, 1)[0]
            rows[4:8] = sc.regs(vb, 4, 4)
            rows[8:29] = sc.regs(vb, 8, 21)
            if k >= len(es):
                continue
            if rows.view(np.uint32).tobytes() != es[k][0].view(np.uint32).tobytes():
                bad += 1
                nan += int(np.isnan(rows).any() or np.isnan(es[k][0]).any())
                if len(first) < 6:
                    first.append((fi, G, k))
    print('%s: circle draws %d, not bit-equal to the entry of stamp G-1 %d (of those with a NaN %d); first %s' % (os.path.basename(os.path.dirname(path)) if os.path.isfile(path) else path, n, bad, nan, first))
