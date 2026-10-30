# Session 73 (Jarrod: "the first second or two of everything, entrances, matches starting, runs in slow motion, before
# speeding up ... even on my phone"): game speed right after each scene change, from a game log's [scene] / [match] /
# [step] lines and its per-frame CSV (frames_NNN.csv: frame, t, ms). The game steps a fixed 1/60 s (word 60) or 1/30 s
# (word 30) per frame, so speed = the frame budget / the frame's real time (1.0 = real time).
#   py tools\android\scene_start_speed.py LOGS_DIR [RUN ...]     (RUN = 083 etc; default: every real-time run)
import csv
import os
import re
import statistics
import sys

SCENE = re.compile(r'\[(?:scene|match)\] (.*?) at frame (\d+)')
WORD = re.compile(r'frame-rate word (?:\d+ -> )?(\d+)\D.*?(?:frame (\d+)|at frame (\d+))')


def load_frames(path):
    out = {}
    with open(path, newline='') as f:
        for row in csv.reader(f):
            try:
                out[int(row[0])] = float(row[2])
            except (ValueError, IndexError):
                pass
    return out


def main():
    d = sys.argv[1]
    runs = sys.argv[2:] or sorted(m.group(1) for m in (re.match(r'frames_(\d+)\.csv$', n) for n in os.listdir(d)) if m)
    for run in runs:
        csv_path = os.path.join(d, 'frames_%s.csv' % run)
        log_path = os.path.join(d, 'wwe2k14_%s.log' % run)
        if not (os.path.exists(csv_path) and os.path.exists(log_path)):
            continue
        frames = load_frames(csv_path)
        if len(frames) < 600:
            continue
        med = statistics.median(frames.values())
        if med < 12.0:  # fast-forwarded
            continue
        events = []
        with open(log_path, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = SCENE.search(line)
                if m:
                    events.append((int(m.group(2)), m.group(1)[:90]))
        print('== run %s: %d frames, median %.1f ms' % (run, len(frames), med))
        for frame, what in events:
            # Speed over 0.5 s windows from the change, budget 16.7 ms (30 fps scenes show as 0.5 when on time).
            cells = []
            n = frame + 1
            for _ in range(6):
                t = 0.0
                count = 0
                while t < 500.0 and n in frames:
                    if frames[n] < 2000:
                        t += frames[n]
                        count += 1
                    n += 1
                if not count:
                    break
                cells.append('%.2f' % (count * 16.667 / t))
            print('  frame %6d %-90s speed/0.5s: %s' % (frame, what, ' '.join(cells)))


if __name__ == '__main__':
    main()
