r"""GPU pieces of a pass-logged run, three views (Session 89; promoted from Session 87's scratch scripts
pieces_by_scene.py, gaps2.py, frame_list.py). Reads the [native][pass] lines (owned_pieces.py's patterns).

    py -I tools\native\pass_pieces.py scenes RUN [--top 14]          GPU ms per piece per scene (frames grouped where
                                                                      the logged frames jump by > 100)
    py -I tools\native\pass_pieces.py gaps RUN F0 F1 [--top 16]      the GPU time between segments, idle taken out
                                                                      ("submission started X ms into the gap"): which
                                                                      piece the real (non-idle) gap time comes before
    py -I tools\native\pass_pieces.py frame RUN F                    one frame's pieces in order, with idle per gap

RUN = a run number (log in --dir, default the main exe's logs, or LOGS=<folder>) or a log path. The pass log needs
--vulkan_gpu_timestamps=true --gpu_native_gpu_timers=true and `cvar gpu_native_segment_log N` / a pass-log route
(short\s80_match_log.txt). Map: py tools\q\q.py map pass_pieces.
Differences from Session 87's scratch scripts (their numbers in old notes): `gaps` keys truth pieces by kind name (gaps2.py
lumped them as "truth"); `scenes` averages segment / gap totals over the frames that logged a total.
"""
import argparse
import os
import re
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import owned_pieces as op  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LOGS = Path(os.environ.get('LOGS') or ROOT / 'out' / 'build' / 'win-amd64-release' / 'logs')
IDLE_AT = re.compile(r'frame (\d+) submission started ([0-9.]+) ms into the ([0-9.]+) ms gap before #(\d+)')


def log_path(run, logdir):
    p = Path(run)
    if p.suffix == '.log' and p.exists():
        return p
    q = Path(logdir) / f'wwe2k14_{int(run):03d}.log'
    if not q.exists():
        sys.exit(f'pass_pieces: no log {q} (give --dir EXE\\logs or a log path)')
    return q


def read(path):
    """frames: {frame: [(index, label, box, ps, draws, own_ms, before_ms)]}, totals: {frame: (seg, gap, span)},
    idle: {(frame, index): idle ms in the gap before that index}."""
    frames, totals, idle = defaultdict(list), {}, {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = IDLE_AT.search(line)
            if m:
                idle[(int(m.group(1)), int(m.group(4)))] = float(m.group(2)); continue
            m = op.PASS.search(line)
            if m:
                kind, owned = int(m.group(3)), m.group(14)
                label = f'owned {owned}' if owned else op.KINDS.get(kind, 'truth')
                frames[int(m.group(1))].append((int(m.group(2)), label, f'{m.group(4)}x{m.group(5)}', m.group(11),
                                                int(m.group(9)), float(m.group(12)), float(m.group(13))))
                continue
            m = op.TOTAL.search(line)
            if m:
                totals[int(m.group(1))] = (float(m.group(3)), float(m.group(4)), float(m.group(5)))
    if not frames:
        sys.exit(f'pass_pieces: no [native][pass] piece lines in {path} (pass log not on in this run?)')
    return frames, totals, idle


def cmd_scenes(frames, totals, idle, a):
    groups, cur = [], []
    for fr in sorted(frames):
        if cur and fr - cur[-1] > 100:
            groups.append(cur); cur = []
        cur.append(fr)
    if cur:
        groups.append(cur)
    for g in groups:
        n = len(g)
        agg = defaultdict(lambda: [0.0, 0, 0])
        for fr in g:
            for _, label, box, ps, draws, ms, _ in frames[fr]:
                k = (label.split()[0] if label.startswith('owned') else label, box, ps)
                x = agg[k]; x[0] += ms; x[1] += draws; x[2] += 1
        t = [totals[f] for f in g if f in totals]
        seg, gap, span = (sum(v[i] for v in t) / max(1, len(t)) for i in range(3))
        print(f'== frames {g[0]}-{g[-1]} ({n}): {sum(len(frames[f]) for f in g) / n:.0f} pieces, segments {seg:.3f} ms '
              f'+ gaps {gap:.3f}, first to last {span:.3f}')
        for k, (ms, draws, c) in sorted(agg.items(), key=lambda x: -x[1][0])[:a.top]:
            print(f'  {ms / n:7.3f} ms  draws {draws / n:7.1f}  x{c / n:4.1f}  {k[0]:10s} {k[1]:10s} ps {k[2]}')


def cmd_gaps(frames, totals, idle, a):
    agg = defaultdict(lambda: [0.0, 0, 0.0])
    n = 0
    for fr in sorted(frames):
        if not (a.f0 <= fr <= a.f1):
            continue
        n += 1
        for i, label, box, ps, draws, own, before in frames[fr]:
            k = (label, box)   # the gap belongs to the piece that follows it
            agg[k][0] += before - idle.get((fr, i), 0.0); agg[k][1] += 1; agg[k][2] += own
    if not n:
        sys.exit(f'pass_pieces: no pass-logged frames in {a.f0}-{a.f1} (logged: {min(frames)}-{max(frames)})')
    print(f'frames {n}: real (non-idle) gap time per frame {sum(v[0] for v in agg.values()) / n:.3f} ms')
    for k, v in sorted(agg.items(), key=lambda x: -x[1][0])[:a.top]:
        print(f'{v[0] / n:7.3f} ms gap before  x{v[1] / n:5.1f}  own {v[2] / n:6.3f}  {k[0]:14s} {k[1]}')


def cmd_frame(frames, totals, idle, a):
    if a.frame not in frames:
        sys.exit(f'pass_pieces: frame {a.frame} not pass-logged (logged: {sorted(frames)[:6]} ...)')
    for i, label, box, ps, draws, own, before in frames[a.frame]:
        idl = f' (idle {idle[(a.frame, i)]:.3f})' if (a.frame, i) in idle else ''
        print(f'#{i:3d} {box:>9s} d{draws:>5d} own {own:6.3f} before {before:6.3f}{idl} {label:22s} ps {ps[:8]}')
    if a.frame in totals:
        s, g, sp = totals[a.frame]
        print(f'segments {s:.3f} ms, between {g:.3f} ms, first to last {sp:.3f} ms')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    # (review: --dir is accepted before or after the subcommand)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument('--dir', default=argparse.SUPPRESS, help='log folder (default the main exe logs or LOGS)')
    ap.add_argument('--dir', default=str(DEFAULT_LOGS), help='log folder (default the main exe logs or LOGS)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('scenes', parents=[common]); p.add_argument('run'); p.add_argument('--top', type=int, default=14)
    p = sub.add_parser('gaps', parents=[common]); p.add_argument('run'); p.add_argument('f0', type=int); p.add_argument('f1', type=int)
    p.add_argument('--top', type=int, default=16)
    p = sub.add_parser('frame', parents=[common]); p.add_argument('run'); p.add_argument('frame', type=int)
    a = ap.parse_args()
    data = read(log_path(a.run, a.dir))
    {'scenes': cmd_scenes, 'gaps': cmd_gaps, 'frame': cmd_frame}[a.cmd](*data, a)


if __name__ == '__main__':
    main()
