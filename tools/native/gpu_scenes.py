"""GPU ms and command-thread busy ms per frame by scene, several runs side by side (Session 52).

For separate runs of one route (not an in-run A/B): the timeline's CP frames (--vulkan_gpu_timestamps=true, the
timeline on) grouped by scene from the file opens (--wwe2k14_file_trace=true): Brock's titantron 343.bik2, Bryan's
324.bik2, the match's M3.PAC. Median and mean of each scene's frames (the first 30 after a scene starts left out).
Between runs this compares different moments of the GPU's clock state: an in-run A/B (ab_gpu.py) is the measure;
this is for finding where to look.

    py tools\\native\\gpu_scenes.py RUN [RUN ...] [--metric gpu_ms|busy]
"""
import argparse
import os
import re
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'bench'))
import bench_report  # noqa: E402

LOGS = os.path.join(HERE, '..', '..', 'out', 'build', 'win-amd64-release', 'logs')
OPEN = re.compile(r"\[file\] NtCreateFile '([^']+)'.* frame=(\d+)")
SCENES = (('343.bik2', 'brock'), ('324.bik2', 'bryan'), ('m3.pac', 'match'))


def scenes_of(run):
    starts, seen = [], set()
    with open(os.path.join(LOGS, f'wwe2k14_{run:03d}.log'), encoding='utf-8', errors='replace') as f:
        for line in f:
            m = OPEN.search(line)
            if m:
                p = m.group(1).lower()
                for key, name in SCENES:
                    if p.endswith(key) and name not in seen:
                        seen.add(name)
                        starts.append((int(m.group(2)), name))
    return starts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('runs', type=int, nargs='+')
    ap.add_argument('--metric', default='gpu_ms')
    ap.add_argument('--skip', type=int, default=30)
    a = ap.parse_args()
    print(f'{"run":>6} ' + ' '.join(f'{n + " med":>11} {n + " mean":>11}' for _, n in SCENES))
    for run in a.runs:
        starts = scenes_of(run)
        ev = bench_report.load_timeline(os.path.join(LOGS, f'timeline_{run:03d}.csv'))
        frames, _ = bench_report.build_cp_frames(ev)
        per = {n: [] for _, n in SCENES}
        for fr in frames:
            g = fr.get('guest_frame', fr.get('frame'))
            v = fr.get(a.metric)
            if g is None or v is None:
                continue
            sc = None
            for f0, n in starts:
                if f0 + a.skip <= g:
                    sc = n
                elif f0 <= g:
                    sc = None
            if sc:
                per[sc].append(v)
        cells = []
        for _, n in SCENES:
            x = per[n]
            cells.append(f'{statistics.median(x):11.3f} {statistics.fmean(x):11.3f}' if x else f'{"-":>11} {"-":>11}')
        print(f'{run:>6} ' + ' '.join(cells))


if __name__ == '__main__':
    main()
