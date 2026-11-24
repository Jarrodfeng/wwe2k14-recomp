r"""Offline replay of the S105 compile-queue requests through the OLD and the NEW admission rule (S106 lane 0C; docs\research\s106\0C.md).  0 game runs.

    py -I tools\admit\replay_history.py                       # the committed fixtures (tools\admit\fixtures\): exit 0 = all checks pass
    py -I tools\admit\replay_history.py --hist-real A.tsv --hist-overlay B.tsv    # the two S105 histories themselves (one per tool sandbox)
    py -I tools\admit\replay_history.py --make-fixture --hist-real A.tsv --hist-overlay B.tsv --lines admit_lines.json    # rebuild the fixtures

Why two histories: in S105 the Bash tool and the PowerShell tool saw different copies of %LOCALAPPDATA%\wwe2k14-admit, so two compile queues ran side
by side and neither saw the other's holds (0C verdict 1). The replay merges both: the fixed rule has ONE queue.

The rule mirrored here is tools\admit\admit.cpp `view_of` + `decide` (entry: decide / view_of): pending = for each live HOLD, its reservation minus its owner
tree's measured use (a pool ramps to its reservation: use(age) = R * min(1, age / ramp)); a lone job is admitted at once only if it fits or can never fit
the PC. The OLD rule is the whole reservation as pending for the hold's whole life, and any lone job admitted at once.

Checks (exit 1 if any fails):
  1  every memory-free timeout in the histories (available - floor >= the minimum's need, physical AND commit) is admitted by the new rule somewhere in
     its own wait at ramps of 5, 15 and 30 s (60 / 120 s are printed, not asserted: a pool that takes 2 minutes to reach its reservation is a real wait).
     S106 0CF (review F10): that "0 of 226" is an UPPER BOUND (every pool reaches its whole reservation; no feedback from the next pool's use). The claim is the
     sensitivity table printed after it: at a 30 s ramp the refused share is 0 / 30 / 55 / 79 of 226 at pool use 1.0 / 0.7 / 0.5 / 0.3 of the reservation (the old
     rule: 168), asserted at 0.7 (<= 35) and 0.5 (<= 60).
  2  planted, the UNSAFE direction (each must WAIT under the new rule): a 10-s-old 4-worker syntax pool with 18 GB available; 6 GB available / 6 GB commit
     with an old (fully used) hold; a lone job at floor + need - 0.5 GB (and the same plant is ADMITTED by the old rule: the plant can tell the two apart).
  3  the OLD rule applied to the logged `[admit] waiting` / TIMEOUT states refuses exactly the 141 + 10 = 151 states that were memory free: the replay's old
     rule is admit.cpp's (S105 `cls.py`'s count).
"""
import argparse
import csv
import datetime as D
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FIX = os.path.join(HERE, 'fixtures')
FLOOR = 4.0                      # admit.json floor_phys_gb == floor_commit_gb
TOTAL_PHYS = 31.91               # this PC's physical GB (the never-fits test)
WIN_FROM, WIN_TO = '2026-11-20 16:30:00', '2026-11-21 02:00:00'   # the S105 window the probe used
MIN = {'cvar.pp': 4, 'syntax.worker': 1, 'q.parse_worker': 2, 'cc.game_src': 1, 'cc.tool': 4}   # ceil(want / 4): the callers' wants 16, 4, ~6, 4, 16
HCOLS = ['q', 'time', 'kind', 'outcome', 'waited_s', 'reserve_phys_gb', 'reserve_commit_gb', 'avail_phys_gb', 'commit_left_gb', 'holders', 'held_s', 'workers']
STATE_RX = re.compile(r'(?P<kind>[\w.]+) needs (?P<need>[\d.]+) GB \+ floor (?P<fl>[\d.]+) \(commit (?P<needc>[\d.]+) \+ [\d.]+\); available (?P<av>[\d.]+) GB, '
                      r'commit left (?P<cm>[\d.]+) GB, holders (?P<h>\d+) \(pending (?P<pp>[\d.]+) GB, commit (?P<pc>[\d.]+) GB\)(?P<rest>.*)')


def ts(s):
    return D.datetime.strptime(s, '%Y-%m-%d %H:%M:%S').timestamp()


def num(x):
    return float(x) if x not in ('', '-', None) else 0.0


def load_hist(path, q):
    rows = []
    with open(path, encoding='utf-8', newline='') as f:
        for r in csv.DictReader(f, delimiter='\t'):
            if WIN_FROM <= r['time'] <= WIN_TO and r['outcome'] in ('admitted', 'timeout_fail'):
                rows.append({'q': q, **{k: r.get(k, '') for k in HCOLS[1:]}})
    return rows


def load_fixture_hist(path):
    with open(path, encoding='utf-8', newline='') as f:
        return list(csv.DictReader(f, delimiter='\t'))


# ---- the rule, mirrored from admit.cpp (view_of + decide) -------------------------------------------------------------------------------------
PLANT = ''   # --plant whole_reservation | lone_always: the new rule broken on purpose; the replay must then FAIL (the tool proves itself)


def pending_new(holds, T, ramp, frac=1.0):
    """view_of: a hold slot adds max(0, R - use / n) per worker: with use(age) = frac * R * min(1, age / ramp) that is R * (1 - frac * min(1, age / ramp)).
    frac = how much of its reservation a pool really uses at full ramp (1.0 = all of it: the upper bound of the model; S106 0CF, review F10)."""
    p = c = 0.0
    if PLANT == 'whole_reservation':
        return pending_old(holds, T)
    for st, end, _k, w, rp, rc in holds:
        if st <= T < end:
            u = frac * (min(1.0, (T - st) / ramp) if ramp > 0 else 1.0)
            p += (1 - u) * w * rp
            c += (1 - u) * w * rc
    return p, c


def pending_old(holds, T):
    p = c = 0.0
    for st, end, _k, w, rp, rc in holds:
        if st <= T < end:
            p += w * rp
            c += w * rc
    return p, c


def decide(avail, commit, pend_p, pend_c, need_p, need_c, holders, lone_rule):
    """True = admit. lone_rule 'old': a lone job is always admitted; 'new': only when its minimum can never fit this PC (the fit itself is the budget test)."""
    never_fits = need_p + FLOOR > TOTAL_PHYS or (PLANT == 'lone_always' and lone_rule == 'new')
    if holders == 0 and (lone_rule == 'old' or never_fits):
        return True
    return avail - pend_p - FLOOR >= need_p - 1e-9 and commit - pend_c - FLOOR >= need_c - 1e-9


def hold_intervals(rows):
    out = []
    for r in rows:
        if r['outcome'] == 'admitted':
            end = ts(r['time'])
            out.append((end - num(r['held_s']), end, r['kind'], int(num(r['workers']) or 1), num(r['reserve_phys_gb']), num(r['reserve_commit_gb'])))
    return out


def needs_of(r):
    k = r['kind']
    rp = num(r['reserve_phys_gb']) or 2.0
    rc = num(r['reserve_commit_gb']) or rp
    return rp * MIN.get(k, 1), rc * MIN.get(k, 1)


def still_timed_out(free, holds, ramp, frac):
    """-> (how many of the memory-free timeout rows the NEW rule still refuses for their whole wait, the waits of the others)."""
    still, waits = 0, []
    for r in free:
        np_, nc = needs_of(r)
        t1 = ts(r['time'])
        t0 = t1 - num(r['waited_s'])
        av, cm = num(r['avail_phys_gb']), num(r['commit_left_gb'])
        tau, got = t0, None
        while tau <= t1:
            pp, pc = pending_new(holds, tau, ramp, frac)
            live = sum(1 for h in holds if h[0] <= tau < h[1])
            if decide(av, cm, pp, pc, np_, nc, live, 'new'):
                got = tau - t0
                break
            tau += 1.0
        if got is None:
            still += 1
        else:
            waits.append(got)
    return still, sorted(waits)


# S106 0CF (review F10): "0 of 226" was an UPPER BOUND on the improvement: it assumes every pool reaches its whole reservation (use fraction 1.0) and a
# real pool uses less (and the use of the next pool feeds back into the next decision, which this offline replay does not model). The claim is therefore
# stated as a sensitivity: at a 30 s ramp, the share of the memory-free timeouts the new rule still refuses grows as the pools' real use falls. The bounds
# below are the measured values on the committed fixtures plus a small margin (use 0.7 -> 30, 0.5 -> 55, 0.3 -> 79 of 226; the old rule 168), asserted at
# 0.5-0.7 only: the new rule must stay far below the old one (at 0.5 less than half of the old rule's refusals) where the real pools sit.
SENS = ((1.0, 0), (0.7, 35), (0.5, 60), (0.3, None))   # (use fraction, asserted upper bound on "still timed out"; None = printed only)


def sensitivity(free, holds):
    bad = 0
    print('  sensitivity at a 30 s ramp (the pools use only this fraction of their reservation at full ramp; the old rule: see below):')
    for frac, bound in SENS:
        still, _w = still_timed_out(free, holds, 30, frac)
        ok = bound is None or still <= bound
        print(f'    use {frac:.1f}: still timed out {still:3d} of {len(free)}' + (f'   [asserted <= {bound}]' if bound is not None else '   [printed only]') + ('' if ok else '   FAIL'))
        bad += 0 if ok else 1
    return bad


def check1(rows, holds, verbose):
    to = [r for r in rows if r['outcome'] == 'timeout_fail']
    free = []
    for r in to:
        np_, nc = needs_of(r)
        if num(r['avail_phys_gb']) - FLOOR >= np_ and num(r['commit_left_gb']) - FLOOR >= nc:
            free.append(r)
    print(f'check 1: {len(to)} timeout_fail rows in the window, {len(free)} with memory free at their last attempt (both dimensions)')
    bad = 0
    for ramp in (5, 15, 30, 60, 120):
        still, waits = still_timed_out(free, holds, ramp, 1.0)   # use fraction 1.0: the UPPER BOUND of the model (every pool reaches its reservation)
        med = waits[len(waits) // 2] if waits else 0
        print(f'  ramp {ramp:3d} s: still timed out {still} of {len(free)}; admitted after median {med:.0f} s, max {(waits[-1] if waits else 0):.0f} s' +
              ('   [asserted]' if ramp <= 30 else '   [printed only]'))
        if ramp <= 30:
            bad += still
    # the old rule on the same rows (information; the real proof of the old rule is check 3)
    old_ok = 0
    for r in free:
        np_, nc = needs_of(r)
        T = ts(r['time']) - 0.5
        pp, pc = pending_old(holds, T)
        live = sum(1 for h in holds if h[0] <= T < h[1])
        old_ok += decide(num(r['avail_phys_gb']), num(r['commit_left_gb']), pp, pc, np_, nc, live, 'old')
    print(f'  (the OLD rule at those moments: admits {old_ok} of {len(free)}: the rest timed out for the pending term alone; it refused {len(free) - old_ok})')
    if free:
        bad += sensitivity(free, holds)
        if len(free) - old_ok < 100:
            print('  FAIL: the old rule refuses fewer than 100 of the memory-free timeouts: the replay is not the S105 data')
            bad += 1
    if not free:
        print('  FAIL: no memory-free timeout rows found: the histories are empty or not the S105 ones')
        return 1
    return 1 if bad else 0


def check2():
    bad = 0
    # (name, available, commit, holders, kind, pending model, must_wait_new)
    t = ts('2026-11-21 00:00:00')
    young = [(t - 10, t + 1800, 'syntax.worker', 4, 4.0, 4.0)]    # a 4-worker syntax pool, 10 s old: 16 GB reserved, its use not yet shown
    old = [(t - 1800, t + 1800, 'syntax.worker', 4, 4.0, 4.0)]     # an old hold: its use is the whole reservation, so nothing is pending
    cases = [
        ('a 10-s-old 4-worker syntax pool, available 18 GB, use ramping from 0 (ramp 30 s)', 18.0, 18.0, young, 4.0, 'new30'),
        ('the same pool with its use not shown at all (worst case: use 0)', 18.0, 18.0, young, 4.0, 'new0'),
        ('available 6 GB / commit 6 GB with an old (fully used) hold: free 2 GB < the 4 GB need', 6.0, 6.0, old, 4.0, 'new30'),
        ('a lone job at floor + need - 0.5 GB (a cvar.pp minimum of 4 x 0.5 GB would not fit: need 2.0 GB)', FLOOR + 2.0 - 0.5, 30.0, [], 2.0, 'new30'),
    ]
    print('check 2: planted cases, each must WAIT under the new rule')
    for name, av, cm, holds, need, model in cases:
        ramp = 0 if model == 'new0' else 30
        pp, pc = pending_new(holds, t, ramp if ramp else 1e9)   # ramp 1e9: use ~ 0 (a pool whose use never shows)
        if model == 'new30':
            pp, pc = pending_new(holds, t, 30)
        live = sum(1 for h in holds if h[0] <= t < h[1])
        new_ok = decide(av, cm, pp, pc, need, need, live, 'new')
        po, pco = pending_old(holds, t)
        old_ok = decide(av, cm, po, pco, need, need, live, 'old')
        print(f'  {"FAIL" if new_ok else "ok  "} {name}: new rule {"ADMITS" if new_ok else "waits"}, old rule {"admits" if old_ok else "waits"}')
        if new_ok:
            bad += 1
    # the plant must be able to tell the rules apart: the lone-job case is admitted by the OLD rule
    pp = pc = 0.0
    if not decide(FLOOR + 2.0 - 0.5, 30.0, pp, pc, 2.0, 2.0, 0, 'old'):
        print('  FAIL: the lone-job plant is not admitted by the old rule: the plant cannot tell the rules apart')
        bad += 1
    # and the other direction: the rule must still ADMIT when there is room (a rule that waits for everything passes the plants above)
    young_ok = decide(30.0, 30.0, *pending_new(old, t, 30), 4.0, 4.0, 1, 'new')
    lone_ok = decide(30.0, 30.0, 0.0, 0.0, 2.0, 2.0, 0, 'new')
    huge_ok = decide(40.0, 40.0, 0.0, 0.0, 40.0, 40.0, 0, 'new')
    print(f'  {"ok  " if young_ok and lone_ok and huge_ok else "FAIL"} with room: an old hold + 30 GB -> {"admits" if young_ok else "WAITS"}; a lone job with room -> '
          f'{"admits" if lone_ok else "WAITS"}; a lone job that can never fit this PC -> {"admits" if huge_ok else "WAITS"}')
    if not (young_ok and lone_ok and huge_ok):
        bad += 1
    return 1 if bad else 0


def check3(states):
    n = {'waiting': 0, 'TIMEOUT': 0}
    free_total = {'waiting': 0, 'TIMEOUT': 0}
    for s in states:
        mn = MIN.get(s['kind'], 1)
        need, needc = s['need'] * mn, s['needc'] * mn
        free = s['av'] - s['fl'] >= need and s['cm'] - s['fl'] >= needc
        dec_ok = (s['av'] - s['pp'] - s['fl'] >= need and s['cm'] - s['pc'] - s['fl'] >= needc) or s['h'] == 0
        if free:
            free_total[s['w']] += 1
            if not dec_ok:
                n[s['w']] += 1
    total = n['waiting'] + n['TIMEOUT']
    print(f'check 3: the OLD rule on the {len(states)} logged waiting / TIMEOUT states refuses {n["waiting"]} waiting + {n["TIMEOUT"]} TIMEOUT = {total} memory-free states '
          f'(the S105 count: 141 + 10 = 151)')
    return 0 if (n['waiting'], n['TIMEOUT']) == (141, 10) else 1


def make_fixture(real, overlay, lines):
    os.makedirs(FIX, exist_ok=True)
    rows = load_hist(real, 'real') + load_hist(overlay, 'overlay')
    with open(os.path.join(FIX, 's105_history.tsv'), 'w', encoding='utf-8', newline='') as f:
        w = csv.writer(f, delimiter='\t', lineterminator='\n')
        w.writerow(HCOLS)
        for r in rows:
            w.writerow([r[k] for k in HCOLS])
    states = []
    seen = {'TIMEOUT': set(), 'waiting': set()}
    for _t, _f, s in json.load(open(lines, encoding='utf-8')):
        for want in ('TIMEOUT', 'waiting'):
            if not s.startswith(want):
                continue
            m = STATE_RX.search(s)
            if not m:
                continue
            key = m.group(0)[:200]
            if key in seen[want]:
                continue
            seen[want].add(key)
            d = m.groupdict()
            states.append({'w': want, 'kind': d['kind'], 'need': float(d['need']), 'fl': float(d['fl']), 'needc': float(d['needc']), 'av': float(d['av']), 'cm': float(d['cm']),
                           'h': int(d['h']), 'pp': float(d['pp']), 'pc': float(d['pc']), 'senior': 'senior waiter' in d['rest']})
    with open(os.path.join(FIX, 's105_states.json'), 'w', encoding='utf-8', newline='') as f:
        json.dump(states, f, separators=(',', ':'))
        f.write('\n')
    print(f'fixtures written under {FIX}: {len(rows)} history rows, {len(states)} logged states')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--hist-real')
    ap.add_argument('--hist-overlay')
    ap.add_argument('--lines', help='with --make-fixture: the parsed S105 [admit] lines (json list of [time, who, text])')
    ap.add_argument('--states', default=os.path.join(FIX, 's105_states.json'))
    ap.add_argument('--make-fixture', action='store_true')
    ap.add_argument('--plant', choices=['whole_reservation', 'lone_always'], help='break the new rule on purpose: the replay must exit 1')
    a = ap.parse_args()
    global PLANT
    PLANT = a.plant or ''
    if a.make_fixture:
        if not (a.hist_real and a.hist_overlay and a.lines):
            print('--make-fixture needs --hist-real, --hist-overlay and --lines')
            return 2
        make_fixture(a.hist_real, a.hist_overlay, a.lines)
        return 0
    if a.hist_real and a.hist_overlay:
        rows = load_hist(a.hist_real, 'real') + load_hist(a.hist_overlay, 'overlay')
        src = f'{a.hist_real} + {a.hist_overlay}'
    else:
        rows = load_fixture_hist(os.path.join(FIX, 's105_history.tsv'))
        src = 'the committed fixture s105_history.tsv'
    print(f'replay_history: {len(rows)} S105 rows ({src}); queues: ' + ', '.join(f'{q} {sum(1 for r in rows if r["q"] == q)}' for q in ('real', 'overlay')))
    holds = hold_intervals(rows)
    with open(a.states, encoding='utf-8') as f:
        states = json.load(f)
    rc = check1(rows, holds, True) | check2() | check3(states)
    print('replay_history: ' + ('PASS' if rc == 0 else 'FAIL') + (f' (PLANTED: {PLANT})' if PLANT else ''))
    return rc


if __name__ == '__main__':
    sys.exit(main())
