"""Session 102 lane S0-2: heat on the record. The one rule that tags a Surface run "steady" or "not steady" from its own heat record,
the stale-power.csv guard, and the fixed-rate-counter-as-clock guard. Mirrored in PowerShell by tools\\surface\\heat_steady.ps1 (the bench runs
on the Surface, which has no Python); tools\\tests\\heat_rules_test.py checks both give the same verdict on the same files.

  py -I tools\\surface\\heat_rules.py STEP_FOLDER [--log wwe2k14_NNN.log] [--prev PREVIOUS_power.csv]
      prints "steady" / "not steady: reason" (+ " | vs previous step: ..." with --prev), exit 0 / 1. Without --log the newest wwe2k14_*.log
      in the folder BY MTIME is used for the freshness check.
  py -I tools\\surface\\heat_rules.py --csv power.csv --log LOG [--prev PREVIOUS_power.csv]
  Tests: py -I tools\\tests\\heat_s02_test.py (gate lane s102_s0_2).

THE RULE (one place, the numbers are the S0-2 pick; recalibrate on the Surface baseline, S0-5):
  rows      power.csv, one per second. Drop the first WARMUP_S seconds. Need at least MIN_ROWS rows left, else "not steady: too short".
  limit     perf_limit (% Performance Limit, the standing cap: 88 on the Surface Pro 6): the median minus the minimum must be <= LIMIT_DROP
            points, and perf_limit_flags must not change value. A dip or a flag change = the platform throttled during the run.
  drift     cpu_perf (% Processor Performance): second-half mean over first-half mean within +-DRIFT (the heat effect; the scene's own load
            moves it too, hence a wide band: R5 saw Bryan 139 -> 120 = -14 % with no throttle).
  power     rapl_pkg (milliwatts): when present, second-half mean over first-half within +-PKG_DRIFT.
  no record cpu_perf column missing or empty = "not steady: no heat record" (a run is never steady by default).
"""
import csv
import math
import os
import sys

WARMUP_S = 10
MIN_ROWS = 20
LIMIT_DROP = 5.0
DRIFT = 0.20
PKG_DRIFT = 0.35
STALE_S = 120  # power.csv last written more than this before / after the run's log = not this run's file


class Refused(Exception):
    """A heat file or reading that must not be used (stale, or a fixed-rate counter fed as a clock)."""


def _col(rows, name):
    out = []
    for r in rows:
        v = r.get(name, '')
        try:
            x = float(v)
        except (TypeError, ValueError):
            continue
        if math.isfinite(x):  # S0-2 review F1: nan / inf are not readings (float() takes "nan"; Windows PowerShell 5.1 does not: both drop them)
            out.append(x)
    return out


def _mean(xs):
    return sum(xs) / len(xs) if xs else None


def _median(xs):
    s = sorted(xs)
    n = len(s)
    return None if not n else (s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2]))


def check_clock_series(values, name):
    """A series fed as a clock must move. cpu_freq_mhz reads the nominal 1600.00 in every row, and a thread cycle counter runs at a fixed
    rate: both read constant against load. Refuses a series whose spread is under 0.5 % of its mean (or fewer than 3 values)."""
    v = [x for x in values if x is not None]
    if len(v) < 3:
        raise Refused(f'{name}: fewer than 3 readings, not a clock')
    m = _mean(v)
    if m == 0 or (max(v) - min(v)) <= 0.005 * abs(m):
        raise Refused(f'{name}: constant ({min(v):g}..{max(v):g}) = a fixed-rate counter, not a clock')
    return m


def check_clock_vs_load(pairs, name, min_load_ratio=2.0, min_move=0.05):
    """pairs: (load, clock) per sample, e.g. (process CPU %, thread cycles / cpu_ms). A real clock moves with load on a power-capped chip
    (it turbos when few cores are busy, sags when all are). R5 found the thread-cycle "GHz" at 1.75 with 88 % of a core busy and 1.80 with 545 %:
    refuses a series whose mean in the lightest third of the samples is within min_move of the heaviest third's while the load differs by
    min_load_ratio or more. (A flat series with no load spread cannot be judged and is refused too.)"""
    p = sorted((l, c) for l, c in pairs if l is not None and c is not None)
    if len(p) < 6:
        raise Refused(f'{name}: fewer than 6 (load, clock) pairs, cannot tell a clock from a fixed-rate counter')
    t = len(p) // 3
    lo, hi = p[:t], p[-t:]
    lo_load, hi_load = _mean([l for l, _ in lo]), _mean([l for l, _ in hi])
    lo_c, hi_c = _mean([c for _, c in lo]), _mean([c for _, c in hi])
    if lo_load <= 0 or hi_load / lo_load < min_load_ratio:
        raise Refused(f'{name}: the load barely varies ({lo_load:g}..{hi_load:g}), cannot show the series moves with it')
    if abs(hi_c / lo_c - 1.0) < min_move:
        raise Refused(f'{name}: {lo_c:.2f} at light load vs {hi_c:.2f} at {hi_load / lo_load:.0f}x the load = does not move with load: a fixed-rate counter, not a clock')
    return hi_c / lo_c


def check_fresh(csv_path, log_path, stale_s=STALE_S):
    """power.csv must have been written by the run whose log sits beside it: its mtime within stale_s of the log's. Else Refused."""
    if not os.path.isfile(csv_path):
        raise Refused(f'{csv_path}: missing')
    if not log_path or not os.path.isfile(log_path):
        raise Refused(f'{csv_path}: no run log to compare its age with')
    d = abs(os.path.getmtime(csv_path) - os.path.getmtime(log_path))
    if d > stale_s:
        raise Refused(f'{csv_path}: stale, written {d:.0f} s apart from the run log (limit {stale_s} s): another run\'s file')


def retryable(reason):
    """Only a heat reason justifies repeating a run (review F3). Too short / no heat record / refused say nothing about heat: the run itself
    is the problem (crashed, timed out, no power file) and a repeat would not fix it."""
    return reason.startswith('not steady: ') and any(k in reason for k in ('perf_limit fell', 'perf_limit_flags changed', 'drifted'))


def _steady_means(rows):
    rows = [r for r in rows if _col([r], 't_s') and _col([r], 't_s')[0] >= WARMUP_S]
    return _mean(_col(rows, 'cpu_perf')), _mean(_col(rows, 'rapl_pkg'))


def vs_previous(rows, prev_rows):
    """The absolute reference (review item 6): this step's mean cpu_perf and package power (after the warm-up) against the previous step's.
    The within-run rule cannot see a chip that was already hot when the step began; this can. Returns a text, '' when either side has no record."""
    a, b = _steady_means(rows), _steady_means(prev_rows)
    parts = []
    if a[0] and b[0]:
        parts.append(f'cpu_perf {100 * (a[0] / b[0] - 1):+.0f} %')
    if a[1] and b[1]:
        parts.append(f'package {100 * (a[1] / b[1] - 1):+.0f} %')
    return 'vs previous step: ' + ', '.join(parts) if parts else ''


def verdict(rows):
    """rows: list of dicts (power.csv). Returns (steady: bool, reason: str)."""
    rows = [r for r in rows if r.get('t_s') not in (None, '')]
    rows = [r for r in rows if _col([r], 't_s') and _col([r], 't_s')[0] >= WARMUP_S]
    if len(rows) < MIN_ROWS:
        return False, f'not steady: too short ({len(rows)} rows after the {WARMUP_S} s warm-up, need {MIN_ROWS})'
    perf = _col(rows, 'cpu_perf')
    if len(perf) < MIN_ROWS:
        return False, 'not steady: no heat record (no cpu_perf column)'
    lim = _col(rows, 'perf_limit')
    if lim:
        drop = _median(lim) - min(lim)
        if drop > LIMIT_DROP:
            return False, f'not steady: perf_limit fell {drop:.0f} points below its median (throttled)'
    fl = sorted(set(_col(rows, 'perf_limit_flags')))
    if len(fl) > 1:
        return False, f'not steady: perf_limit_flags changed ({", ".join(f"{x:g}" for x in fl)})'
    h = len(perf) // 2
    a, b = _mean(perf[:h]), _mean(perf[h:])
    if a and abs(b / a - 1.0) > DRIFT:
        return False, f'not steady: cpu_perf drifted {100 * (b / a - 1):+.0f} % first to second half (band +-{100 * DRIFT:.0f} %)'
    pk = _col(rows, 'rapl_pkg')
    if len(pk) >= MIN_ROWS:
        h = len(pk) // 2
        a, b = _mean(pk[:h]), _mean(pk[h:])
        if a and abs(b / a - 1.0) > PKG_DRIFT:
            return False, f'not steady: package power drifted {100 * (b / a - 1):+.0f} % (band +-{100 * PKG_DRIFT:.0f} %)'
    return True, 'steady'


def read_rows(path):
    with open(path, newline='', encoding='utf-8') as f:
        return list(csv.DictReader(f))


def main(argv):
    csv_path = None
    log = None
    prev = None
    i = 0
    folder = None
    while i < len(argv):
        if argv[i] == '--csv':
            csv_path = argv[i + 1]; i += 2
        elif argv[i] == '--log':
            log = argv[i + 1]; i += 2
        elif argv[i] == '--prev':
            prev = argv[i + 1]; i += 2
        else:
            folder = argv[i]; i += 1
    if folder:
        csv_path = csv_path or os.path.join(folder, 'power.csv')
        if not log:
            logs = sorted((os.path.join(folder, f) for f in os.listdir(folder) if f.startswith('wwe2k14_') and f.endswith('.log')),
                          key=os.path.getmtime)
            log = logs[-1] if logs else None
    if not csv_path:
        print('usage: heat_rules.py STEP_FOLDER | --csv power.csv [--log LOG]')
        return 2
    try:
        check_fresh(csv_path, log)
        rows = read_rows(csv_path)
        ok, why = verdict(rows)
    except Refused as e:
        print(f'not steady: refused: {e}')
        return 1
    if prev and os.path.isfile(prev):
        vp = vs_previous(rows, read_rows(prev))
        if vp:
            why += ' | ' + vp
    print(why)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
