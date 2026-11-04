r"""Whole-frame GPU busy from a run's timeline (Session 89; promoted from Session 87's win.py and ts.py): the
`gpu_ts` events of timeline_NNN.csv (a = submission, b = its GPU span in us, c = 1 on a frame's last submission;
needs the submission stamps: --vulkan_gpu_timestamps=true) summed per frame.

    py -I tools\native\frame_gpu_busy.py busy RUN [RUN...] --window NAME:F0:F1 [--window ...]
        mean / median GPU busy us per frame in each window, one column per run
    py -I tools\native\frame_gpu_busy.py frames RUN F0 F1
        each frame's submissions (submission, us) and their sum

RUN = a run number (timeline in --dir, default the main exe's logs or LOGS=) or a CSV path. Frame numbers here count
frames that ended a submission in the timeline (from 1), not the guest frame: check against a known scene mark first.
Windows include both ends (Session 87's win.py stopped before F1: its means differ by one frame's share).
"""
import argparse
import csv
import os
import statistics as st
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LOGS = Path(os.environ.get('LOGS') or ROOT / 'out' / 'build' / 'win-amd64-release' / 'logs')


def timeline(run, logdir):
    p = Path(run)
    if p.suffix == '.csv' and p.exists():
        return p
    q = Path(logdir) / f'timeline_{int(run):03d}.csv'
    if not q.exists():
        sys.exit(f'frame_gpu_busy: no timeline {q}')
    return q


def per_frame(path):
    """{frame: [(submission, us), ...]} from the gpu_ts events."""
    out, acc, fr = {}, [], 0
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            if r['event'] != 'gpu_ts':
                continue
            acc.append((int(r['a'], 16), int(r['b'], 16)))
            if int(r['c'], 16):
                fr += 1; out[fr] = acc; acc = []
    if not out:
        sys.exit(f'frame_gpu_busy: no gpu_ts events in {path} (submission stamps off in this run?)')
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    common = argparse.ArgumentParser(add_help=False)   # (review: --dir before or after the subcommand)
    common.add_argument('--dir', default=argparse.SUPPRESS)
    ap.add_argument('--dir', default=str(DEFAULT_LOGS))
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('busy', parents=[common]); p.add_argument('runs', nargs='+'); p.add_argument('--window', action='append', required=True)
    p = sub.add_parser('frames', parents=[common]); p.add_argument('run'); p.add_argument('f0', type=int); p.add_argument('f1', type=int)
    a = ap.parse_args()
    if a.cmd == 'frames':
        fr = per_frame(timeline(a.run, a.dir))
        for i in range(a.f0, a.f1 + 1):
            if i in fr:
                print(i, fr[i], 'sum', sum(u for _, u in fr[i]))
        return
    data = {r: per_frame(timeline(r, a.dir)) for r in a.runs}
    print('GPU busy per frame (sum of submission spans, us): mean / median (frames)')
    for w in a.window:
        name, f0, f1 = w.rsplit(':', 2)
        line = f'{name:18s}'
        for r in a.runs:
            v = [sum(u for _, u in data[r][i]) for i in range(int(f0), int(f1) + 1) if i in data[r]]
            line += f' | {r}: ' + (f'{st.mean(v):7.0f} / {st.median(v):7.0f} (n {len(v)})' if v else 'no frames')
        print(line)


if __name__ == '__main__':
    main()
