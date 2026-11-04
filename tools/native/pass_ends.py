r"""Who ends the owned frame's render passes (Session 89; promoted from Session 87's ends.py): counts the
"[native][seg] frame F owned render pass end (WxH, N draws recorded): base B stack A A ..." lines of a run by
size and symbolised call stack (the SDK DLL that ran, via tools\perf\barmap.py's symbolizer).

    py -I tools\native\pass_ends.py RUN F0 F1 [--min-width W] [--dir LOGDIR] [--dll PATH] [--top 30]

RUN = run number (log in --dir, default the main exe's logs or LOGS=) or a log path. --dll defaults to the
rexgpu-xenos.dll beside the log folder (EXE\logs -> EXE\rexgpu-xenos.dll) - it must be the build that ran, with its
PDB. Needs `cvar gpu_native_segment_log N` in the run (the segment log prints the pass ends).
"""
import argparse
import collections
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'perf'))
import barmap  # noqa: E402

DEFAULT_LOGS = Path(os.environ.get('LOGS') or ROOT / 'out' / 'build' / 'win-amd64-release' / 'logs')
LINE = re.compile(r'frame (\d+) owned render pass end \((\d+)x(\d+), (\d+) draws recorded\): base ([0-9A-F]+) '
                  r'stack ((?:[0-9A-F]+ ?){1,8})')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('run'); ap.add_argument('f0', type=int); ap.add_argument('f1', type=int)
    ap.add_argument('--min-width', type=int, default=0)
    ap.add_argument('--dir', default=str(DEFAULT_LOGS))
    ap.add_argument('--dll')
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--depth', type=int, default=6, help='stack frames per key (default 6)')
    a = ap.parse_args()
    log = Path(a.run) if a.run.endswith('.log') else Path(a.dir) / f'wwe2k14_{int(a.run):03d}.log'
    if not log.exists():
        sys.exit(f'pass_ends: no log {log}')
    dll = Path(a.dll) if a.dll else log.parent.parent / 'rexgpu-xenos.dll'
    rows = []
    with open(log, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = LINE.search(line)
            if m and a.f0 <= int(m.group(1)) <= a.f1 and int(m.group(2)) >= a.min_width:
                rows.append((int(m.group(2)), int(m.group(3)), int(m.group(5), 16),
                             [int(x, 16) for x in m.group(6).split()]))
    print(f'{len(rows)} owned pass ends (width >= {a.min_width}) in frames {a.f0}-{a.f1} of {log.name}')
    if not rows:
        print('  (none: is gpu_native_segment_log on in this run, and the frames inside its window?)')
        return
    sym = {}
    if dll.exists():
        for base in sorted({r[2] for r in rows}):
            addrs = sorted({x for r in rows if r[2] == base for x in r[3]})
            sym.update(barmap.symbolize(addrs, base, str(dll)))
    else:
        print(f'  (no DLL at {dll}: addresses unsymbolised; give --dll)')
    cnt = collections.Counter()
    for w, h, base, st in rows:
        cnt[(w, h, tuple(sym.get(x, hex(x)).split(' ')[0][:110] for x in st[:a.depth]))] += 1
    for (w, h, stack), n in cnt.most_common(a.top):
        print(f'{n:6d}  {w}x{h}')
        for s in stack:
            print(f'          {s}')


if __name__ == '__main__':
    main()
