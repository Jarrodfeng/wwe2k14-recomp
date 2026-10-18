"""Where did the racing 52-byte boot allocation land? (Session 8 task 2; src/fp_probe.h)

Usage: py tools/determinism/alloc_place.py RUN [RUN ...] [--exe-dir PRESET]

For each run with an allocation trace (out/harness/determinism/alloc_NNN.tsv, --wwe2k14_alloc_trace_frames=N) it
finds the boot thread's 0x836F3AAC marker request (return address 0x827E0FB8 / 0x827E0FC0, the boot thread = the
thread of the first row) and the 52-byte path-1 request from 0x82250080 on another thread, and prints how many
boot-thread requests lie between them: "+k" = the racing request entered after the marker request and k more
boot-thread requests ("lands k after"), "-k" = before the marker with k boot-thread requests between. Also prints
the heap marker words and the [detalloc] / [detplace] lines from the run's log. Runs without a trace print only
the log lines.
"""
import argparse
import os
import re
import sys

R = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DET = os.path.join(R, 'out', 'harness', 'determinism')
MARKERS = ('827E0FB8', '827E0FC0')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('runs', nargs='+')
    ap.add_argument('--exe-dir', default='win-amd64-release')
    args = ap.parse_args()
    for run in args.runs:
        n = '%03d' % int(run)
        name = 'alloc_%s' % n if args.exe_dir == 'win-amd64-release' else 'alloc_%s_%s' % (args.exe_dir, n)
        path = os.path.join(DET, name + '.tsv')
        log = os.path.join(R, 'out', 'build', args.exe_dir, 'logs', 'wwe2k14_%s.log' % n)
        lines = []
        if os.path.exists(log):
            with open(log, encoding='utf-8', errors='replace') as f:
                for line in f:
                    if re.search(r'\[(detalloc|detplace)\]|heap markers|alloctrace\] stopped', line):
                        lines.append(line.rstrip('\n').split('] ', 4)[-1])
        place = 'no trace'
        if os.path.exists(path):
            rows = []
            with open(path, encoding='utf-8', errors='replace') as f:
                f.readline()
                for line in f:
                    p = line.rstrip('\n').split('\t')
                    if len(p) == 7:
                        rows.append(p)
            boot = rows[0][2] if rows else None
            mi = next((i for i, r in enumerate(rows) if r[2] == boot and r[6] in MARKERS), None)
            ri = next((i for i, r in enumerate(rows) if r[2] != boot and r[3] == '1' and r[4] == '52'
                       and r[6] == '82250080'), None)
            if mi is None or ri is None:
                place = 'marker row %s, racing row %s' % (mi, ri)
            elif ri > mi:
                k = sum(1 for r in rows[mi + 1:ri] if r[2] == boot)
                place = '+%d (lands %d boot-thread requests after the marker; racing request on guest thread %s)' % (
                    k, k, rows[ri][2])
            else:
                k = sum(1 for r in rows[ri + 1:mi] if r[2] == boot)
                place = '-%d (before the marker, %d boot-thread requests between; guest thread %s)' % (
                    k, k, rows[ri][2])
        print('run %s: %s' % (n, place))
        for line in lines:
            print('   %s' % line)


if __name__ == '__main__':
    sys.exit(main())
