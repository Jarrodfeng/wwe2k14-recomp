"""Session 21: compare two runs' frame hashes (src/clip_capture.h --wwe2k14_clip_hash_log).

    py tools/perf/hash_compare.py A.hashes.csv B.hashes.csv [--list 20] [--ignore-tiles 3,4,11]
    py tools/perf/hash_compare.py RUN_A RUN_B [--dir out\\harness\\step21\\clips]   (finds NNN_clip_*.hashes.csv)

Frames are matched by guest frame. Prints frames in both, identical, different (first ones listed), and frames only
in one. With tile hashes (8 x 8 grid, tile = row * 8 + column from the top left): which tiles differ, per frame and
as a histogram; --ignore-tiles leaves tiles out (a frame then counts as different only if another tile differs).
Exit code 0 = every common frame identical (outside ignored tiles) and the frame sets equal, 1 otherwise.
"""
import argparse
import collections
import glob
import os
import sys

CLIPS = r'C:\Users\Jarrod\wwe2k14-recomp\out\harness\step21\clips'


def resolve(x, d):
    if os.path.exists(x):
        return x
    hits = sorted(glob.glob(os.path.join(d, '%s_clip_*.hashes.csv' % str(x).zfill(3))))
    if not hits:
        raise SystemExit('no hashes for %s in %s' % (x, d))
    return hits[-1]


def load_full(path):
    """{guest frame: (full hash, [tile hashes] or None)}"""
    out = {}
    for line in open(path):
        if line.startswith('guest_frame'):
            continue
        parts = line.strip().split(',')
        out[int(parts[0])] = (parts[1], parts[2].split(';') if len(parts) > 2 and parts[2] else None)
    return out


def load(path):
    """{guest frame: full hash} (hash_shift.py)"""
    return {f: v[0] for f, v in load_full(path).items()}


def tile_diff(a, b):
    if a[1] is None or b[1] is None:
        return None
    return [i for i, (x, y) in enumerate(zip(a[1], b[1])) if x != y]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--dir', default=CLIPS)
    ap.add_argument('--list', type=int, default=20)
    ap.add_argument('--ignore-tiles', default='')
    a = ap.parse_args()
    ignore = {int(x) for x in a.ignore_tiles.split(',') if x}
    pa, pb = resolve(a.a, a.dir), resolve(a.b, a.dir)
    A, B = load_full(pa), load_full(pb)
    common = sorted(set(A) & set(B))
    diff, tiles_of = [], {}
    hist = collections.Counter()
    for f in common:
        if A[f][0] == B[f][0]:
            continue
        td = tile_diff(A[f], B[f])
        if td is not None:
            hist.update(td)
            kept = [t for t in td if t not in ignore]
            if not kept:
                continue
            tiles_of[f] = td
        diff.append(f)
    only_a, only_b = sorted(set(A) - set(B)), sorted(set(B) - set(A))
    print('A %s (%d frames)\nB %s (%d frames)' % (pa, len(A), pb, len(B)))
    print('common %d (guest frames %s-%s): identical %d, different %d%s; only in A %d, only in B %d' % (
        len(common), common[0] if common else '-', common[-1] if common else '-', len(common) - len(diff),
        len(diff), ' (ignoring tiles %s)' % ','.join(map(str, sorted(ignore))) if ignore else '', len(only_a),
        len(only_b)))
    if diff:
        print('first different frames: %s' % ', '.join(
            '%d%s' % (f, '[%s]' % ' '.join(map(str, tiles_of[f])) if f in tiles_of else '') for f in diff[:a.list]))
    if hist:
        print('tiles differing (tile: frames), all differing frames: %s' % ', '.join(
            '%d:%d' % kv for kv in sorted(hist.items(), key=lambda kv: -kv[1])))
    ok = not diff and not only_a and not only_b
    print('RESULT: %s' % ('IDENTICAL' if ok else 'DIFFERENT'))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
