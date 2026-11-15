#!/usr/bin/env python3
"""S102 TS1: read the GPU-watchdog reports a Surface job runner (s0_5_jobs.ps1 -> wer_lib.ps1) saved beside a job's results, and say whether they match the run logs.

  py -I tools\\surface\\wer_report.py FOLDER          (also used by bench_line.py and ab_line.py: they print the line below under their own output)

A job folder holds  <job>\\watchdog\\reports.txt  (lines  REP|code|event time (Surface local)|dump name|WER folder|staged dump  from wer_scan.ps1) and the dumps themselves
(local only: kernel dumps of the Surface, never committed). FOLDER is the job folder, or any folder below it (an unzipped step / arm folder); the
nearest `watchdog` folder above (or one level below) is used. The logs `wwe2k14_*.log` below the job folder give the `[rdr][DEVICE_LOST] =====` times.

Prints one line, or nothing when the folder has no watchdog listing and no DEVICE_LOST line:
  GPU watchdog: 2 reports (2 events; 141 x 2) | log DEVICE_LOST: 2, each with a report within -30..+120 s
  GPU watchdog: 0 reports (listed) | log DEVICE_LOST: 1 -> NO REPORT FOR 1 (a loss without a watchdog report: the loss was not a GPU timeout, or the listing missed it)
  GPU watchdog: 1 report (1 event; 141 x 1) | log DEVICE_LOST: 0 -> 1 REPORT WITHOUT A LOG LOSS (the GPU timed out, our log did not see the device lost)
The time rule is measured (2026-11-15: the log's loss line 05:20:47.36, the report's EventTime 05:21:00): the report lands 0-15 s after the log line.
"""
import re, sys
from datetime import datetime, timedelta
from pathlib import Path

LOST = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)(?:\.\d+)?\].*\[rdr\]\[DEVICE_LOST\] =====')
BEFORE_S, AFTER_S = 30, 120          # a report matches a loss when it is -30 .. +120 s from the log line
EVENT_GAP_S = 30                     # reports closer than this (same code) are one event (the Surface queued 2-3 per loss)


def parse_reports(text):
    """REP lines -> [{'code','time','dump','dir','staged'}]; '#' / NOW / WARN lines are skipped; a malformed REP line raises ValueError (never silently dropped)."""
    out = []
    for ln in text.splitlines():
        if not ln.startswith('REP|'):
            continue
        f = ln.split('|')
        if len(f) != 6:
            raise ValueError(f'bad REP line (want 6 fields): {ln[:120]}')
        out.append({'code': f[1], 'time': datetime.strptime(f[2], '%Y-%m-%dT%H:%M:%S'), 'dump': f[3], 'dir': f[4], 'staged': f[5]})
    return out


def find_watchdog(folder):
    """The nearest watchdog\\reports.txt: the folder itself, its parents (3 levels), then one level below. None when there is none."""
    p = Path(folder).resolve()
    for q in [p] + list(p.parents)[:3]:
        if (q / 'watchdog' / 'reports.txt').is_file():
            return q / 'watchdog' / 'reports.txt'
    for q in sorted(p.glob('*/watchdog/reports.txt')):
        return q
    return None


def lost_times(folder):
    """The `[rdr][DEVICE_LOST] =====` times of every wwe2k14_*.log under folder (one per lost run)."""
    out = []
    for lg in sorted(Path(folder).rglob('wwe2k14_*.log')):
        try:
            with open(lg, encoding='utf-8', errors='replace') as fh:
                for ln in fh:
                    m = LOST.match(ln)
                    if m:
                        out.append(datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S'))
        except OSError:
            pass
    return out


def events(reps):
    ev = []
    for r in sorted(reps, key=lambda r: r['time']):
        if ev and ev[-1]['code'] == r['code'] and (r['time'] - ev[-1]['t1']).total_seconds() <= EVENT_GAP_S:
            ev[-1]['t1'] = r['time']
            ev[-1]['n'] += 1
        else:
            ev.append({'code': r['code'], 't0': r['time'], 't1': r['time'], 'n': 1})
    return ev


def matched(lost, reps):
    """How many of the lost times have a report from -BEFORE_S to +AFTER_S seconds."""
    lo, hi = timedelta(seconds=-BEFORE_S), timedelta(seconds=AFTER_S)
    return sum(1 for t in lost if any(lo <= r['time'] - t <= hi for r in reps))


def summary_line(folder):
    """The one-line summary (see the module doc) or None. Raises ValueError for a malformed reports.txt."""
    wd = find_watchdog(folder)
    if wd is None:
        lost = lost_times(folder)
        return (f'GPU watchdog: no listing (the job ran before the runner listed reports) | log DEVICE_LOST: {len(lost)}' if lost else None)
    reps = parse_reports(wd.read_text(encoding='utf-8', errors='replace'))
    job = wd.parent.parent                              # the job folder: all its logs, whatever folder was asked about
    lost = lost_times(job)
    ev = events(reps)
    codes = {}
    for r in reps:
        codes[r['code']] = codes.get(r['code'], 0) + 1
    cs = '; '.join(f'{c} x {n}' for c, n in sorted(codes.items())) or 'none'
    s = f'GPU watchdog: {len(reps)} report{"" if len(reps) == 1 else "s"} ' + (f'({len(ev)} event{"" if len(ev) == 1 else "s"}; {cs})' if reps else '(listed)')
    m = matched(lost, reps)
    s += f' | log DEVICE_LOST: {len(lost)}'
    if lost and m == len(lost):
        s += f', each with a report within -{BEFORE_S}..+{AFTER_S} s'
    elif lost:
        s += f' -> NO REPORT FOR {len(lost) - m} (a loss without a watchdog report: not a GPU timeout, or the listing missed it)'
    lo, hi = timedelta(seconds=-AFTER_S), timedelta(seconds=BEFORE_S)
    free = [e for e in ev if not any(lo <= e['t0'] - t <= hi or lo <= e['t1'] - t <= hi for t in lost)]
    if free:
        s += f' -> {len(free)} EVENT{"" if len(free) == 1 else "S"} WITHOUT A LOG LOSS (the GPU timed out, our log did not see the device lost; at ' + ', '.join(e['t0'].strftime('%H:%M:%S') for e in free[:4]) + ')'
    return s


def print_wer(folder):
    """For bench_line.py / ab_line.py: print the summary line, or a warning when reports.txt is malformed (the reader's own numbers are never held back)."""
    try:
        s = summary_line(folder)
    except (ValueError, OSError) as e:
        s = f'GPU watchdog: reports.txt unreadable ({e})'
    if s:
        print(s)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    try:
        s = summary_line(sys.argv[1])
    except ValueError as e:
        print(f'REFUSED: {e}', file=sys.stderr)
        return 2
    if s:
        print(s)
    return 0


if __name__ == '__main__':
    sys.exit(main())
