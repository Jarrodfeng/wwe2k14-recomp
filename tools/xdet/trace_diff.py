"""Compare two det_serial scheduler traces (--det_serial_trace) line by line (Session 77, XDET_PLAN.md SCHED layer).
Read-only. Prints how many det vblanks differ, and for the first few both pick lists, and whether a difference is only
the position of external requests ('x' = a GPU interrupt or other host-side guest code run between turns) among the
same thread picks.

    py tools/xdet/trace_diff.py TRACE_A TRACE_B [--show 8]
    py tools/xdet/trace_diff.py RUN_A RUN_B          (run numbers: the trace path is read from each run's log)
"""
import argparse
import os
import re
import sys

R = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOGS = os.path.join(R, 'out', 'build', 'win-amd64-release', 'logs')


def trace_path(arg):
    if not arg.isdigit():
        return arg
    log = os.path.join(LOGS, 'wwe2k14_%03d.log' % int(arg))
    with open(log, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.search(r'\[det\] serial trace -> (\S+) \(open\)', line)
            if m:
                return m.group(1)
    sys.exit('no trace line in %s' % log)


def read(path):
    out = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            p = line.rstrip('\n').split(' ', 3)
            if len(p) >= 3 and p[0].isdigit():
                out[int(p[0])] = (int(p[1]), p[2], p[3].split() if len(p) > 3 else [])
    return out


def picks_only(tokens):
    """The decisions alone: det_serial_trace_detail adds '[CP wait]' before a pick and 'iPOS.N' interrupt tags."""
    return [re.sub(r'^\[[^\]]*\]', '', t) for t in tokens if not re.match(r'^i[0-9A-F]+\.\d$', t)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--show', type=int, default=8)
    a = ap.parse_args()
    ta, tb = read(trace_path(a.a)), read(trace_path(a.b))
    common = sorted(set(ta) & set(tb))
    diffs = [v for v in common if ta[v][:2] != tb[v][:2]]
    ext_only = 0
    for v in diffs:
        pa = [p for p in picks_only(ta[v][2]) if p != 'x']
        pb = [p for p in picks_only(tb[v][2]) if p != 'x']
        full = len(picks_only(ta[v][2])) == ta[v][0] and len(picks_only(tb[v][2])) == tb[v][0]  # not cut at 240
        if pa == pb and full:
            ext_only += 1
    print('vblanks: A %d, B %d, %d in common; differing: %d (%.3f%%); of them only the position of external '
          'requests (x) among the same picks: %d' % (len(ta), len(tb), len(common), len(diffs),
                                                    100.0 * len(diffs) / max(1, len(common)), ext_only))
    for v in diffs[:a.show]:
        print('vblank %d: A count %d %s' % (v, ta[v][0], ' '.join(ta[v][2])))
        print('%s  B count %d %s' % (' ' * len('vblank %d:' % v), tb[v][0], ' '.join(tb[v][2])))


if __name__ == '__main__':
    main()
