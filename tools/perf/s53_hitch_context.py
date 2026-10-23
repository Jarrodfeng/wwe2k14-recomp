"""Session 53 item 2.7: the longest frames of a run and what the log shows around each (pipeline creations, file
opens, scene changes), to tell pipeline-creation hitches from loading hitches.

    py tools\\perf\\s53_hitch_context.py RUN [--top 15] [--min-ms 50] [--window 0.5] [--logs DIR]

frames_RUN.csv (frame, t_seconds, frame_ms) gives each frame's end time since the first guest frame; the log line
'[wwe2k14][frames] first guest frame' anchors that clock to wall time (else the log's first line).
"""
import argparse
import csv
import datetime as dt
import os
import re

TS = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]')


def parse_ts(line):
    m = TS.match(line)
    return dt.datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S.%f') if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--top', type=int, default=15)
    ap.add_argument('--min-ms', type=float, default=50)
    ap.add_argument('--window', type=float, default=0.5, help='seconds before the long frame to look at')
    ap.add_argument('--logs', default=r'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\logs')
    a = ap.parse_args()
    frames = []
    with open(os.path.join(a.logs, f'frames_{a.run}.csv')) as f:
        for r in csv.DictReader(f):
            frames.append((int(r['frame']), float(r['t_seconds']), float(r['frame_ms'])))
    lines = open(os.path.join(a.logs, f'wwe2k14_{a.run}.log'), encoding='utf-8', errors='replace').read().splitlines()
    # Anchor: the frames csv starts at the first presented frame; the log line that says so, else the first
    # 'VulkanPresenter' swapchain line.
    anchor = None
    for ln in lines:
        if '[frames] first guest frame' in ln:
            anchor = parse_ts(ln)
            break
    if anchor is None:
        anchor = parse_ts(lines[0])
    stamped = [(parse_ts(ln), ln) for ln in lines]
    stamped = [(t, ln) for t, ln in stamped if t]
    long = sorted([fr for fr in frames if fr[2] >= a.min_ms], key=lambda x: -x[2])[:a.top]
    print(f'run {a.run}: {len(frames)} frames, {sum(1 for fr in frames if fr[2] >= a.min_ms)} >= {a.min_ms} ms; '
          f'anchor {anchor}')
    for n, t, ms in sorted(long, key=lambda x: x[1]):
        end = anchor + dt.timedelta(seconds=t)
        start = end - dt.timedelta(milliseconds=ms) - dt.timedelta(seconds=a.window)
        near = [ln for ts, ln in stamped if start <= ts <= end]
        pipes = sum('Creating graphics pipeline' in ln or 'Creating compute pipeline' in ln for ln in near)
        files = [ln for ln in near if re.search(r'\.(pac|bik2|PAC|BIK2|dat|Dat|xmv)\b', ln)]
        scenes = [ln for ln in near if 'scene=' in ln or 'bench:' in ln or 'log ' in ln.lower() and 'wwe2k14][input]' in ln]
        print(f'  frame {n:6d}  t={t:8.2f}s  {ms:7.1f} ms  pipelines {pipes:3d}  file lines {len(files):3d}')
        for ln in (files[:3] + scenes[:2]):
            print('      ' + ln[26:200])


if __name__ == '__main__':
    main()
