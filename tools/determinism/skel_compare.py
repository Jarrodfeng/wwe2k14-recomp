"""Compare two runs' skeleton update probes (--wwe2k14_skel_probe=true, src/fp_probe.h; Session 8 task 2).

Usage: py tools/determinism/skel_compare.py A B [--exe-dir PRESET] [--from-frame F] [--max N]

Reads out/harness/determinism/skel_A.tsv and skel_B.tsv (one S row per sub_82BD30E8 call with hashes of its inputs
and outputs and MXCSR, one D row per sub_82BD3620 call with its delta vector). Per object (the `self` pointer), the
two call sequences are aligned with difflib on (root, parent indices, local matrices, output) hashes, so extra calls
in one run (load-time frame differences) show as inserts, not as a difference in every later call. For the first
differing calls it prints which inputs differ:
  root   the root matrix (this+96)            par   the parent indices
  loc    the local matrices at the loop start  locm  the local matrices changed during the call (another writer)
  off    the offset vector (r1+80)            out   the output matrices at exit
and the MXCSR / cached csr at each hook. If no input differs but `out` does, the computation itself (or its
floating-point state) differs.

With skel_A.bin / skel_B.bin (--wwe2k14_skel_probe_dump_from) it also prints the differing words of the first
differing call that both dumps hold: matrix k (bone), word offset, both values as hex and float.

Game-derived output: run it and keep its output under out/harness/ only.
"""
import argparse
import difflib
import os
import struct
import sys

R = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DET = os.path.join(R, 'out', 'harness', 'determinism')

COLS = ('kind frame gtid self count r27 parents local out h_root h_par h_loc_in h_loc_mid h_out_mid off_x off_y '
        'off_z off_w h_out_exit mx_in mx_mid mx_exit csr_in csr_exit').split()


def stem(run, exe_dir):
    n = '%03d' % int(run)
    return 'skel_%s' % n if exe_dir == 'win-amd64-release' else 'skel_%s_%s' % (exe_dir, n)


def read_tsv(run, exe_dir, want=None):
    path = os.path.join(DET, stem(run, exe_dir) + '.tsv')
    rows = []
    with open(path, encoding='utf-8', errors='replace') as f:
        header = f.readline().rstrip('\n').split('\t')
        if header != COLS:
            print('warning: %s header differs from the expected columns' % path)
        for line in f:
            p = line.rstrip('\n').split('\t')
            if len(p) != len(COLS):
                continue  # last line cut off by the process exit
            if want and p[3] not in want:
                continue
            r = dict(zip(COLS, p))
            r['frame'] = int(r['frame'])
            rows.append(r)
    return path, rows


def key(r):
    if r['kind'] == 'D':
        return ('D', r['parents'], r['local'], r['out'], r['off_x'], r['off_y'], r['off_z'])
    return ('S', r['h_root'], r['h_par'], r['h_loc_in'], r['off_x'], r['off_z'], r['h_out_exit'])


def fields(a, b):
    if a['kind'] != b['kind']:
        return ['kind']
    if a['kind'] == 'D':
        out = []
        if (a['parents'], a['local'], a['out']) != (b['parents'], b['local'], b['out']):
            out.append('old position')
        if (a['off_x'], a['off_y'], a['off_z'], a['off_w']) != (b['off_x'], b['off_y'], b['off_z'], b['off_w']):
            out.append('delta')
        return out
    out = []
    for name, col in (('root', 'h_root'), ('par', 'h_par'), ('loc', 'h_loc_in'), ('out_mid', 'h_out_mid')):
        if a[col] != b[col]:
            out.append(name)
    if (a['h_loc_in'] != a['h_loc_mid']) or (b['h_loc_in'] != b['h_loc_mid']):
        if a['h_loc_mid'] != '0000000000000000' or b['h_loc_mid'] != '0000000000000000':
            out.append('locm(A %s B %s)' % (a['h_loc_in'] != a['h_loc_mid'], b['h_loc_in'] != b['h_loc_mid']))
    if (a['off_x'], a['off_y'], a['off_z'], a['off_w']) != (b['off_x'], b['off_y'], b['off_z'], b['off_w']):
        out.append('off')
    if a['h_out_exit'] != b['h_out_exit']:
        out.append('out')
    if (a['mx_in'], a['mx_mid'], a['mx_exit'], a['csr_in'], a['csr_exit']) != \
            (b['mx_in'], b['mx_mid'], b['mx_exit'], b['csr_in'], b['csr_exit']):
        out.append('mxcsr')
    return out


def fmt(r):
    if r['kind'] == 'D':
        return 'D f%d self %s old (%s %s %s) delta (%s %s %s %s)' % (
            r['frame'], r['self'], r['parents'], r['local'], r['out'], r['off_x'], r['off_y'], r['off_z'], r['off_w'])
    return ('S f%d gt%s self %s n=%s r27=%s root %s par %s loc %s/%s outmid %s off (%s %s) out %s mx %s/%s/%s '
            'csr %s/%s' % (r['frame'], r['gtid'], r['self'], r['count'], r['r27'], r['h_root'][-6:], r['h_par'][-6:],
                           r['h_loc_in'][-6:], r['h_loc_mid'][-6:], r['h_out_mid'][-6:], r['off_x'], r['off_z'],
                           r['h_out_exit'][-6:], r['mx_in'], r['mx_mid'], r['mx_exit'], r['csr_in'], r['csr_exit']))


def read_bin(run, exe_dir):
    path = os.path.join(DET, stem(run, exe_dir) + '.bin')
    recs = {}
    if not os.path.exists(path):
        return None
    data = open(path, 'rb').read()
    pos = 0
    while pos + 32 <= len(data):
        if data[pos:pos + 4] != b'SKL1':
            break
        kind, frame, gtid, self_, count = struct.unpack_from('<IQIII', data, pos + 4)
        pos += 28
        if kind == 1:
            sizes = [64, count * 80, count * 64, count * 64, 16]
        else:
            sizes = [16, 16]
        parts = []
        for s in sizes:
            parts.append(data[pos:pos + s])
            pos += s
        if any(len(p) != s for p, s in zip(parts, sizes)):
            break
        recs.setdefault((kind, '%08X' % self_, frame), []).append(parts)
    return recs


def f32(word):
    return struct.unpack('>f', word)[0]


def diff_words(name, a, b, stride, limit=12):
    n = 0
    for off in range(0, min(len(a), len(b)), 4):
        wa, wb = a[off:off + 4], b[off:off + 4]
        if wa != wb:
            n += 1
            if n <= limit:
                print('      %-5s item %3d +%-3d A %s (%.9g)  B %s (%.9g)' % (
                    name, off // stride, off % stride, wa.hex().upper(), f32(wa), wb.hex().upper(), f32(wb)))
    if n > limit:
        print('      %-5s ... %d differing words in all' % (name, n))
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--exe-dir', default='win-amd64-release')
    ap.add_argument('--from-frame', type=int, default=0, help='ignore differences before this A frame')
    ap.add_argument('--max', type=int, default=6, help='differing blocks to show per object')
    ap.add_argument('--self', default='', help='comma list of object pointers (self column) to compare; default all')
    args = ap.parse_args()
    want = set(x.strip().upper() for x in args.self.split(',') if x.strip())
    pa, ra = read_tsv(args.a, args.exe_dir, want)
    pb, rb = read_tsv(args.b, args.exe_dir, want)
    print('A %s: %d rows   B %s: %d rows' % (pa, len(ra), pb, len(rb)))
    for name, rows in (('A', ra), ('B', rb)):
        s = [r for r in rows if r['kind'] == 'S']
        mx = {}
        for r in s:
            for col in ('mx_in', 'mx_mid', 'mx_exit', 'csr_in', 'csr_exit'):
                mx.setdefault(col, {}).setdefault(r[col], 0)
                mx[col][r[col]] += 1
        locm = sum(1 for r in s if r['h_loc_mid'] != '0000000000000000' and r['h_loc_mid'] != r['h_loc_in'])
        threads = sorted(set(r['gtid'] for r in rows))
        print('%s: %d S rows, %d D rows, guest threads %s, local matrices changed during a call: %d' % (
            name, len(s), len(rows) - len(s), ','.join(threads), locm))
        for col in ('mx_in', 'mx_mid', 'mx_exit', 'csr_in', 'csr_exit'):
            print('   %-8s %s' % (col, '  '.join('%s x%d' % kv for kv in sorted(mx.get(col, {}).items()))))
    ba = read_bin(args.a, args.exe_dir)
    bb = read_bin(args.b, args.exe_dir)
    by_a, by_b = {}, {}
    for r in ra:
        by_a.setdefault(r['self'], []).append(r)
    for r in rb:
        by_b.setdefault(r['self'], []).append(r)
    # print objects in the order of their first call in either run (ties by address)
    selves = sorted((set(by_a) | set(by_b)) if not want else want,
                    key=lambda s: (min([r['frame'] for r in by_a.get(s, [])] + [r['frame'] for r in by_b.get(s, [])],
                                       default=0), s))
    firsts = []
    for s in selves:
        xa, xb = by_a.get(s, []), by_b.get(s, [])
        ka, kb = [key(r) for r in xa], [key(r) for r in xb]
        sm = difflib.SequenceMatcher(None, ka, kb, autojunk=False)
        ops = [op for op in sm.get_opcodes() if op[0] != 'equal']
        ops = [op for op in ops if (xa[op[1]]['frame'] if op[1] < len(xa) else 1 << 60) >= args.from_frame]
        eq = sum(op[2] - op[1] for op in sm.get_opcodes() if op[0] == 'equal')
        print('\nobject %s: A %d calls (frames %s-%s), B %d calls, %d aligned equal, %d differing blocks' % (
            s, len(xa), xa[0]['frame'] if xa else '-', xa[-1]['frame'] if xa else '-', len(xb), eq, len(ops)))
        for tag, i1, i2, j1, j2 in ops[:args.max]:
            fa = xa[i1]['frame'] if i1 < len(xa) else None
            fb = xb[j1]['frame'] if j1 < len(xb) else None
            print('  %s A[%d:%d] (frame %s) B[%d:%d] (frame %s)' % (tag, i1, i2, fa, j1, j2, fb))
            if tag == 'replace':
                for k in range(min(3, i2 - i1, j2 - j1)):
                    a, b = xa[i1 + k], xb[j1 + k]
                    print('    differs in: %s' % ', '.join(fields(a, b)))
                    print('    A %s' % fmt(a))
                    print('    B %s' % fmt(b))
                    if k == 0 and ba is not None and bb is not None and a['kind'] == 'S':
                        pa_ = ba.get((1, s, a['frame']))
                        pb_ = bb.get((1, s, b['frame']))
                        if pa_ and pb_:
                            # the k-th call of this object in that frame
                            ia = sum(1 for r in xa[:i1] if r['frame'] == a['frame'] and r['kind'] == 'S')
                            ib = sum(1 for r in xb[:j1] if r['frame'] == b['frame'] and r['kind'] == 'S')
                            if ia < len(pa_) and ib < len(pb_):
                                A, B = pa_[ia], pb_[ib]
                                print('    raw words that differ (dump):')
                                for name, x, y, stride in (('root', A[0], B[0], 64), ('ptab', A[1], B[1], 80),
                                                           ('loc', A[2], B[2], 64), ('out', A[3], B[3], 64),
                                                           ('off', A[4], B[4], 16)):
                                    diff_words(name, x, y, stride)
            if fa is not None:
                firsts.append((fa, s, tag))
    firsts.sort()
    print('\nfirst differing blocks over all objects (A frame, object, kind):')
    for f in firsts[:10]:
        print('  %s %s %s' % f)


if __name__ == '__main__':
    sys.exit(main())
