"""S102 S0-4: the verdict of an in-run on / off run of one switch (tools\\bench\\make_ab_route.py makes the route).

  py -I tools\\bench\\ab_verdict.py RUNLOG [--frames FRAMES.csv] [--switch NAME] [--on VALUE] [--settle 30] [--tail 60] [--ci 98]
                                    [--untabled-ok] [--json]

WINDOWS are cut at the route's own lines `ab: window k/N ON|OFF (frame F)` (the log step, src\\input_script.h), the last one ends at the
`scene=ab_end` line (else the end of the frames file minus --tail). Not at the `[switch]` lines: an ABBA route's second OFF is no change and
writes no line. A run log without `ab:` lines (a hand-made route) falls back to the `[switch]` lines, arms by equality with --on.

PROOFS the verdict demands before it prints a number (exit 2 otherwise, never a quiet 0):
  - every window that changes the arm has a `[switch] frame N NAME=VALUE` line with the right value within a few frames of the window start
    (the flip happened); the first window needs one unless the table default already equals its value;
  - every table switch line has a reader's acknowledgement `[switch] seen frame N gen G NAME=VALUE` (rdr_switches.h Seen: the read site
    saw the new value). A switch that nothing re-reads has none. `(not in the table)` lines can never be acknowledged: exit 2 unless
    --untabled-ok, then every number is printed as UNVERIFIED;
  - the frames file is the log's own (its name from the log line, else the log's number; never "any frames*.csv in the folder") and agrees with
    one `[wwe2k14][frames] t=.. frames a-b: .. avg X ms` summary line of the log;
  - entrance routes: no window starts later than step 15 + the bound the route logs (`ab: bound step15+N`), from the `log_state` lines.

STATISTICS: windows alternate. For each ON window, d_k = ON_k - mean(the OFF windows next to it). With n >= 4 such pairs the mean of d has a
t-based confidence interval (--ci 90 | 95 | 98 | 99, default 98: the default that meets both simulated targets below with 4 pairs); "no difference" when the interval covers 0 or |mean d| is under a floor (0.05 ms /
0.2 fps). Fewer than 4 pairs: exit 2. (Simulated: 8 windows, per-window noise 0.5 ms, no effect -> "no difference" in >= 95 % of runs;
a 2 ms effect is found in >= 80 %; ab_verdict_selftest.py runs both.)
Sides: shown fps (sum of `presented` over the window / its seconds), GPU work (mean filled `gpu_ms`), swap thread (mean `game_cpu_ms`), render
thread (mean `render_ms`, a column S0-3 adds; n/a until then).
"""
import argparse
import csv
import json
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import switch_table

SWITCH_RE = re.compile(r'\[switch\] frame (\d+) (\S+?)=(\S*)( \(not in the table\))?\s*$')
SEEN_RE = re.compile(r'\[switch\] seen frame (\d+) gen (\d+) (\S+?)=(-?\d+)\s*$')
AB_RE = re.compile(r'ab: window (\d+)/(\d+) (ON|OFF)\b.*\(frame (\d+)\)\s*$')
BOUND_RE = re.compile(r'ab: bound step15\+(\d+)')
END_RE = re.compile(r'scene=ab_end.*\(frame (\d+)\)')
HANDOFF_RE = re.compile(r'handoff_memory in_memory=(\d+)')   # S102 T8 review F1: the 600-frame [rdr][live] line's count of frames handed over in memory
STATE_RE = re.compile(r'state entrance_step_0 = (-?\d+).*\(frame (\d+)\)\s*$')
SUMMARY_RE = re.compile(r'\[wwe2k14\]\[frames\] t=\S+ s, frames (\d+)-(\d+): [\d.]+ fps, avg ([\d.]+) ms')
CSV_RE = re.compile(r'per-frame CSV: (.+?)\)?\s*$')
RENDER_COLUMNS = ('render_ms', 'render_thread_ms', 'render_thread_cpu_ms')
FLOOR_MS, FLOOR_FPS = 0.05, 0.2
SETTLE_MIN_ROWS = 60
MIN_PAIRS = 4
# two-sided Student t critical values: df -> (90 %, 95 %, 98 %, 99 %); a df between rows uses the row below it (wider, safe)
T_TABLE = [(1, 6.314, 12.706, 31.821, 63.657), (2, 2.920, 4.303, 6.965, 9.925), (3, 2.353, 3.182, 4.541, 5.841), (4, 2.132, 2.776, 3.747, 4.604),
           (5, 2.015, 2.571, 3.365, 4.032), (6, 1.943, 2.447, 3.143, 3.707), (7, 1.895, 2.365, 2.998, 3.499), (8, 1.860, 2.306, 2.896, 3.355),
           (9, 1.833, 2.262, 2.821, 3.250), (10, 1.812, 2.228, 2.764, 3.169), (12, 1.782, 2.179, 2.681, 3.055), (15, 1.753, 2.131, 2.602, 2.947),
           (20, 1.725, 2.086, 2.528, 2.845), (30, 1.697, 2.042, 2.457, 2.750)]


class Fail(Exception):
    pass


def t_crit(df, ci):
    col = {90: 1, 95: 2, 98: 3, 99: 4}[ci]
    best = None
    for row in T_TABLE:
        if row[0] <= df:
            best = row[col]
    return best if df <= 30 else {90: 1.645, 95: 1.960, 98: 2.326, 99: 2.576}[ci]


def parse_log(path):
    r = {'switch': [], 'seen': [], 'ab': [], 'states': [], 'bound': None, 'end': None, 'csv': None, 'summaries': [], 'refused': 0}
    try:
        text = open(path, encoding='utf-8', errors='replace').read().split('\n')
    except OSError as e:
        raise Fail('cannot read the run log %s: %s' % (path, e))
    for line in text:
        if '[switch] refused' in line:
            r['refused'] += 1
            continue
        m = SWITCH_RE.search(line)
        if m:
            r['switch'].append((int(m.group(1)), m.group(2), m.group(3), bool(m.group(4))))
            continue
        m = SEEN_RE.search(line)
        if m:
            r['seen'].append((int(m.group(1)), int(m.group(2)), m.group(3), m.group(4)))
            continue
        m = AB_RE.search(line)
        if m:
            r['ab'].append((int(m.group(1)), int(m.group(2)), m.group(3), int(m.group(4))))
            continue
        m = STATE_RE.search(line)
        if m:
            r['states'].append((int(m.group(2)), int(m.group(1))))
            continue
        m = BOUND_RE.search(line)
        if m:
            r['bound'] = int(m.group(1))
            continue
        m = END_RE.search(line)
        if m:
            r['end'] = int(m.group(1))
            continue
        m = HANDOFF_RE.search(line)
        if m:
            r['handoff'] = max(r.get('handoff') or 0, int(m.group(1)))
            continue
        m = SUMMARY_RE.search(line)
        if m:
            r['summaries'].append((int(m.group(1)), int(m.group(2)), float(m.group(3))))
            continue
        if r['csv'] is None and 'per-frame CSV:' in line:
            m = CSV_RE.search(line)
            if m:
                r['csv'] = m.group(1).strip()
    return r


def find_frames(log_path, named, from_log):
    if named:
        if not os.path.isfile(named):
            raise Fail('the frames file %s does not exist' % named)
        return named
    folder = os.path.dirname(os.path.abspath(log_path))
    cands = []
    if from_log:
        cands.append(os.path.join(folder, os.path.basename(from_log.replace('\\', '/'))))   # the log's own line, its basename beside the log
    m = re.search(r'(\d+)$', os.path.splitext(os.path.basename(log_path))[0])
    if m:
        cands.append(os.path.join(folder, 'frames_%s.csv' % m.group(1)))   # the log's own number
    for c in cands:
        if os.path.isfile(c):
            return c
    raise Fail('no frames file belonging to %s (looked for: %s; a different run\'s frames_*.csv beside it is never taken); pass --frames' % (log_path, ', '.join(cands) or 'nothing'))


def num(cell):
    try:
        return float(cell)
    except (TypeError, ValueError):
        return None


def read_frames(path):
    with open(path, encoding='utf-8', errors='replace', newline='') as f:
        rows = list(csv.DictReader(f))
    if not rows or 'frame' not in rows[0]:
        raise Fail('%s has no "frame" column or no rows' % path)
    return rows


def cross_check(rows, summaries, csv_path):
    """The frames file must be this run's: one log summary line (mean frame ms over frames a-b) must agree with the file."""
    last = int(float(rows[-1]['frame']))
    for a, b, avg in summaries:
        if a < 2 or b > last:
            continue
        v = [num(r.get('frame_ms')) for r in rows if a <= int(float(r['frame'])) <= b]
        v = [x for x in v if x is not None]
        if len(v) < (b - a + 1) // 2:
            continue
        mine = sum(v) / len(v)
        if abs(mine - avg) > max(0.5, 0.1 * avg):
            raise Fail('%s is not this run\'s frames file: the log says frames %d-%d averaged %.1f ms, the file says %.1f ms' % (csv_path, a, b, avg, mine))
        return (a, b, avg, mine)
    raise Fail('cannot tell %s belongs to this run: the log has no "[wwe2k14][frames] t=.. frames a-b .. avg X ms" line inside the file\'s frames' % csv_path)


def window_numbers(rows):
    out = {}
    t = [num(r.get('t_seconds')) for r in rows]
    presented = [num(r.get('presented')) for r in rows[1:]]   # a row's `presented` counts since the row before it: the first row's belongs to before the window
    if None not in t and len(rows) > 1 and t[-1] > t[0] and presented and None not in presented:
        out['shown fps'] = sum(presented) / (t[-1] - t[0])
    else:
        out['shown fps'] = None

    def mean(col):
        v = [num(r.get(col)) for r in rows]
        v = [x for x in v if x is not None]
        return sum(v) / len(v) if v else None

    out['GPU work (ms)'] = mean('gpu_ms')
    out['swap thread (ms)'] = mean('game_cpu_ms')
    out['render thread (ms)'] = next((mean(c) for c in RENDER_COLUMNS if c in rows[0] and mean(c) is not None), None)
    return out


def build_windows(log, args, last_frame):
    """[(k, arm, start, end)]"""
    on_norm = switch_table.norm(args.on)
    if log['ab']:
        ab = sorted(log['ab'])
        end = log['end'] if log['end'] is not None else last_frame - args.tail
        return [(k, arm, f, ab[i + 1][3] if i + 1 < len(ab) else end) for i, (k, n, arm, f) in enumerate(ab)], True
    ev = sorted(log["switch"])
    end = log['end'] if log['end'] is not None else last_frame - args.tail
    return [(i + 1, 'ON' if switch_table.norm(e[2]) == on_norm else 'OFF', e[0], ev[i + 1][0] if i + 1 < len(ev) else end) for i, e in enumerate(ev)], False


def check_handoff(name, log):
    """S102 T8 review F1: a handoff_memory verdict needs frames that really went over in memory (the case ring of a +dev owner build forces bytes: the switch then changes nothing)."""
    if name != 'wwe2k14_rdr_handoff_memory':
        return
    n = log.get('handoff')
    if n is None:
        raise Fail('%s: the log has no "handoff_memory in_memory=N" count (the 600-frame [rdr][live] line): a difference proves nothing' % name)
    if n == 0:
        raise Fail('%s: 0 frames were handed over in memory (the case ring forced bytes? run with --wwe2k14_case=false): the switch changed nothing, no verdict' % name)


def verdict(args):
    log = parse_log(args.runlog)
    if not log['switch']:
        raise Fail('no "[switch] frame N NAME=VALUE" line in %s: this run never flipped a switch (or it predates S0-4)%s'
                   % (args.runlog, '; it has %d refused (start-only) change(s)' % log['refused'] if log['refused'] else ''))
    names = sorted({e[1] for e in log['switch']})
    name = args.switch or (names[0] if len(names) == 1 else None)
    if name is None:
        raise Fail('the log flips %d switches (%s): pass --switch NAME' % (len(names), ', '.join(names)))
    events = sorted(e for e in log['switch'] if e[1] == name)
    if not events:
        raise Fail('the log has no [switch] line for %s (it has: %s)' % (name, ', '.join(names)))
    check_handoff(name, log)
    untabled = any(e[3] for e in events)
    if untabled and not args.untabled_ok:
        raise Fail('%s is "(not in the table)": nothing re-reads it per frame and no reader can acknowledge it, so a difference proves nothing. '
                   'Add it to src\\renderer_adapter\\rdr_switches.h and read it through rdr_sw::BoolSeen, or pass --untabled-ok to see the numbers as UNVERIFIED' % name)
    csv_path = find_frames(args.runlog, args.frames, log['csv'])
    rows = read_frames(csv_path)
    check = cross_check(rows, log['summaries'], csv_path)
    frames = [int(float(r['frame'])) for r in rows]
    windows, from_route = build_windows(log, args, frames[-1])
    on_norm = switch_table.norm(args.on)
    # PROOF 1: each flip happened
    table = switch_table.load() if not untabled else {}
    default = table.get(name, {}).get('default')
    prev = None
    for k, arm, start, end in windows:
        want_value_on = arm == 'ON'
        needs = prev != arm if prev is not None else (default is None or not ((str(default) == on_norm) == want_value_on))
        # (the first window needs a line unless the table default already equals its value)
        if needs:
            ok = any(start - 2 <= f <= start + 30 and (switch_table.norm(v) == on_norm) == want_value_on for f, n, v, u in events)
            if not ok:
                raise Fail('window %d says %s (frame %d) but no "[switch] frame N %s=%s" line follows it: the flip did not happen' % (k, arm, start, name, args.on if want_value_on else 'the OFF value'))
        prev = arm
    # PROOF 2: each table switch line was seen by a reader
    if not untabled:
        for i, (f, n, v, u) in enumerate(events):
            nxt = events[i + 1][0] if i + 1 < len(events) else 10 ** 9
            if not any(s[2] == name and s[3] == switch_table.norm(v) and f - 10 <= s[0] <= nxt + 10 for s in log['seen']):
                raise Fail('the flip at frame %d (%s=%s) was never acknowledged by a reader (no "[switch] seen frame N gen G %s=%s"): nothing re-reads this switch per frame, '
                           'so no number below could be its effect' % (f, name, v, name, switch_table.norm(v)))
    # PROOF 3: still inside the entrance
    if log['bound'] is not None and log['states']:
        first15 = next((f for f, s in sorted(log['states']) if s >= 15), None)
        if first15 is not None:
            late = [k for k, arm, start, end in windows if start > first15 + log['bound']]
            if late:
                raise Fail('windows %s start more than %d frames after step 15 (first seen at frame %d): the entrance is over, they measure the ring' % (late, log['bound'], first15))
    wins = {'ON': [], 'OFF': []}
    detail, series = [], []
    for k, arm, start, end in windows:
        part = [r for r, fr in zip(rows, frames) if start + args.settle <= fr < end]
        if len(part) < SETTLE_MIN_ROWS:
            raise Fail('window %d (frames %d-%d, %s) has %d rows after the settle: too short to measure' % (k, start, end, arm, len(part)))
        nums = window_numbers(part)
        series.append((arm, nums))
        wins[arm].append(nums)
        detail.append({'window': k, 'arm': arm, 'from': start, 'to': end, 'rows': len(part), **nums})
    result = {'switch': name, 'windows': len(windows), 'from_route_lines': from_route, 'settle': args.settle, 'ci': args.ci, 'unverified': untabled,
              'frames_file': csv_path, 'cross_check': check, 'sides': {}, 'detail': detail}
    for side in ('shown fps', 'GPU work (ms)', 'swap thread (ms)', 'render thread (ms)'):
        if any(nums[side] is None for arm, nums in series):
            result['sides'][side] = {'verdict': 'n/a (no data for this side in the frames file)'}
            continue
        d = []
        for i, (arm, nums) in enumerate(series):
            if arm != 'ON':
                continue
            nb = [series[j][1][side] for j in (i - 1, i + 1) if 0 <= j < len(series) and series[j][0] == 'OFF']
            if nb:
                d.append(nums[side] - sum(nb) / len(nb))
        if len(d) < MIN_PAIRS:
            raise Fail('%d ON / OFF pairs (need >= %d) in %s: too few windows to tell a difference from window-to-window noise (2 + 2 windows called "no difference" wrongly 22 %% of the time)' % (len(d), MIN_PAIRS, args.runlog))
        n = len(d)
        mean_d = sum(d) / n
        sd = math.sqrt(sum((x - mean_d) ** 2 for x in d) / (n - 1))
        half = t_crit(n - 1, args.ci) * sd / math.sqrt(n)
        floor = FLOOR_FPS if side == 'shown fps' else FLOOR_MS
        on = sum(nums[side] for arm, nums in series if arm == 'ON') / len(wins['ON'])
        off = sum(nums[side] for arm, nums in series if arm == 'OFF') / len(wins['OFF'])
        if (mean_d - half <= 0 <= mean_d + half) or abs(mean_d) < floor:
            text = 'no difference'
        else:
            text = 'ON %s by %.2f %s' % ('higher' if mean_d > 0 else 'lower', abs(mean_d), 'fps' if side == 'shown fps' else 'ms')
        if untabled:
            text += ' UNVERIFIED'
        result['sides'][side] = {'on': on, 'off': off, 'diff': mean_d, 'ci_half': half, 'pairs': n, 'verdict': text}
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('runlog')
    ap.add_argument('--frames')
    ap.add_argument('--switch')
    ap.add_argument('--on', default='true', help='the ON value (equality, so an int switch works: --on 4)')
    ap.add_argument('--settle', type=int, default=30)
    ap.add_argument('--tail', type=int, default=60)
    ap.add_argument('--ci', type=int, default=98, choices=(90, 95, 98, 99))
    ap.add_argument('--untabled-ok', action='store_true')
    ap.add_argument('--json', action='store_true')
    args = ap.parse_args()
    try:
        r = verdict(args)
    except Fail as e:
        print('ab_verdict: FAIL: %s' % e, file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(r, indent=1))
        return 0
    print('ab_verdict: %s  %d windows (%s), first %d frames of each dropped, %d %% CI%s' % (
        r['switch'], r['windows'], 'cut at the route\'s ab: lines' if r['from_route_lines'] else 'cut at the [switch] lines', r['settle'], r['ci'], ', UNVERIFIED' if r['unverified'] else ''))
    print('%-20s %10s %10s %10s %9s   %s' % ('side', 'ON', 'OFF', 'ON-OFF', 'CI +-', 'verdict'))
    for side, s in r['sides'].items():
        if 'on' not in s:
            print('%-20s %10s %10s %10s %9s   %s' % (side, '-', '-', '-', '-', s['verdict']))
        else:
            print('%-20s %10.3f %10.3f %+10.3f %9.3f   %s  (%d pairs)' % (side, s['on'], s['off'], s['diff'], s['ci_half'], s['verdict'], s['pairs']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
