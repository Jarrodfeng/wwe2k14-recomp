# Session 73 (scene starts in slow motion): game speed in the first seconds after each load-turbo window ends (the
# moment a scene becomes visible: "[wwe2k14][turbo] ... off at frame N"), from a run's log and frames CSV. Speed =
# game time / real time over 0.5 s windows (fixed 1/60 s step per frame at word 60); also the frames of 30+ ms.
#   py tools\perf\scene_start_ab.py LOG_DIR RUN [RUN ...]
#   py tools\perf\scene_start_ab.py --selftest
# S111 WM30LOAD: a 30 Years season-video run has two "ingame" offs (the video's first frame and, after the re-opened
# window, the titantron); each is a scene start and each is reported.
import csv
import os
import re
import sys

OFF = re.compile(r'\[turbo\] (ingame|match) off at frame (\d+)')


def scene_starts(lines):
    """(window, frame) of every in-game window end, in log order, and the late-frame threshold."""
    starts, threshold = [], 'game'
    for line in lines:
        m = OFF.search(line)
        if m:
            starts.append((m.group(1), int(m.group(2))))
        if 'late-frame threshold set to' in line:
            threshold = line.split('late-frame threshold set to ')[1].split(' ')[0]
    return starts, threshold


def selftest():
    log = [
        '[2026-11-26 10:00:01.000] [info] [wwe2k14][turbo] ingame on at frame 4064',
        '[2026-11-26 10:00:01.037] [info] [wwe2k14][turbo] ingame off at frame 4092 (movie GAME:\\movies\\season\\0412.bik2; 28 frames, 37 ms)',
        '[2026-11-26 10:01:31.000] [info] [wwe2k14][turbo] the 30 Years season card is over (flow state 30 left at frame 185050): the in-game load window opens again',
        '[2026-11-26 10:01:31.000] [info] [wwe2k14][turbo] ingame on at frame 185050',
        '[2026-11-26 10:01:32.000] [info] [wwe2k14][turbo] ingame off at frame 185300 (movie GAME:\\movies\\titantron\\344.bik2; 250 frames, 1000 ms)',
        '[2026-11-26 10:02:00.000] [info] [wwe2k14][turbo] match on at frame 186000',
        '[2026-11-26 10:02:01.000] [info] [wwe2k14][turbo] match off at frame 186100 (slot 0 motion; 100 frames, 1000 ms)',
    ]
    starts, _ = scene_starts(log)
    want = [('ingame', 4092), ('ingame', 185300), ('match', 186100)]
    ok = starts == want
    if not ok:
        print('FAIL two ingame windows: got %s, want %s' % (starts, want))
    print('scene_start_ab selftest:', 'PASS' if ok else 'FAIL')
    return 0 if ok else 1


def main():
    if sys.argv[1:] == ['--selftest']:
        sys.exit(selftest())
    d = sys.argv[1]
    for run in sys.argv[2:]:
        frames = {}
        with open(os.path.join(d, 'frames_%s.csv' % run), newline='') as f:
            for row in csv.reader(f):
                if row and row[0].isdigit():
                    frames[int(row[0])] = float(row[2])
        with open(os.path.join(d, 'wwe2k14_%s.log' % run), encoding='utf-8', errors='replace') as f:
            starts, threshold = scene_starts(f)
        print('== run %s (late-frame threshold %s)' % (run, threshold))
        for kind, start in starts:
            cells, n, slow, total_ms, count = [], start, 0, 0.0, 0
            for _ in range(4):
                t, c = 0.0, 0
                while t < 500.0 and n in frames:
                    t += frames[n]
                    c += 1
                    slow += frames[n] >= 30.0
                    n += 1
                if not c:
                    break
                total_ms += t
                count += c
                cells.append('%.2f' % (c * 16.667 / t))
            print('  %-6s frame %5d: speed per 0.5 s %s | first 2 s: %.2f, frames >= 30 ms: %d' % (
                kind, start, ' '.join(cells), count * 16.667 / total_ms if total_ms else 0, slow))


if __name__ == '__main__':
    main()
