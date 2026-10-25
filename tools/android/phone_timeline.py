"""GPU-bound or CPU-bound, per 5 s window, from a timeline (--wwe2k14_timeline=true --vulkan_gpu_timestamps=true).

    py tools/android/phone_timeline.py <timeline_NNN.csv> [--window 5]

Per window: frames (cp_swap), fps, GPU ms per frame (sum of gpu_ts submission durations / frames) and GPU load (GPU
busy time / window), and the GPU command thread's time split (ms per frame): wrm (WAIT_REG_MEM: the game's vblank
pacing flag), idle (out of commands: waiting for the game), hwait (waiting for the host GPU), intr (guest interrupt
callbacks), busy (the rest: translating the game's commands). Single pass, streaming (the phone's CSVs are ~60 MB).
GPU load near 100 % = GPU bound; CP busy near the frame time with low GPU load = GPU command thread bound; much idle =
the game's own threads are late (CPU side of the game).
"""
import argparse
import csv
from collections import defaultdict


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('csv')
    ap.add_argument('--window', type=float, default=5.0)
    a = ap.parse_args()
    win_ns = int(a.window * 1e9)
    cp_thread = None
    win = defaultdict(lambda: defaultdict(float))  # window -> key -> value
    open_ = {}
    rows = []
    with open(a.csv, newline='') as f:
        r = csv.reader(f)
        next(r)
        for row in r:
            try:
                t, th, ev = int(row[0]), int(row[1]), row[2]
                b = int(row[4], 16)
            except (ValueError, IndexError):
                break
            if ev == 'cp_swap':
                cp_thread = th
            rows.append((t, th, ev, b))
    rows.sort(key=lambda x: x[0])
    for t, th, ev, b in rows:
        w = t // win_ns
        if ev == 'cp_swap':
            win[w]['frames'] += 1
        elif ev == 'gpu_ts':
            win[w]['gpu_us'] += b
        elif ev in ('wrm_b', 'cp_idle_b', 'hwait_b', 'intr_b') and th == cp_thread:
            open_[ev[:-2]] = t
        elif ev in ('wrm_e', 'cp_idle_e', 'hwait_e', 'intr_e') and th == cp_thread:
            k = ev[:-2]
            t0 = open_.pop(k, None)
            if t0 is not None:
                win[t0 // win_ns][k] += (t - t0) / 1e6
    print(f'{"t(s)":>6} {"fps":>5} {"GPU ms/f":>8} {"GPU load":>8} | CP ms/frame: {"wrm":>5} {"idle":>5} {"hwait":>5} '
          f'{"intr":>5} {"busy":>5}')
    for w in sorted(win):
        d = win[w]
        n = d['frames']
        if not n:
            continue
        frame_ms = a.window * 1000 / n
        waits = d['wrm'] + d['cp_idle'] + d['hwait'] + d['intr']
        busy = max(0.0, a.window * 1000 - waits)
        print(f'{w * a.window:6.0f} {n / a.window:5.1f} {d["gpu_us"] / 1000 / n:8.2f} '
              f'{d["gpu_us"] / 1e4 / a.window:7.0f}% |              {d["wrm"] / n:5.2f} {d["cp_idle"] / n:5.2f} '
              f'{d["hwait"] / n:5.2f} {d["intr"] / n:5.2f} {busy / n:5.2f}   (frame {frame_ms:.1f} ms)')


if __name__ == '__main__':
    main()
