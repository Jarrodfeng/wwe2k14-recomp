"""All pairs of a set of runs' det_serial traces (Session 77): per pair, the differing det vblanks split into
'placement' (the same number of external requests, x, in a different place among the same thread picks) and 'count'
(a different number of x), and thread-order differences. Read-only.
    py tools/xdet/trace_pairs.py RUN RUN [RUN ...]
"""
import itertools
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trace_diff as td  # noqa: E402


def main():
    traces = {r: td.read(td.trace_path(r)) for r in sys.argv[1:]}
    for a, b in itertools.combinations(sys.argv[1:], 2):
        ta, tb = traces[a], traces[b]
        place, count, order = [], [], []
        for v in sorted(set(ta) & set(tb)):
            if ta[v][:2] == tb[v][:2]:
                continue
            pa, pb = td.picks_only(ta[v][2]), td.picks_only(tb[v][2])
            if [p for p in pa if p != 'x'] != [p for p in pb if p != 'x']:
                order.append(v)
            elif pa.count('x') != pb.count('x'):
                count.append(v)
            else:
                place.append(v)
        print('%s vs %s: placement %d %s | count %d %s | thread order %d %s' % (
            a, b, len(place), place[:6], len(count), (count[0], count[-1]) if count else '', len(order), order[:4]))


if __name__ == '__main__':
    main()
