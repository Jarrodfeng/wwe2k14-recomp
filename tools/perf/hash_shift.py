"""Session 21: are the frames two hash lists disagree on just a capture one frame early / late?

    py tools/perf/hash_shift.py RUN_A RUN_B [--dir out\\harness\\step21\\clips]

For each guest frame whose hashes differ: 'shift' when A's hash equals B's at f-1 or f+1 (or B's equals A's at
f-1 / f+1), 'repeat' when either side equals its own previous frame (a capture that got the previous image again),
else 'unexplained' (a real rendering difference). Prints the counts and the unexplained frames. Numbers only.
"""
import argparse
import sys

sys.path.insert(0, __import__('os').path.dirname(__file__))
from hash_compare import CLIPS, load, resolve  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--dir', default=CLIPS)
    a = ap.parse_args()
    A, B = load(resolve(a.a, a.dir)), load(resolve(a.b, a.dir))
    counts = {'shift': 0, 'repeat': 0, 'unexplained': 0}
    unexplained = []
    for f in sorted(set(A) & set(B)):
        if A[f] == B[f]:
            continue
        near_b = {B.get(f - 1), B.get(f + 1)}
        near_a = {A.get(f - 1), A.get(f + 1)}
        if A[f] in near_b or B[f] in near_a:
            counts['shift'] += 1
        elif A[f] == A.get(f - 1) or B[f] == B.get(f - 1):
            counts['repeat'] += 1
        else:
            counts['unexplained'] += 1
            unexplained.append(f)
    print('%s vs %s: differing frames %d = shift %d, repeat %d, unexplained %d' % (
        a.a, a.b, sum(counts.values()), counts['shift'], counts['repeat'], counts['unexplained']))
    if unexplained:
        print('unexplained: %s' % ', '.join(map(str, unexplained[:40])))
    sys.exit(1 if unexplained else 0)


if __name__ == '__main__':
    main()
