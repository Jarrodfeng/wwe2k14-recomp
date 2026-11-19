#!/usr/bin/env python3
"""S102 M7: read an IN-RUN A/B (the game's --wwe2k14_ab_flip, src\\ab_flip_math.h, docs\\renderer\\IN_RUN_AB.md): one run that flips PerFrame switches on a fixed schedule.

  py -I tools\\surface\\flip_line.py RUN_A --pair RUN_B [--rel R0:R1] [--settle N] [--quiet]     the job: TWO runs with complementary schedules (run B has /first=on)
  py -I tools\\surface\\flip_line.py STEP_FOLDER [--run N] [--settle N] [--frames F0:F1] [--no-window]    one run alone (drift cancelled, content NOT: see below)
  py -I tools\\surface\\flip_line.py OLD_RUN --null-blocks N     the past-bug mode for an OLD run (no flips): alternating N-frame blocks that changed nothing, the delta must be ~0

THE PAIR (the estimator; S102 M7 review F1-F4, F7):
 - Frames are paired by ROUTE-MARK time, rel = guest frame - the anchor mark's frame (the log's `[ab_flip] anchor mark N at frame A`): content follows the route's marks, not the absolute
   guest frame (the tron mark fell 77 frames apart across 12 runs). A run whose spec has no anchor, or whose log has no anchor line, is REFUSED; so are runs whose flips sit at other rel frames.
 - Settle rows (the first SETTLE rows after each flip) are out of BOTH runs. Rows are in phase blocks (a phase = a block, the same rel frames in both runs).
 - CROSSOVER: in a block where run 1 is on, d = on(run 1) - ref(run 2); where run 2 is on, d = on(run 2) - ref(run 1). A run-level offset c (R19: +2.46 ms for a whole run) enters the two directions with opposite
   signs; it cancels only when the directions are weighted equally, so adjacent blocks (one of each direction) are PAIRED and the estimate is the mean of the pair means (a window that clips one direction's
   frames does not unbalance it). The interval is a t interval on the pair means, df = pairs - 1 (a percentile bootstrap over 3-4 blocks was 25 % false CLEAR on null data). CLEAR needs >= 6 blocks (3 pairs).
 - Judged like bench_line.py: shown fps (presents / sum frame_ms), the 1 % low as the p99 of present_gap_ms (printed as gap ms and as fps), plus gpu_ms, frame_ms and the other columns.
 - Guards: MISMATCH (exit 3, no number) when the log's [switch] lines, the readers' `[switch] seen` lines (value, frame in [F-1, F+settle]) or the nf census lines (loop / dispatcher at guest frame G) disagree
   with the phase the [ab_flip] lines say; SLOW STATE when the delta of a phase's second half differs from its first; SETTLE SHORT when the measured lag (the settle profile: mean signed delta per row after a flip,
   zero crossing) plus 3 exceeds settle.
ONE RUN ALONE: each phase against the reference LINEARLY INTERPOLATED to the phase midpoint (a linear drift cancels exactly, CONTENT does not), t95 and bootstrap intervals, the naive whole-run difference.
This is the number a pair replaces; it is printed beside the pair as 'single run alone'."""
import argparse, csv, math, random, re, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench_line as bl
import burst_rules   # S102 B1
import census_phase   # S102 CEN1

LINE_START = re.compile(r'\[ab_flip\] on at frame (\d+): period (\d+) settle (\d+) anchor (\d+) start (\d+) cycle ([A-Za-z0-9,]+) \(reference = ([A-Za-z0-9]+)\)')
LINE_SPEC = re.compile(r'\[ab_flip\] spec (\S+)')
LINE_ANCHOR = re.compile(r'\[ab_flip\] anchor mark (\d+) at frame (\d+)')
LINE_PHASE = re.compile(r'\[ab_flip\] frame (\d+) phase ([A-Za-z0-9]+) no (-?\d+) rel (-?\d+)')
LINE_SWITCH = re.compile(r'\[switch\] frame (\d+) (\w+)=(\S+?)\s*$')
LINE_SEEN = re.compile(r'\[switch\] seen frame (\d+) gen (\d+) (\w+)=(\S+)')
LINE_CENSUS = re.compile(r'\[rdr\]\[nf_replay\].*guest frame (\d+), loop (\d+), dispatcher (\d+)')
LINE_REFUSED = re.compile(r'\[ab_flip\] (REFUSED[^\n]*|frame \d+: the setting[^\n]*REJECTED[^\n]*)')
SPEC_KEYS = ('period', 'settle', 'start', 'order', 'first', 'anchor')
FRAME_COLS = ['frame_ms', 'game_cpu_ms', 'gpu_ms', 'render_thread_ms'] + list(bl.SPLIT_COLS)
METRICS = ['frame_ms', 'p99_ms', 'game_cpu_ms', 'gpu_ms', 'render_thread_ms'] + list(bl.SPLIT_COLS)   # the single-run reader's list
POWER_W = 1.5
MIN_BLOCKS = 6
T95 = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365, 8: 2.306, 9: 2.262, 10: 2.228, 12: 2.179, 15: 2.131, 20: 2.086, 30: 2.042}


class Refused(Exception):
    pass


# S102 main (2026-11-15 04:0x): a switch read on our render thread is stamped with the guest frame that thread is working on, which runs up to
# the render queue's depth behind the swap thread that applies the flip (R21-B's stage_split: seen at F-2). Accept seen lines from F-4 (max_queued 2-4).
SEEN_EARLY = 4

def tcrit(df):
    if df >= 30:
        return 1.96
    return T95[max(x for x in T95 if x <= max(df, 1))]


def num(x):
    try:
        v = float(x)
        return v if v == v else None
    except (TypeError, ValueError):
        return None


def norm(x):
    """true / false / a number as one comparable text (the registry prints true / false, `[switch] seen` prints the number)."""
    if x == 'true':
        return '1'
    if x == 'false':
        return '0'
    try:
        return str(int(x, 0))
    except (TypeError, ValueError):
        return str(x)


def mean(v):
    return sum(v) / len(v) if v else None


def read_run(step, run=None):
    step = Path(step)
    n = run if run is not None else bl.newest_run(step)
    if n is None:
        raise Refused(f'{step}: no frames_NNN.csv')
    csvp, logp = step / f'frames_{n}.csv', step / f'wwe2k14_{n}.log'
    if not csvp.is_file() or not logp.is_file():
        raise Refused(f'{step}: run {n} lacks frames_{n}.csv or wwe2k14_{n}.log')
    lg = bl.read_log(logp)
    info = {'start': None, 'spec': None, 'anchor': None, 'phases': [], 'switch': [], 'seen': [], 'census': [], 'problems': []}
    with open(logp, encoding='utf-8', errors='replace') as f:
        for line in f:
            st = bl.STAMP.match(line)
            ck = bl.clock(*st.groups()) if st else None
            m = LINE_START.search(line)
            if m:
                info['start'] = tuple(int(x) if x.isdigit() else x for x in m.groups())   # frame, period, settle, anchor, start, cycle, reference
            m = LINE_SPEC.search(line)
            if m:
                info['spec'] = m.group(1)
            m = LINE_ANCHOR.search(line)
            if m:
                info['anchor'] = (int(m.group(1)), int(m.group(2)))
            m = LINE_PHASE.search(line)
            if m:
                info['phases'].append((int(m.group(1)), m.group(2), int(m.group(3)), ck, int(m.group(4))))
            m = LINE_SEEN.search(line)
            if m:
                info['seen'].append((int(m.group(1)), m.group(3), m.group(4)))
            else:
                m = LINE_SWITCH.search(line)
                if m:
                    info['switch'].append((int(m.group(1)), m.group(2), m.group(3)))
            m = LINE_CENSUS.search(line)
            if m:
                info['census'].append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
            m = LINE_REFUSED.search(line)
            if m:
                info['problems'].append(m.group(1))
    with open(csvp, newline='', encoding='utf-8', errors='replace') as f:
        rd = csv.DictReader(f)
        bl.check_header(rd.fieldnames, csvp.name)
        rows = []
        for r in rd:
            if not (r.get('frame') and r.get('frame_ms')):
                continue
            d = {'frame': int(r['frame']), 't': num(r.get('t_seconds')), 'presented': num(r.get('presented')), 'gap': num(r.get('present_gap_ms'))}
            for c in FRAME_COLS:
                d[c] = num(r.get(c))
            rows.append(d)
    power = []
    pc = step / 'power.csv'
    if pc.is_file():
        with open(pc, newline='', encoding='utf-8', errors='replace') as f:
            for r in csv.DictReader(f):
                try:
                    power.append((bl.clock(*r['time'].split(':')), num(r.get('rapl_pkg'))))
                except (ValueError, TypeError, KeyError):
                    pass
    return n, lg, info, rows, power


def print_census(step, n, info, ref, tag=''):
    """S102 CEN1: the census lines of each phase (census_phase.py): a lever whose census never printed in its ON phase is said so."""
    try:
        ev = census_phase.read_events(Path(step) / f'wwe2k14_{n}.log')
    except OSError:
        print(tag + 'census by phase: the log cannot be read')
        return
    seen = {}
    for p in sorted(info['phases']):
        seen.setdefault(p[2], p[1])
    if not any(k == 'marker' for k, _ in ev):
        print(tag + 'census by phase: NO "[census] flush" MARKER in the log (a build without census_flush.h: the census lines show only every 600 frames)')
        return
    for line in census_phase.report(ev, sorted(seen.items()), ref):
        print(tag + line)


def spec_phases(spec):
    out = {}
    for part in spec.split('/'):
        k, _, v = part.partition('=')
        if k not in SPEC_KEYS:
            out[k] = {a: norm(b) for a, b in (kv.split('=', 1) for kv in v.split('+'))}
    return out


def switch_check(info):
    """The log must agree with itself: at every phase change each switch whose value differs from the previous phase's needs (1) a `[switch] frame F NAME=V` line at that frame with that value, (2) a reader's
    `[switch] seen frame S gen G NAME=V` line with that value and S in [F-1, F+settle] (a switch nothing re-reads cannot give a verdict), and the nf census lines (guest frame G, loop L, dispatcher D) must show the
    phase's value for nf_replay_loop / nf_render_dispatch when G is past the settle of the phase active at G. -> [problem text]."""
    probs = list(info['problems'])
    spec = info['spec']
    if not spec:
        return probs + ['no [ab_flip] spec line in the log: the log cannot be checked against the switch lines']
    st = info['start']
    settle = st[2] if st else 30
    phases = spec_phases(spec)
    sw = {}
    for f, n, v in info['switch']:
        sw.setdefault((f, n), norm(v))
    prev = None
    ordered = sorted(info['phases'])
    for f, name, no, _ck, _rel in ordered:
        want = phases.get(name)
        if want is None:
            probs.append(f'the log has a phase {name!r} at frame {f}, which the spec does not define')
            prev = None
            continue
        if no >= 0 and prev is not None:
            for n, v in want.items():
                if prev.get(n) == v:
                    continue
                got = sw.get((f, n))
                if got is None:
                    near = [x for x in info['switch'] if x[1] == n and abs(x[0] - f) <= 600]
                    probs.append(f'phase {name} at frame {f}: no "[switch] frame {f} {n}=" line' + (f' (the nearest is at frame {near[0][0]})' if near else ' (none at all)'))
                elif got != v:
                    probs.append(f'phase {name} at frame {f}: {n} became {got}, the spec says {v}')
                ok = [s for s in info['seen'] if s[1] == n and f - SEEN_EARLY <= s[0] <= f + settle and norm(s[2]) == v]
                if not ok:
                    other = [s for s in info['seen'] if s[1] == n and f - SEEN_EARLY <= s[0] <= f + settle]
                    probs.append(f'phase {name} at frame {f}: no "[switch] seen" line for {n}={v} in frames {f - SEEN_EARLY}..{f + settle}' +
                                 (f' (a seen line shows {norm(other[0][2])})' if other else ' (no reader acknowledged the change)'))
        prev = want
    for g, loop, disp in info['census']:
        act = [p for p in ordered if p[0] <= g and p[2] >= 0]
        if not act or g - act[-1][0] <= settle:
            continue
        want = phases.get(act[-1][1], {})
        for name, val in (('wwe2k14_nf_replay_loop', loop), ('wwe2k14_nf_render_dispatch', disp)):
            if name in want and want[name] != str(val):
                probs.append(f'nf census at guest frame {g}: {name} is {val}, the phase {act[-1][1]} says {want[name]}')
    return probs


def percentile99(v):
    s = sorted(v)
    return s[min(len(s) - 1, int(0.99 * len(s)))]


def build_phases(info, rows, settle, win=None, rel_win=None):
    """-> [phase dict: name, no, f0 (the flip frame), rel0, rows (settle dropped; each row gets 'rel'), c0, c1, mid]. The pre-phase (no -1) is not a phase."""
    ph = sorted(p for p in info['phases'] if p[2] >= 0)
    if not ph:
        raise Refused('no [ab_flip] phase lines in the log (the run had no --wwe2k14_ab_flip, or its spec was refused)')
    A = info['anchor'][1] if info['anchor'] else 0
    last_frame = rows[-1]['frame']
    t_of = {r['frame']: r['t'] for r in rows}
    anchor = next((p for p in ph if p[3] is not None and t_of.get(p[0]) is not None), None)
    out = []
    for i, (f, name, no, ck, rel0) in enumerate(ph):
        nxt = ph[i + 1][0] if i + 1 < len(ph) else last_frame
        first, last = f + 1 + settle, nxt
        if win:
            first, last = max(first, win[0]), min(last, win[1])
        if rel_win:
            first, last = max(first, rel_win[0] + A), min(last, rel_win[1] + A)
        sel = [dict(r, rel=r['frame'] - A) for r in rows if first <= r['frame'] <= last]
        if len(sel) < 10:
            continue
        c0 = c1 = None
        if anchor and sel[0]['t'] is not None and sel[-1]['t'] is not None:
            base = anchor[3] - t_of[anchor[0]]
            c0, c1 = base + sel[0]['t'], base + sel[-1]['t']
        out.append({'name': name, 'no': no, 'f0': f, 'rel0': rel0, 'rows': sel, 'c0': c0, 'c1': c1, 'mid': (sel[0]['frame'] + sel[-1]['frame']) / 2})
    return out


def null_phases(rows, win, block, mark=None):
    f0, f1 = win if win else (rows[0]['frame'], rows[-1]['frame'])
    out, f, k = [], f0, 0
    t_of = {r['frame']: r['t'] for r in rows}
    base = (mark[0] - t_of[mark[2]]) if mark and t_of.get(mark[2]) is not None else None   # clock of day = base + t_seconds (a scene mark's log time and its csv row)
    while f + block - 1 <= f1:
        sel = [r for r in rows if f <= r['frame'] < f + block]
        if len(sel) >= 10:
            out.append({'name': 'off' if k % 2 == 0 else 'on', 'no': k, 'f0': f, 'rows': sel, 'c0': None if base is None else base + sel[0]['t'], 'c1': None if base is None else base + sel[-1]['t'],
                        'mid': (sel[0]['frame'] + sel[-1]['frame']) / 2})
        f += block
        k += 1
    return out


def phase_value(p, metric, power):
    rs = p['rows']
    if metric == 'pkg_w':
        if p['c0'] is None or not power:
            return None
        v = [w for t, w in power if w is not None and p['c0'] <= t <= p['c1']]
        return sum(v) / len(v) / 1000.0 if v else None
    if metric == 'p99_ms':
        v = [r['frame_ms'] for r in rs if r['frame_ms'] is not None]
        return percentile99(v) if v else None
    v = [r[metric] for r in rs if r.get(metric) is not None]
    return sum(v) / len(v) if len(v) >= max(5, len(rs) // 2) else None


def paired(phases, power, metric, ref):
    """ONE RUN ALONE. Per non-reference phase NAME: pair deltas against the linearly interpolated reference. Only two-sided pairs when >= 2 exist."""
    seq = [(p['mid'], p['name'], phase_value(p, metric, power), p) for p in phases]
    res = {}
    for name in sorted({s[1] for s in seq if s[1] != ref}):
        two, one = [], []
        for i, (mid, nm, v, _p) in enumerate(seq):
            if nm != name or v is None:
                continue
            a = next(((m, x) for m, n2, x, _ in reversed(seq[:i]) if n2 == ref and x is not None), None)
            b = next(((m, x) for m, n2, x, _ in seq[i + 1:] if n2 == ref and x is not None), None)
            if a and b:
                est = a[1] + (b[1] - a[1]) * (mid - a[0]) / (b[0] - a[0])
                two.append(v - est)
            elif a or b:
                one.append(v - (a or b)[1])
        use, kind = (two, 'two-sided') if len(two) >= 2 else (two + one, 'one-sided included')
        pv = [s[2] for s in seq if s[1] == name and s[2] is not None]
        rv = [s[2] for s in seq if s[1] == ref and s[2] is not None]
        res[name] = {'deltas': use, 'kind': kind, 'n_two': len(two), 'n_one': len(one),
                     'naive': (sum(pv) / len(pv) - sum(rv) / len(rv)) if pv and rv else None, 'ref_vals': rv, 'p_vals': pv}
    return res


def t_interval(d):
    """mean, lo, hi, sd of a list (t interval, df = n - 1); None below 2 values."""
    n = len(d)
    if n < 2:
        return None
    m = sum(d) / n
    sd = math.sqrt(sum((x - m) ** 2 for x in d) / (n - 1))
    h = tcrit(n - 1) * sd / math.sqrt(n)
    return m, m - h, m + h, sd


def interval(d):
    n = len(d)
    t = t_interval(d)
    if t is None:
        return None
    m, lo, hi, sd = t
    rng = random.Random(1)
    bs = sorted(sum(rng.choice(d) for _ in range(n)) / n for _ in range(4000))
    return m, lo, hi, bs[100], bs[3899], sd


def fmt(v, spec='{:+.2f}'):
    return spec.format(v) if v is not None else '-'


def load_flip(step, a, need_anchor=False):
    n, lg, info, rows, power = read_run(step, a.run)
    st = info['start']
    settle = a.settle if a.settle is not None else (st[2] if st else 30)
    win = None
    if getattr(a, 'frames', None):
        x, y = a.frames.split(':')
        win = (int(x), int(y))
    elif not getattr(a, 'no_window', False):
        for kind in ('heavy', 'entrance', 'match'):
            try:
                w = bl.windows(kind, lg['marks'], Path(step).name)[0]
                win = (w[1], w[2])
                break
            except bl.Refused:
                continue
    if need_anchor:
        if not st or st[3] == 0:
            raise Refused(f'{Path(step).name}: the spec has no anchor=N: pairing by absolute guest frame is not trusted (content follows the route marks). Add anchor=<route mark id>')
        if not info['anchor'] or info['anchor'][0] != st[3]:
            raise Refused(f'{Path(step).name}: no "[ab_flip] anchor mark {st[3]} at frame" line in the log (the mark never fired)')
    rel_win = None
    if getattr(a, 'rel', None):
        x, y = a.rel.split(':')
        rel_win = (int(x), int(y))
    end = (None, win[1]) if win and need_anchor else None
    phases = build_phases(info, rows, settle, None if need_anchor else win, rel_win)
    if end:   # the pair reads from the anchor to the route's end mark
        A = info['anchor'][1]
        phases = [dict(p, rows=[r for r in p['rows'] if r['frame'] <= end[1]]) for p in phases]
        phases = [p for p in phases if len(p['rows']) >= 10]
    ref = st[6] if st else (phases[0]['name'] if phases else None)
    return {'n': n, 'info': info, 'rows': rows, 'power': power, 'win': win, 'settle': settle, 'phases': phases, 'ref': ref, 'problems': switch_check(info), 'step': step, 'start': st,
            'anchor_frame': info['anchor'][1] if info['anchor'] else 0}


def rel_states(run):
    out = {}
    for p in run['phases']:
        for r in p['rows']:
            out[r['rel']] = (p['name'], p['no'], r)
    return out


def side_stats(rows):
    """shown fps (presents / sum frame_ms) and the 1 % low gap (p99 of present_gap_ms over presented frames) of a block's rows on one side."""
    ms = [r['frame_ms'] for r in rows if r['frame_ms'] is not None]
    pres = [r['presented'] for r in rows if r['presented'] is not None]
    fps = 1000.0 * sum(pres) / sum(ms) if ms and len(pres) == len(rows) and sum(ms) > 0 else None
    gaps = [r['gap'] for r in rows if r['gap'] is not None and r['presented']]
    return fps, (percentile99(gaps) if len(gaps) >= 5 else None)


def pair_blocks(blocks):
    """blocks = [(no, direction, d)] in order; adjacent blocks of opposite direction are paired left to right -> [pair mean]."""
    bs = sorted(blocks)
    out, i = [], 0
    while i + 1 < len(bs):
        (n1, d1, v1), (n2, d2, v2) = bs[i], bs[i + 1]
        if n2 == n1 + 1 and d1 != d2:
            out.append((v1 + v2) / 2)
            i += 2
        else:
            i += 1
    return out


def crossover(blocks):
    """-> dict(mean, lo, hi, pairs, blocks, d1, d2): the crossover estimate with a t interval on the pair means."""
    pm = pair_blocks(blocks)
    t = t_interval(pm)
    m1 = mean([v for _, d, v in blocks if d == 1])
    m2 = mean([v for _, d, v in blocks if d == 2])
    if t is None:
        return {'mean': mean(pm), 'lo': None, 'hi': None, 'pairs': len(pm), 'blocks': len(blocks), 'd1': m1, 'd2': m2}
    return {'mean': t[0], 'lo': t[1], 'hi': t[2], 'pairs': len(pm), 'blocks': len(blocks), 'd1': m1, 'd2': m2}


def verdict_of(c, name_on, ref, label, better_up=False):
    if c['lo'] is None or c['blocks'] < MIN_BLOCKS:
        return f'TOO FEW BLOCKS ({c["blocks"]} blocks, {c["pairs"]} pairs: CLEAR needs {MIN_BLOCKS} blocks / 3 pairs; use period 60-100 or a longer window)'
    if c['lo'] > 0 or c['hi'] < 0:
        return f'CLEAR: {name_on} is {abs(c["mean"]):.2f} {label} {"higher" if c["mean"] > 0 else "lower"} than {ref} (95 % t interval {c["lo"]:+.2f} .. {c["hi"]:+.2f}, {c["pairs"]} crossover pairs)'
    return f'NOT SEPARATED: {label} difference {c["mean"]:+.2f}, interval {c["lo"]:+.2f} .. {c["hi"]:+.2f} includes 0 ({c["pairs"]} pairs)'


def settle_profile(r1, r2, on, ref, blocks_no, settle, period):
    """Mean signed delta per row after a flip: e_j = s * (v1 - v2) at the j-th row after the flip, s = +1 where run 1 is on. It runs from about -plateau (the old phase) to +plateau (the new one). -> (profile list, plateau, lag, se)."""
    m1 = {r['rel']: r for r in r1['rawrows']}
    m2 = {r['rel']: r for r in r2['rawrows']}
    L = min(period, settle + 25)
    sums = [[] for _ in range(L)]
    plats, tails = [], []
    for no, (rel0, s) in blocks_no.items():
        if no == 0:
            continue
        e = []
        for j in range(1, L + 1):
            a, b = m1.get(rel0 + j), m2.get(rel0 + j)
            e.append(s * (a['frame_ms'] - b['frame_ms']) if a and b and a['frame_ms'] is not None and b['frame_ms'] is not None else None)
        if sum(x is not None for x in e) < L * 0.8:
            continue
        for j, x in enumerate(e):
            if x is not None:
                sums[j].append(x)
        pl = [x for x in e[settle:] if x is not None]
        tl = [x for x in e[settle // 2:settle] if x is not None]
        if pl and tl:
            plats.append(mean(pl))
            tails.append(mean(tl) - mean(pl))   # the last rows of the settle window against the plateau: a short settle leaves the old phase in them
    prof = [mean(x) for x in sums]
    if len(plats) < 3 or any(p is None for p in prof):
        return prof, None, None, None, None
    t = t_interval(plats)
    tail = t_interval(tails)
    plateau, se = t[0], t[3] / math.sqrt(len(plats))
    lag = None
    if abs(plateau) >= 3 * se:
        for j in range(len(prof) - 2):
            sm = (prof[j] + prof[j + 1] + prof[j + 2]) / 3
            if sm * plateau >= 0:
                lag = j + 1
                break
    return prof, plateau, lag, se, tail


def pair_main(a):
    """TWO runs with complementary schedules, paired by route-mark time. See the module docstring."""
    r1, r2 = load_flip(a.step, a, True), load_flip(a.pair, a, True)
    probs = ['run 1: ' + p for p in r1['problems']] + ['run 2: ' + p for p in r2['problems']]
    if probs:
        for p in probs:
            print('MISMATCH: ' + p)
        print('VERDICT: MISMATCH: the phases the logs name are not what the game applied: no number is trusted')
        return 3
    if r1['ref'] != r2['ref']:
        raise Refused('the two runs name different references (%s / %s): not one spec' % (r1['ref'], r2['ref']))
    # a run's last phase can be cut short by the quit (fewer than 10 rows before the route ends): compare the phases BOTH runs have, by phase number, and read only those
    d1 = {p['no']: p['rel0'] for p in r1['phases']}
    d2 = {p['no']: p['rel0'] for p in r2['phases']}
    common_no = sorted(set(d1) & set(d2))
    bad = [(n, d1[n], d2[n]) for n in common_no if d1[n] != d2[n]]
    if r1['settle'] != r2['settle'] or bad or not common_no:
        first = ('first differing entry: phase %d flips at rel %d in run 1 and rel %d in run 2' % bad[0]) if bad else 'no phase in common'
        raise Refused('the two runs have different flip frames (rel to the anchor) or settle (settle %s / %s; %s; run 1 %s, run 2 %s): the schedules must differ only in which phase comes first'
                      % (r1['settle'], r2['settle'], first, sorted(d1.items())[:4], sorted(d2.items())[:4]))
    for r in (r1, r2):
        r['phases'] = [p for p in r['phases'] if p['no'] in common_no]
    if len(d1) != len(d2):
        print('note: run 1 has %d phases, run 2 has %d (the shorter run\'s last phase was cut by the quit): the %d common phases are read' % (len(d1), len(d2), len(common_no)))
    ref = r1['ref']
    s1, s2 = rel_states(r1), rel_states(r2)
    common = sorted(set(s1) & set(s2))
    names = {s1[f][0] for f in common} | {s2[f][0] for f in common}
    if len(names) != 2 or ref not in names:
        raise Refused('the paired runs must have exactly two phases (reference %s and one other), got %s' % (ref, sorted(names)))
    on = next(x for x in names if x != ref)
    same = [f for f in common if s1[f][0] == s2[f][0]]
    if same:
        raise Refused('the schedules are not complementary: %d frames (e.g. rel %d) are in the same phase in both runs' % (len(same), same[0]))
    print(f'PAIRED flip job: run 1 {r1["n"]} ({Path(a.step).name}) starts {s1[common[0]][0]}, run 2 {r2["n"]} ({Path(a.pair).name}) starts {s2[common[0]][0]} | anchors: run 1 frame {r1["anchor_frame"]}, run 2 frame {r2["anchor_frame"]} (paired by rel = frame - anchor) |'
          f' settle {r1["settle"]} dropped from both | {len(common)} paired frames, reference {ref}, other {on}')
    for tag, st_, r_ in (('run 1: ', a.step, r1), ('run 2: ', a.pair, r2)):
        print_census(st_, r_['n'], r_['info'], ref, tag)
    blocks = {}
    for f in common:
        blocks.setdefault(s1[f][1], []).append((f, s1[f][2], s2[f][2], 1 if s1[f][0] == on else 2))
    # per block: (direction, d) for every metric; side stats for shown fps / 1 % low gap
    per = {}
    for no, lst in sorted(blocks.items()):
        if len(lst) < 10:
            continue
        dr = lst[0][3]
        on_rows = [x[1] if dr == 1 else x[2] for x in lst]
        ref_rows = [x[2] if dr == 1 else x[1] for x in lst]
        rec = {'dir': dr, 'n': len(lst), 'rel0': lst[0][0]}
        for m in FRAME_COLS:
            v = [(o[m] - q[m]) for o, q in zip(on_rows, ref_rows) if o.get(m) is not None and q.get(m) is not None]
            if len(v) >= max(5, len(lst) // 2):
                rec[m] = mean(v)
        fo, go = side_stats(on_rows)
        fr, gr = side_stats(ref_rows)
        rec['fps'] = (fo, fr)
        rec['gap'] = (go, gr)
        if fo is not None and fr is not None:
            rec['shown_fps'] = fo - fr
        if go is not None and gr is not None:
            rec['gap_p99'] = go - gr
        mid = len(lst) // 2
        for m in ('frame_ms', 'gpu_ms'):
            h = [[(o[m] - q[m]) for o, q in zip(on_rows[s], ref_rows[s]) if o.get(m) is not None and q.get(m) is not None] for s in (slice(0, mid), slice(mid, None))]
            if h[0] and h[1]:
                rec['half_' + m] = mean(h[1]) - mean(h[0])
        per[no] = rec
    print('blocks: ' + ' '.join(f'{no}[{r["dir"]}:{r["n"]}]' for no, r in per.items()) + '  (direction 1 = run 1 on)')
    out = {}
    for m in FRAME_COLS + ['shown_fps', 'gap_p99']:
        bl_ = [(no, r['dir'], r[m]) for no, r in per.items() if m in r]
        if len(bl_) >= 2:
            out[m] = crossover(bl_)
    lab = {'shown_fps': 'fps', 'gap_p99': 'ms'}
    for m, c in out.items():
        if a.quiet and m not in ('shown_fps', 'gap_p99', 'frame_ms', 'gpu_ms'):
            continue
        tail = '' if c['lo'] is None else f'  t95 [{c["lo"]:+.2f}, {c["hi"]:+.2f}]'
        print(f'{m:<20} {on} - {ref}: {fmt(c["mean"])}{tail}  blocks {c["blocks"]} pairs {c["pairs"]} | direction means {fmt(c["d1"])} / {fmt(c["d2"])} (half their difference {fmt((c["d1"] - c["d2"]) / 2 if c["d1"] is not None and c["d2"] is not None else None)} = the run-level offset the crossover cancels)')
    # the 1 % low and shown fps per block, both sides
    if not a.quiet:
        for no, r in per.items():
            print(f'  block {no} dir {r["dir"]}: shown fps {on} {fmt(r["fps"][0], "{:.1f}")} {ref} {fmt(r["fps"][1], "{:.1f}")} | 1 % low gap ms {on} {fmt(r["gap"][0], "{:.1f}")} {ref} {fmt(r["gap"][1], "{:.1f}")}')
    # settle profile (raw rows: the settle rows are in it)
    for r in (r1, r2):
        A = r['anchor_frame']
        r['rawrows'] = [dict(x, rel=x['frame'] - A) for x in r['rows']]
    period = r1['start'][1]
    bn = {}
    for p in r1['phases']:
        bn[p['no']] = (p['rel0'], 1 if p['name'] == on else -1)
    prof, plateau, lag, se, tail = settle_profile(r1, r2, on, ref, bn, r1['settle'], period)
    if plateau is None:
        print('settle profile: effect too small (or too few blocks) to measure the lag')
        lag_msg = None
    else:
        print('settle profile (mean signed delta of frame_ms per row after a flip, rows 1..%d, plateau %+.2f ms): ' % (len(prof), plateau) + ' '.join('%+.1f' % x for x in prof[:min(len(prof), 12)]) + ' ...')
        if lag is None:
            lag_msg = None   # the plateau is within noise: no lag to measure
        else:
            print(f'settle lag: the delta crosses zero at row {lag} after the flip (settle {r1["settle"]})')
            lag_msg = None
        if tail:
            print(f'settle tail: rows {r1["settle"] // 2 + 1}..{r1["settle"]} minus the plateau: {tail[0]:+.2f} ms (t interval {tail[1]:+.2f} .. {tail[2]:+.2f}, {len(bn) - 1} blocks)')
        if tail and (tail[1] > 0 or tail[2] < 0) and abs(tail[0]) > 0.25 * abs(plateau):
            lag_msg = (f'SETTLE SHORT: the second half of the settle window differs from the plateau by {tail[0]:+.2f} ms (interval {tail[1]:+.2f} .. {tail[2]:+.2f}, excludes 0): '
                       f'the old phase still shows at row {r1["settle"]} (the delta crosses zero at row {lag}); raise settle')
    for m, label in (('shown_fps', 'fps'), ('gap_p99', 'ms of 1 % low gap'), ('gpu_ms', 'ms of gpu_ms'), ('frame_ms', 'ms of frame_ms')):
        if m not in out:
            continue
        c = out[m]
        v = verdict_of(c, on, ref, label)
        hk = {'frame_ms': 'half_frame_ms', 'gpu_ms': 'half_gpu_ms'}.get(m)
        if hk:
            hv = [r[hk] for r in per.values() if hk in r]
            ht = t_interval(hv) if len(hv) >= 3 else None
            if ht and (c['lo'] > 0 or c['hi'] < 0) and (ht[1] > 0 or ht[2] < 0) and abs(ht[0]) > (c['hi'] - c['lo']) / 2 and abs(ht[0]) > 0.25 * abs(c['mean']) if c['lo'] is not None else False:
                v += f' | SLOW STATE: the delta of a phase\'s second half differs from its first half by {ht[0]:+.2f} ms (interval {ht[1]:+.2f} .. {ht[2]:+.2f}, excludes 0): a controller dwell or a cache is still settling, so lengthen settle or this lever is not flip-able'
        if lag_msg:
            v += ' | ' + lag_msg
        print(f'VERDICT {m} {on}-{ref}: {v}')
    for tag, r in (('run 1', r1), ('run 2', r2)):
        for m in ('frame_ms', 'gpu_ms'):
            d = paired(r['phases'], r['power'], m, ref).get(on)
            if d and d['deltas']:
                print(f'single run {tag} alone, {m}: within-run adjacent-phase delta {sum(d["deltas"]) / len(d["deltas"]):+.2f} ({len(d["deltas"])} pairs), naive whole-run {fmt(d["naive"])}')
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('step')
    ap.add_argument('--pair', help='the OTHER run of a two-run flip job (complementary schedule, first=on)')
    ap.add_argument('--rel', help='pair mode: only rel frames R0:R1 (frames after the anchor)')
    ap.add_argument('--run', type=int)
    ap.add_argument('--settle', type=int)
    ap.add_argument('--frames')
    ap.add_argument('--no-window', action='store_true')
    ap.add_argument('--null-blocks', type=int)
    ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args()
    try:
        if a.pair:
            return pair_main(a)
        n, lg, info, rows, power = read_run(a.step, a.run)
        win = None
        wclock = None   # S102 B1: the window's clock seconds, for the thermal state
        if a.frames:
            x, y = a.frames.split(':')
            win = (int(x), int(y))
        elif not a.no_window:
            for kind in ('heavy', 'entrance', 'match'):
                try:
                    w = bl.windows(kind, lg['marks'], Path(a.step).name)[0]
                    win = (w[1], w[2])
                    wclock = (w[3], w[4])
                    break
                except bl.Refused:
                    continue
        st = info['start']
        settle = a.settle if a.settle is not None else (st[2] if st else 30)
        if a.null_blocks:
            phases = null_phases(rows, win, a.null_blocks, lg['marks'][0] if lg['marks'] else None)
            problems = []
            title = f'NULL schedule (no flips, blocks of {a.null_blocks} frames): the delta must be ~0'
        else:
            phases = build_phases(info, rows, settle, win)
            problems = switch_check(info)
            title = f'flip run {n}: period {st[1] if st else "?"}, settle {settle}, cycle {st[5] if st else "?"}'
        if len(phases) < 2:
            raise Refused('fewer than two usable phases in the window')
        ref = phases[0]['name'] if (a.null_blocks or not st) else st[6]
        print(f'{title} | window {win[0]}..{win[1]}' if win else title, '| phases %d, reference %s' % (len(phases), ref))
        if wclock and (Path(a.step) / 'power.csv').is_file():   # S102 B1: burst-aware thermal state of the window (burst_rules.py, R28)
            th = burst_rules.classify(burst_rules.read_power(Path(a.step) / 'power.csv'), wclock[0], wclock[1])
            print('heat: ' + burst_rules.text(th) + (f'; window mean {th["perf"]:.0f} %, {th["w"]:.1f} W' if th['perf'] is not None else '') + ' (a flip compares phases inside one run: a BURST window still pairs fairly, its absolute fps is not the bar)')
        print_census(a.step, n, info, st[6] if st else None)
        print('phase list: ' + ' '.join(f'{p["name"]}{p["no"]}[{p["rows"][0]["frame"]}..{p["rows"][-1]["frame"]}]' for p in phases))
        if problems:
            for p in problems:
                print('MISMATCH: ' + p)
            print('VERDICT: MISMATCH: the phases the log names are not what the game applied: no number is trusted from this run')
            return 3
        if phases[-1]['name'] != ref and len(phases) >= 3:
            print('note: the last phase is not the reference, so its pair has no later reference and a drift-biased partner (write the schedule ending in the reference)')
        shown = [m for m in METRICS + ['pkg_w'] if any(phase_value(p, m, power) is not None for p in phases)]
        verdicts = []
        for m in shown:
            r = paired(phases, power, m, ref)
            for name, d in r.items():
                iv = interval(d['deltas'])
                rv = d['ref_vals']
                spread = (max(rv) - min(rv)) if len(rv) >= 2 else None
                lvl = sum(rv) / len(rv) if rv else None
                if iv is None:
                    line = f'{m:<20} {name} - {ref}: pairs {len(d["deltas"])} ({d["kind"]}): too few for an interval | naive {fmt(d["naive"])}'
                else:
                    line = (f'{m:<20} {name} - {ref}: {iv[0]:+.2f}  t95 [{iv[1]:+.2f}, {iv[2]:+.2f}]  boot [{iv[3]:+.2f}, {iv[4]:+.2f}]  pairs {len(d["deltas"])} ({d["kind"]}, {d["n_two"]} two-sided)'
                            f' | naive whole-run {fmt(d["naive"])} | {ref} level {fmt(lvl, "{:.2f}")}, {ref}-{ref} spread {fmt(spread, "{:.2f}")}')
                if not a.quiet or m in ('frame_ms', 'gpu_ms', 'pkg_w'):
                    print(line)
                if m in ('frame_ms', 'gpu_ms', 'p99_ms'):
                    verdicts.append((m, name, iv, d, spread))
        for m, name, iv, d, spread in verdicts:
            if iv is None or len(d['deltas']) < 3:
                v = f'TOO FEW PAIRS ({len(d["deltas"])}: need 3 or more; lengthen the window or shorten the period)'
            elif iv[1] > 0 or iv[2] < 0:
                v = f'CLEAR: {name} is {abs(iv[0]):.2f} ms {"higher" if iv[0] > 0 else "lower"} than {ref} in {m} (95 % interval {iv[1]:+.2f} .. {iv[2]:+.2f}; one run alone: content is NOT cancelled, use a pair)'
            else:
                v = f'NOT SEPARATED: {m} difference {iv[0]:+.2f} ms, interval {iv[1]:+.2f} .. {iv[2]:+.2f} includes 0'
            if spread is not None and spread > 1.0:
                v += f' | DRIFT: the {ref} phases themselves differ by {spread:.2f} ms'
            pw = paired(phases, power, 'pkg_w', ref).get(name)
            if pw and pw['deltas'] and abs(sum(pw['deltas']) / len(pw['deltas'])) > POWER_W:
                v += f' | POWER STEP {sum(pw["deltas"]) / len(pw["deltas"]):+.1f} W between {name} and {ref} (the phases did not run at the same power)'
            print(f'VERDICT {m} {name}-{ref}: {v}')
        return 0
    except (Refused, bl.Refused) as e:
        print('REFUSED: ' + str(e))
        return 2


if __name__ == '__main__':
    sys.exit(main())
