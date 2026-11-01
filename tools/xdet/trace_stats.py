"""Per-run counts from det_serial scheduler traces (Session 77): total decisions, thread picks, external requests
('x' = GPU interrupt callbacks), idle points, and the det vblank ranges where two runs differ (contiguous blocks).
Read-only.   py tools/xdet/trace_stats.py RUN_OR_TRACE [RUN_OR_TRACE ...]
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trace_diff as td  # noqa: E402


def blocks(vbs):
    out = []
    for v in vbs:
        if out and v == out[-1][1] + 1:
            out[-1][1] = v
        else:
            out.append([v, v])
    return out


def main():
    runs = sys.argv[1:]
    traces = [(r, td.read(td.trace_path(r))) for r in runs]
    for r, t in traces:
        picks = [p for v in sorted(t) for p in td.picks_only(t[v][2])]
        decisions = sum(t[v][0] for v in t)
        print('%-8s vblanks %d, decisions %d, x %d, idle %d, thread picks %d' % (
            r, len(t), decisions, picks.count('x'), picks.count('-'),
            sum(1 for p in picks if p not in ('x', '-'))))
    for i in range(1, len(traces)):
        (ra, ta), (rb, tb) = traces[0], traces[i]
        common = sorted(set(ta) & set(tb))
        diff = [v for v in common if ta[v][:2] != tb[v][:2]]
        xa = sum(td.picks_only(ta[v][2]).count('x') for v in diff)
        xb = sum(td.picks_only(tb[v][2]).count('x') for v in diff)
        print('%s vs %s: %d differing vblanks in blocks %s; x in them: %d vs %d' % (
            ra, rb, len(diff), ', '.join('%d-%d' % tuple(b) if b[1] > b[0] else str(b[0]) for b in blocks(diff)[:12]),
            xa, xb))


if __name__ == '__main__':
    main()
