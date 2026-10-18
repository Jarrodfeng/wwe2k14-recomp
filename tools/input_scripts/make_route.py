"""Turn an input recording (--wwe2k14_input_record) into a replay script with file-load sync points.

Usage:
  py tools/input_scripts/make_route.py <recording.txt> --sync PATTERN@FRAME [...] [--shot LABEL@FRAME ...]
                                       [--from FRAME] [--until FRAME] [--timeout N] > route.txt

Keeps the recording's exact pad states (`set` lines, so overlapping presses and hold lengths survive) and its
timing, but re-anchors the timing at each sync point: at the recorded frame of a sync (a file the game opened,
e.g. MNUIHD.PAC@1879) the script emits `wait_file PATTERN TIMEOUT`, and the following waits count from there.
Pick sync points where the pad is at rest (no direction held), because a held state stays held while waiting.
Recorded frames: see summarize_recording.py. `--shot` adds `screenshot LABEL` at a recorded frame.
`--from` drops inputs before that frame (the state is reset to neutral); `--until` stops after that frame.
Output: the script body only (add a header, on_fail, marks and the ending yourself).
"""
import argparse
import sys


def parse(path):
    """-> list of (frame, kind, payload): kind 'set' (payload = rest of the line) or 'file' (payload = path)."""
    out = []
    frame = 0
    for line in open(path, encoding='utf-8'):
        words = line.split()
        if not words:
            continue
        if words[0] == 'wait_frames':
            frame += int(words[1])
        elif words[0] == 'set':
            out.append((frame, 'set', line.strip()))
        elif line.startswith('# frame') and ' opened ' in line:
            out.append((frame, 'file', line.split(' opened ', 1)[1].strip()))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('recording')
    ap.add_argument('--sync', action='append', default=[], help='PATTERN@FRAME (case-insensitive substring)')
    ap.add_argument('--shot', action='append', default=[], help='LABEL@FRAME')
    ap.add_argument('--from', dest='start', type=int, default=0)
    ap.add_argument('--until', type=int, default=None)
    ap.add_argument('--timeout', type=int, default=1800)
    args = ap.parse_args()

    events = parse(args.recording)
    marks = []  # (frame, order, text): order puts syncs before sets and shots at the same frame
    for spec in args.sync:
        pattern, at = spec.rsplit('@', 1)
        at = int(at)
        hits = [f for f, k, p in events if k == 'file' and f == at and pattern.lower() in p.lower()]
        if not hits:
            sys.exit(f'sync {spec}: no file matching {pattern!r} opened at recorded frame {at}')
        marks.append((at, 0, f'wait_file {pattern} {args.timeout}'))
    for spec in args.shot:
        label, at = spec.rsplit('@', 1)
        marks.append((int(at), 2, f'screenshot {label}'))
    for f, k, p in events:
        if k == 'set' and f >= args.start and (args.until is None or f <= args.until):
            marks.append((f, 1, p))
    end = args.until if args.until is not None else float('inf')
    marks = [m for m in marks if args.start <= m[0] <= end]  # syncs/shots outside the range are dropped
    marks.sort(key=lambda m: (m[0], m[1]))

    ref = args.start
    lines = []
    if args.start:
        lines.append('set none 0 0 0 0 0 0')
    for f, order, text in marks:
        if order == 0:  # sync: wait for the load, then count from its recorded frame
            lines.append(f'# recorded frame {f}')
            lines.append(text)
            ref = f
            continue
        if f > ref:
            lines.append(f'wait_frames {f - ref}')
            ref = f
        lines.append(text)
    print('\n'.join(lines))


if __name__ == '__main__':
    main()
