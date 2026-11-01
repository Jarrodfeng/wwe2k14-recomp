"""Decode raw det_hash dumps of a run as big-endian floats / words, to check what candidate regions hold (Session 77,
XDET_PLAN.md P1.1). Read-only; never launches the game. Output is game-derived: it stays in the terminal / out\\.

    py tools/xdet/peek_regions.py RUN [--regions p0,p1,dist] [--every N] [--words] [--exe-dir PRESET_OR_DIR]

RUN is the run number (wwe2k14_NNN.log). Region names come from line 2 of det_NNN.csv; the bytes from det_NNN.bin
(--wwe2k14_det_hash_raw_every). For each dumped frame (every N-th dump with --every) it prints each region's
32-bit words as floats ('-' when unreadable), or as hex with --words. Large regions print only their non-zero words.
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'determinism'))
import det_compare  # noqa: E402  (read_bin, det_stem, DET)


def region_names(run, exe_dir):
    path = os.path.join(det_compare.DET, det_compare.det_stem(run, exe_dir) + '.csv')
    with open(path, encoding='utf-8', errors='replace') as f:
        line = f.readline()
        while line.startswith('#'):
            line = f.readline()
        names = line.strip().split(',')[1:]
        # Session 77 v2 CSVs: 'vb' (the det vblank) is a column but has no bytes in the raw dumps.
        return [n for n in names if n != 'vb']


def fmt_word(b, words):
    (u,) = struct.unpack('>I', b)
    if words:
        return '%08X' % u
    (x,) = struct.unpack('>f', b)
    return '%.4g' % x


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run', type=int)
    ap.add_argument('--regions', default='')
    ap.add_argument('--every', type=int, default=1)
    ap.add_argument('--words', action='store_true')
    ap.add_argument('--exe-dir', default=None)
    a = ap.parse_args()
    exe_dir = a.exe_dir
    if exe_dir and not os.path.isabs(exe_dir):
        exe_dir = os.path.join(det_compare.BUILD, exe_dir)
    names = region_names(a.run, exe_dir or det_compare.EXE_DIR)
    want = [n for n in a.regions.split(',') if n] or names
    missing = [n for n in want if n not in names]
    if missing:
        sys.exit('peek_regions: no region %s (have: %s)' % (', '.join(missing), ', '.join(names)))
    dumps = det_compare.read_bin(a.run, exe_dir)
    if not dumps:
        sys.exit('peek_regions: no raw dumps for run %d (needs --wwe2k14_det_hash_raw_every)' % a.run)
    for k, frame in enumerate(sorted(dumps)):
        if k % a.every:
            continue
        print('frame %d' % frame)
        regs = dumps[frame]
        for n in want:
            r = regs[names.index(n)]
            if r is None:
                print('  %-6s -' % n)
                continue
            b = bytes(r)
            cells = []
            for off in range(0, len(b) - 3, 4):
                w = b[off:off + 4]
                if len(b) > 64 and w == b'\0\0\0\0':
                    continue
                cells.append(('+%d:' % off if len(b) > 64 else '') + fmt_word(w, a.words))
            print('  %-6s %s' % (n, ' '.join(cells[:40]) + (' ...' if len(cells) > 40 else '')))


if __name__ == '__main__':
    main()
