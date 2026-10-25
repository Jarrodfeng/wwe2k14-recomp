"""In-run A/B of a GPU setting by GPU time per frame (Session 40).

A route switches a setting on and off every N frames and logs "ab: mode M" (M = 1 on, 0 off; any digit works), with
--wwe2k14_file_trace=true so the scenes are known from the file opens (Brock's titantron 343.bik2, Bryan's 324.bik2,
the match's M3.PAC), and --vulkan_gpu_timestamps=true for the GPU time of every CP frame (bench_report.py). Each
window's mean GPU ms per frame (the first SKIP frames after a switch left out) is compared with the windows of the
other mode right before and after it in the same scene (paired: neighbouring windows see nearly the same shots).
Per scene: the median change over the pairs and the middle half of the pairs' changes.

    py tools\\native\\ab_gpu.py RUN [--on 1] [--skip 10]

Fast-forwarded runs are fine (GPU ms are the GPU's own). Numbers only. Session 43: --metric busy compares the command
thread's busy ms per frame instead (the CPU side of a setting).
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
MODE = re.compile(r'\[input\] line \d+: ab: mode (\d) \(frame (\d+)\)')
OPEN = re.compile(r"\[file\] NtCreateFile '([^']+)'.* frame=(\d+)")
SCENES = (('343.bik2', 'tron_343 (Brock)'), ('324.bik2', 'tron_324 (Bryan)'), ('103.bik2', 'tron_103 (Undertaker)'), ('m3.pac', 'match'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run', type=int)
    ap.add_argument('--on', default='1', help='the mode digit that means "on"')
    ap.add_argument('--skip', type=int, default=10)
    ap.add_argument('--metric', default='gpu_ms',
                    help='Session 43: the per-frame number compared: gpu_ms, or busy / busy_nd (command thread busy ms)')
    a = ap.parse_args()
    log = os.path.join(LOGS, f'wwe2k14_{a.run:03d}.log')
    switches, scene_starts = [], []
    seen = set()
    with open(log, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = MODE.search(line)
            if m:
                switches.append((int(m.group(2)), m.group(1)))
                continue
            m = OPEN.search(line)
            if m:
                path = m.group(1).lower()
                for key, name in SCENES:
                    if path.endswith(key) and name not in seen:
                        seen.add(name)
                        scene_starts.append((int(m.group(2)), name))
    ev = bench_report.load_timeline(os.path.join(LOGS, f'timeline_{a.run:03d}.csv'))
    frames, meta = bench_report.build_cp_frames(ev)
    if not frames or (a.metric == 'gpu_ms' and 'gpu_ms' not in frames[0]):
        sys.exit('no CP frames with GPU time (--vulkan_gpu_timestamps=true?)')

    def at(table, frame):
        cur = None
        for f0, v in table:
            if f0 <= frame:
                cur = (f0, v)
        return cur

    # Windows: (scene, mode, [gpu ms]) in time order.
    windows = []
    for fr in frames:
        g = fr.get('guest_frame')
        if g is None or fr.get(a.metric) is None:
            continue
        sw = at(switches, g)
        sc = at(scene_starts, g)
        if not sw or not sc or g - sw[0] < a.skip:
            continue
        key = (sc[1], sw[1], sw[0])
        if not windows or windows[-1][0] != key:
            windows.append((key, []))
        windows[-1][1].append(fr[a.metric])
    print(f'run {a.run}: {len(switches)} switches, {len(windows)} windows, scenes {[s for _, s in scene_starts]}')
    by_scene = {}
    for (scene, mode, _), vals in windows:
        if len(vals) >= 20:
            by_scene.setdefault(scene, []).append((mode, statistics.mean(vals), len(vals)))
    print(f'{"scene":<18} {"pairs":>5} {"off ms":>7} {"on ms":>7} {"change":>8} {"middle half":>18}')
    for scene, ws in by_scene.items():
        changes = []
        for i, (mode, ms, _) in enumerate(ws):
            if mode != a.on:
                continue
            near = [w[1] for w in (ws[i - 1] if i else None, ws[i + 1] if i + 1 < len(ws) else None)
                    if w and w[0] != a.on]
            if near:
                base = statistics.mean(near)
                changes.append(100.0 * (ms - base) / base)
        off = [w[1] for w in ws if w[0] != a.on]
        on = [w[1] for w in ws if w[0] == a.on]
        if not changes or not off or not on:
            continue
        q = statistics.quantiles(changes, n=4) if len(changes) >= 4 else [min(changes), 0, max(changes)]
        print(f'{scene:<18} {len(changes):>5} {statistics.mean(off):>7.3f} {statistics.mean(on):>7.3f} '
              f'{statistics.median(changes):>+7.1f}% {q[0]:>+8.1f} .. {q[-1]:>+6.1f}%')


if __name__ == '__main__':
    main()
