"""Benchmark report for wwe2k14 runs: per-CP-frame extraction, scene stats, entrance variants, in-run A/B.

Session 6 harness (docs/optimization_candidates.md, specs B3, B4, B6, B7, B8 with the reviewers' corrections).
Reads only our own run output (timeline_NNN.csv, frames_NNN.csv, wwe2k14_NNN.log); writes derived numbers under
out/bench/ (gitignored). Never prints game data.

    py tools/bench/bench_report.py report 073 074 [--logs DIR] [--window LABEL=SCENE:A:B] [--ab NAME ...]
    py tools/bench/bench_report.py summary 080 081 082 083        (one line per run + arm comparison; run_bench.ps1)
    py tools/bench/bench_report.py classify 066 067 068 070 073 074
    py tools/bench/bench_report.py refs --scene tron_324 --variant X 066 067 070 074
    py tools/bench/bench_report.py verify 073 --frames 3896-5300  (timeline.py's numbers, for checking)

Definitions (same as tools/analysis/timeline.py):
  CP frame   one cp_swap to the next on the GPU command-processor thread. The k-th cp_swap is paired with the
             k-th VdSwap (smallest shift that keeps every cp_swap after its VdSwap), which gives each CP frame
             the guest frame number it presents.
  busy       span - cp_idle - wrm - hwait - intr (so the swap drain, endsub and vswap count as busy)
  busy_nd    busy minus the drain_b/drain_e scope (the CP waiting for the submit thread; new SDK event)
  asub       GPU Submit thread time (asub scopes) overlapping the CP frame
  bneck      max(busy_nd, asub): the pipeline bottleneck once the CP is fast
  kicks      the game's GPU kicks (wptr events) during the paired guest frame (VdSwap to VdSwap)
Scenes (guest frames; "+0" is the frame being built when the anchor happened, i.e. logged frame N -> N+1):
  tron_NNN   titantron\\NNN.bik2 opens -> next titantron open or LOADHD.PAC
  match      M3.PAC + 300 frames -> WINNING*.PAC, LOADHD.PAC, end of data, or --match-frames (default 3600)
  mark:NAME  timeline 'mark' events (a = id, b = guest frame) -> next mark or end of data
Windows: fixed offsets inside a scene, e.g. tron_324_pan = tron_324 +380..+500 (Bryan's heavy pan).
Route checks (B7): '# bench_expect: tron_343=1562:3 tron_324 match' in the input script, or --expect, makes a run
invalid when a scene is missing or has the wrong length; runs of one script must also reach the same scenes.
"""
import argparse
import bisect
import csv
import datetime
import glob
import json
import math
import os
import re
import sys
import time
from collections import Counter, OrderedDict, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
DEFAULT_LOGS = os.path.join(REPO, 'out', 'build', 'win-amd64-release', 'logs')
BENCH = os.path.join(REPO, 'out', 'bench')
REF_DIR = os.path.join(BENCH, 'reference')
RUNS_DIR = os.path.join(BENCH, 'runs')
REPORT_DIR = os.path.join(BENCH, 'reports')

ENTRANCE_SLOW_MS = 45.0      # cp_swap cadence >= 45 ms = a dropped frame at 30 fps (3 vblanks)
MATCH_SLOW_MS = 25.0         # >= 25 ms = a dropped frame at 60 fps
ENTRANCE_BUDGET_MS = 33.3
MATCH_BUDGET_MS = 16.7
MATCH_START_OFFSET = 300     # skip the match intro
ANCHOR_DEDUP_FRAMES = 30     # the same anchor file opened again within this many frames is the same event
VARIANT_FRAMES = 690
VARIANT_SMOOTH = 15
VARIANT_MIN_R = 0.9          # B3 correction: 0.98 would reject the valid X run 067 (0.972)
VARIANT_MAX_LAG = 3
VARIANT_LEVEL_FRAMES = 300   # level fallback window (entrances are not skipped this early)
VARIANT_SKIP_TAIL = 100      # frames dropped before an anchor that cut the scene short (skip -> next load)
VARIANT_SEGMENT = 115        # frames per letter of the per-segment pattern (690 = 6 segments)
VARIANT_SEG_MAD = 0.75       # kicks/frame: a segment matches a reference within this ...
VARIANT_SEG_MARGIN = 0.5     # ... and is this much closer than the next reference
VARIANT_MAX_MAD = 0.5        # kicks/frame; Brock light vs heavy: ~2.0 over 300 frames, light vs light 0.09-0.15
DEFAULT_SETTLE = 6
WIN_MIN_PCT = 3.0            # B6: a win must beat max(3%, 2 x the A/A run-to-run sd) ...
DEFAULT_AA_SD_PCT = 2.0      # ... (B6 guess from one A/A pair: 1.5-2%; replace with the calibration's number)
REGRESSION_PCT = 5.0         # B6: any scene's p99 +5%, or the match's p95 +5%, is a regression
SHORT_FRAC = 0.8             # summary: a run's scene shorter than this x the group's median length isn't compared
EXPECT_TOL = 3               # default tolerance (frames) of a scene-length expectation (B1: 1562-1564 for Brock)
MANIFEST_MAX_SKEW_S = 600    # a manifest whose start is further than this from the log's first line is stale

# (label, scene, start offset, end offset): frames [start+a, start+b) of the scene.
DEFAULT_WINDOWS = [
    ('tron_343_open', 'tron_343', 30, 150),   # Brock's opening shot (light: exactly 11 kicks/frame)
    ('tron_324_690', 'tron_324', 0, 690),     # Bryan, the window Session 5 was re-scored on
    ('tron_324_pan', 'tron_324', 380, 500),   # Bryan's heavy pan (both camera variants)
]

# Per-frame counters recorded on the CP thread right before cp_swap (SDK patch, Session 6). Old timelines lack them.
COUNTERS = OrderedDict([
    ('cpf', ('draws', 'host_draws', 'copies')),
    ('cpf_t0', ('t0_packets', 't0_regs', 't0_float_regs')),
    ('cpf_t0x', ('t0_fetch_regs', 't0_boolloop_regs', 't3_packets')),
    ('cpf_rr', ('rr_calls', 'rr_allvalid', 'rr_kib')),
    ('cpf_bind', ('tex_sets', 'const_sets', 'sampler_params')),
    ('cpf_fc', ('fc_writes', 'fc_changed', 'pipeline_changes')),
    ('cpf_cpu', ('cp_cpu', 'checkpoint', 'submissions')),
])
NOT_SUMMED = {'cp_cpu', 'checkpoint'}  # identifiers, not counts: keep the last value in the frame

HARD_FAILS = [
    ('crash', re.compile(r'\[wwe2k14\]\[crash\]')),
    ('device_lost', re.compile(r'device lost', re.I)),
    ('failed_to_await', re.compile(r'Failed to await')),
    ('script_failed', re.compile(r'script FAILED')),
]

LOG_TS = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\] \[(\w+)\]')
LOG_PREFIX = re.compile(r'^\[[^\]]*\] \[\w+\] \[[^\]]*\] (?:\[t\d+\] )?')
FILE_LINE = re.compile(r"\[wwe2k14\]\[file\] [^']*'([^']*)'.*-> 0x00000000\b")
FRAME_FIELD = re.compile(r'\bframe=(\d+)')
MARK_LINE = re.compile(r'\[wwe2k14\]\[input\] mark (\d+)(?: (.*?))? \(frame \d+\)')
MODIFIED_LINE = re.compile(r"\[wwe2k14\] modified: (\S+) = '(.*)' \(source: ")
CVAR_LINE = re.compile(r'\[wwe2k14\]\[input\] (?:line \d+(?: \([^)]*\))?: )?cvar (\S+) = ')
SCRIPT_LINE = re.compile(r'\[wwe2k14\]\[input\] replaying script (.+?) \(\d+ steps\)')
# Route expectations inside an input script (a comment, so the game ignores it), e.g.
#   # bench_expect: tron_343=1562:3 tron_324=2271 match
EXPECT_LINE = re.compile(r'^\s*#\s*bench_expect:\s*(.*?)\s*$', re.I)


# ----------------------------------------------------------------------------------------------- small helpers

def fnv1a32(text):
    h = 0x811C9DC5
    for byte in text.encode('utf-8'):
        h = ((h ^ byte) * 0x01000193) & 0xFFFFFFFF
    return h


def hx(s):
    return int(s, 16)


def run_id(spec):
    """'73', '073', 'timeline_073.csv', '...\\wwe2k14_073.log' -> '073'."""
    m = re.search(r'(\d+)(?:\.csv|\.log|\.json)?$', os.path.basename(str(spec)))
    if not m:
        raise SystemExit("can't read a run number from %r" % spec)
    return '%03d' % int(m.group(1))


def pct(sorted_vals, q):
    """Percentile with linear interpolation (numpy's default), on a sorted list."""
    if not sorted_vals:
        return float('nan')
    if len(sorted_vals) == 1:
        return sorted_vals[0]
    pos = (len(sorted_vals) - 1) * q / 100.0
    lo = int(math.floor(pos))
    hi = min(lo + 1, len(sorted_vals) - 1)
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (pos - lo)


def mean(vals):
    vals = [v for v in vals if v is not None]
    return sum(vals) / len(vals) if vals else float('nan')


def max_window_mean(vals, w=60):
    if not vals:
        return float('nan')
    if len(vals) <= w:
        return sum(vals) / len(vals)
    s = sum(vals[:w])
    best = s
    for k in range(w, len(vals)):
        s += vals[k] - vals[k - w]
        best = max(best, s)
    return best / w


def dist(vals):
    """mean / p50 / p95 / p99 / max of a list (ms)."""
    v = sorted(x for x in vals if x is not None)
    if not v:
        return dict(mean=float('nan'), p50=float('nan'), p95=float('nan'), p99=float('nan'), max=float('nan'))
    return dict(mean=sum(v) / len(v), p50=pct(v, 50), p95=pct(v, 95), p99=pct(v, 99), max=v[-1])


def fmt(x, nd=2):
    if x is None or (isinstance(x, float) and math.isnan(x)):
        return '-'
    if isinstance(x, int):
        return str(x)
    return '%.*f' % (nd, x)


def parse_expect(text):
    """'tron_343=1562:3 tron_324 match' -> [(scene, length or None, tolerance)]. A scene named alone must be
    present; with =LENGTH[:TOL] its length in guest frames must also be within TOL (default 3). ValueError if bad."""
    out = []
    for tok in re.split(r'[\s,;]+', text.strip()):
        if not tok:
            continue
        m = re.match(r'^([\w:.\-]+)(?:=(\d+)(?::(\d+))?)?$', tok)
        if not m:
            raise ValueError('bad expectation %r (want SCENE or SCENE=LENGTH[:TOLERANCE])' % tok)
        out.append((m.group(1), int(m.group(2)) if m.group(2) else None,
                    int(m.group(3)) if m.group(3) else EXPECT_TOL))
    return out


# ----------------------------------------------------------------------------------------------- timeline

def load_timeline(path):
    """event name -> list of (t_ns, seq, thread, a, b, c), sorted by (t, file order); a/b/c stay hex strings."""
    ev = defaultdict(list)
    with open(path, 'r', newline='') as f:
        f.readline()
        seq = 0
        for line in f:
            p = line.rstrip('\r\n').split(',')
            if len(p) != 6:
                break  # truncated last line of a crashed or killed run
            try:
                t = int(p[0])
                th = int(p[1])
            except ValueError:
                break
            ev[p[2]].append((t, seq, th, p[3], p[4], p[5]))
            seq += 1
    for v in ev.values():
        v.sort()
    return ev


def pair_scopes(ev, name, thread):
    """X_b / X_e on one thread -> [(t0, t1, begin_event)], paired with a stack in time order (timeline.py)."""
    if thread is None:
        return []
    merged = [(x[0], x[1], 0, x) for x in ev.get(name + '_b', ()) if x[2] == thread]
    merged += [(x[0], x[1], 1, x) for x in ev.get(name + '_e', ()) if x[2] == thread]
    merged.sort(key=lambda m: (m[0], m[1]))
    stack, out = [], []
    for t, _, kind, x in merged:
        if kind == 0:
            stack.append(x)
        elif stack:
            b = stack.pop()
            out.append((b[0], t, b))
    out.sort(key=lambda iv: iv[0])
    return out


class IntervalSum:
    """Total length of intervals clipped to [lo, hi), like timeline.py's clipped(), but with bisect + prefix sums."""

    def __init__(self, ivs):
        ivs = sorted((a, b) for a, b, *_ in ivs)
        self.s = [a for a, b in ivs]
        self.e = [b for a, b in ivs]
        self.pmax, self.cum = [], [0]
        m = -1 << 62
        for a, b in ivs:
            m = max(m, b)
            self.pmax.append(m)
            self.cum.append(self.cum[-1] + b - a)
        # Disjoint and in order: the fast path is exact. Otherwise sum the candidates one by one (still exact).
        self.disjoint = all(self.s[k] >= self.e[k - 1] for k in range(1, len(ivs)))

    def __len__(self):
        return len(self.s)

    def total(self, lo, hi):
        if not self.s:
            return 0
        j = bisect.bisect_left(self.s, hi)
        i = bisect.bisect_right(self.pmax, lo)
        if j <= i:
            return 0
        if self.disjoint:
            tot = self.cum[j] - self.cum[i]
            if self.s[i] < lo:
                tot -= lo - self.s[i]
            if self.e[j - 1] > hi:
                tot -= self.e[j - 1] - hi
            return tot
        tot = 0
        for k in range(i, j):
            d = min(self.e[k], hi) - max(self.s[k], lo)
            if d > 0:
                tot += d
        return tot


def build_cp_frames(ev):
    """Per-CP-frame table from a loaded timeline. Returns (frames, meta) or (None, meta)."""
    meta = {}
    vd = [(x[0], hx(x[3])) for x in ev.get('vdswap', ())]
    cps = [x[0] for x in ev.get('cp_swap', ())]
    meta['vdswaps'] = len(vd)
    meta['cp_swaps'] = len(cps)
    meta['events'] = sum(len(v) for v in ev.values())
    meta['event_counts'] = {k: len(v) for k, v in sorted(ev.items())}
    meta['vd_times'] = vd
    meta['guest_kicks'] = {}
    if not vd or len(cps) < 2:
        return None, meta
    cp_th = ev['cp_swap'][0][2]
    sub_th = ev['asub_b'][0][2] if ev.get('asub_b') else None
    meta['threads'] = dict(main=vd and ev['vdswap'][0][2], cp=cp_th, submit=sub_th)

    # Pair the k-th VdSwap with the k-th cp_swap (one XE_SWAP packet per VdSwap), exactly as timeline.py does.
    offset = None
    for shift in range(0, 5):
        if all(cps[i + shift] >= vd[i][0] for i in range(min(50, len(vd) - 1, len(cps) - shift - 1))):
            offset = shift
            break
    meta['pairing'] = 'index' if offset is not None else 'time'
    meta['offset'] = offset

    sums = {k: IntervalSum(pair_scopes(ev, k, cp_th))
            for k in ('cp_idle', 'wrm', 'hwait', 'intr', 'endsub', 'vswap', 'drain')}
    asub_ivs = pair_scopes(ev, 'asub', sub_th)
    asub = IntervalSum(asub_ivs)
    meta['asub_scopes'] = len(asub_ivs)
    meta['drain_scopes'] = len(sums['drain'])
    vd_t = [t for t, _ in vd]
    wptr = [x[0] for x in ev.get('wptr', ())]

    # Kicks per guest frame (vd index i: VdSwap i-1 to VdSwap i).
    kicks_by_index = [None] + [bisect.bisect_left(wptr, vd_t[i]) - bisect.bisect_left(wptr, vd_t[i - 1])
                               for i in range(1, len(vd))]

    frames = []
    neg_lag = 0
    for j in range(1, len(cps)):
        c0, c1 = cps[j - 1], cps[j]
        span = c1 - c0
        part = {k: s.total(c0, c1) for k, s in sums.items()}
        busy = span - part['cp_idle'] - part['wrm'] - part['hwait'] - part['intr']
        a_sub = asub.total(c0, c1)
        busy_nd = max(busy - part['drain'], 0)
        if offset is not None:
            i = j - offset
        else:
            # Fallback: the latest VdSwap before this cp_swap. The CP presents its frame some 60 ms after the VdSwap,
            # so these labels run late (073 forced into this path: +1 frame on 5324 of 5337 CP frames, more at 60 fps);
            # validity() marks such a run invalid instead of comparing it.
            i = bisect.bisect_right(vd_t, c1) - 1
        if 0 <= i < len(vd):
            guest = vd[i][1]
            lag = c1 - vd[i][0]
            if lag < 0:
                neg_lag += 1
            kicks = kicks_by_index[i]
            vd_ms = (vd_t[i] - vd_t[i - 1]) / 1e6 if i >= 1 else None
        else:
            guest, lag, kicks, vd_ms = None, None, None, None
        frames.append(OrderedDict([
            ('cp_frame', j), ('guest_frame', guest), ('t0_ms', c0 / 1e6), ('span', span / 1e6),
            ('busy', busy / 1e6), ('busy_nd', busy_nd / 1e6), ('wrm', part['wrm'] / 1e6),
            ('idle', part['cp_idle'] / 1e6), ('hwait', part['hwait'] / 1e6), ('intr', part['intr'] / 1e6),
            ('drain', part['drain'] / 1e6), ('endsub', part['endsub'] / 1e6), ('vswap', part['vswap'] / 1e6),
            ('asub', a_sub / 1e6), ('bneck', max(busy_nd, a_sub) / 1e6),
            ('lag', lag / 1e6 if lag is not None else None), ('kicks', kicks), ('vd_ms', vd_ms),
            ('_c0', c0), ('_c1', c1),
        ]))
    meta['negative_lags'] = neg_lag
    meta['guest_kicks'] = {vd[i][1]: kicks_by_index[i] for i in range(1, len(vd))}
    meta['vd_times'] = vd

    # Per-frame counters: an event recorded right before cp_swap j belongs to CP frame j (index j-1 in `frames`).
    present = [name for name in COUNTERS if ev.get(name)]
    meta['counters'] = present
    for name in present:
        cols = COUNTERS[name]
        for f in frames:
            for col in cols:
                f[col] = None
        for x in ev[name]:
            j = bisect.bisect_left(cps, x[0])
            if not 1 <= j < len(cps):
                continue
            f = frames[j - 1]
            for col, raw in zip(cols, x[3:6]):
                v = hx(raw)
                if col in NOT_SUMMED or f[col] is None:
                    f[col] = v
                else:
                    f[col] += v

    # GPU time per CP frame (gpu_ts a = submission, b = GPU us, c = is_swap). A submission belongs to the frame whose
    # draws it carries: a swap submission ends right after its cp_swap, any other one before the next cp_swap.
    if ev.get('gpu_ts'):
        endsub_t = {}
        for x in ev.get('endsub_b', ()):
            endsub_t[hx(x[4])] = x[0]
        for f in frames:
            f['gpu_ms'] = None
        for x in ev['gpu_ts']:
            sub, us, is_swap = hx(x[3]), hx(x[4]), hx(x[5])
            t = endsub_t.get(sub, x[0])
            j = bisect.bisect_right(cps, t) - (1 if is_swap else 0)
            if 1 <= j < len(cps):
                f = frames[j - 1]
                f['gpu_ms'] = (f['gpu_ms'] or 0.0) + us / 1000.0
        meta['gpu_ts'] = len(ev['gpu_ts'])

    # Drain reasons and submit-thread processors, per CP frame.
    if len(sums['drain']):
        for f in frames:
            f['drain_n'] = 0
        for t0, t1, b in pair_scopes(ev, 'drain', cp_th):
            j = bisect.bisect_right(cps, t0)
            if 1 <= j < len(cps):
                frames[j - 1]['drain_n'] += 1
    meta['drain_reasons'] = dict(Counter(hx(x[3]) for x in ev.get('drain_b', ())))
    meta['_asub_ivs'] = asub_ivs
    meta['_cps'] = cps
    return frames, meta


# ----------------------------------------------------------------------------------------------- frames + log

def load_frames_csv(path):
    """frames_NNN.csv -> (frame numbers, t_seconds, frame_ms); frame 1 (t=0) is added."""
    fr, ts, ms = [1], [0.0], [None]
    if not os.path.exists(path):
        return None
    with open(path, newline='') as f:
        f.readline()
        for line in f:
            p = line.strip().split(',')
            if len(p) != 3:
                break
            try:
                fr.append(int(p[0]))
                ts.append(float(p[1]))
                ms.append(float(p[2]))
            except ValueError:
                break
    return fr, ts, ms


def log_parts(logs, n):
    """wwe2k14_NNN.log plus rotated parts (wwe2k14_NNN.K.log, higher K = older), oldest first."""
    base = os.path.join(logs, 'wwe2k14_%s.log' % n)
    rotated = []
    for p in glob.glob(os.path.join(logs, 'wwe2k14_%s.*.log' % n)):
        m = re.match(r'wwe2k14_%s\.(\d+)\.log$' % n, os.path.basename(p))
        if m:
            rotated.append((int(m.group(1)), p))
    parts = [p for _, p in sorted(rotated, reverse=True)]
    if os.path.exists(base):
        parts.append(base)
    return parts


def normalize_error(msg):
    """Message kind for the histogram: hex and decimal numbers become '#' (names like wwe2k14 or d3d12 stay)."""
    msg = re.sub(r'0x[0-9A-Fa-f]+', '\x00', msg)
    msg = re.sub(r'(?<![A-Za-z0-9_])\d+(\.\d+)?', '#', msg)
    return msg.replace('\x00', '0x#').strip()[:200]


def parse_ts(s):
    return datetime.datetime.strptime(s, '%Y-%m-%d %H:%M:%S.%f')


def load_log(logs, n):
    """What the report needs from the game log: file opens, input-script lines, errors, hard fails."""
    out = dict(parts=log_parts(logs, n), first_frame_ts=None, opens=[], errors=Counter(), error_ts=[],
               hard=defaultdict(list), input=[], cvar_names={}, mark_names={}, start_ts=None, end_ts=None,
               settings=OrderedDict(), script=None)
    for part in out['parts']:
        with open(part, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = LOG_TS.match(line)
                if not m:
                    continue
                ts, level = m.group(1), m.group(2)
                if out['start_ts'] is None:
                    out['start_ts'] = ts
                out['end_ts'] = ts
                if '[wwe2k14]' in line:
                    if out['first_frame_ts'] is None and '[wwe2k14][frames] first guest frame' in line:
                        out['first_frame_ts'] = ts
                    elif '[wwe2k14][file]' in line:
                        fm = FILE_LINE.search(line)
                        if fm:
                            ff = FRAME_FIELD.search(line[fm.end(1):])
                            out['opens'].append((ts, fm.group(1), int(ff.group(1)) if ff else None))
                    elif '[wwe2k14][input]' in line:
                        text = LOG_PREFIX.sub('', line.rstrip())
                        out['input'].append((ts, text))
                        # input_script.h: "cvar NAME = VALUE (frame N) -> ok" and "mark ID [TEXT] (frame N)".
                        cm = MARK_LINE.search(text)
                        if cm:
                            label = re.sub(r'\s+', '_', (cm.group(2) or '').strip())[:40]
                            out['mark_names'][int(cm.group(1))] = label or cm.group(1)
                        cm = CVAR_LINE.search(text)
                        if cm:
                            out['cvar_names'][fnv1a32(cm.group(1))] = cm.group(1)
                        sm = SCRIPT_LINE.search(text) if out['script'] is None else None
                        if sm:
                            out['script'] = sm.group(1)  # as passed: relative to the working directory
                for key, pat in HARD_FAILS:
                    if pat.search(line):
                        out['hard'][key].append(LOG_PREFIX.sub('', line.rstrip())[:240])
                if level in ('warning', 'error', 'critical'):
                    out['errors'][level[0].upper() + ' ' + normalize_error(LOG_PREFIX.sub('', line))] += 1
                    out['error_ts'].append(ts)
                if '[wwe2k14] modified: ' in line:
                    mm = MODIFIED_LINE.search(line)
                    if mm:
                        out['settings'][mm.group(1)] = mm.group(2)
    return out


class FrameClock:
    """Maps log wall-clock timestamps to guest frames: the log's 'first guest frame' line is frame 1 at t=0 in
    frames_NNN.csv (B2's +-1 frame method). Returns N = frames presented before that moment."""

    def __init__(self, first_ts, frames):
        self.ok = bool(first_ts and frames)
        if self.ok:
            self.t_first = parse_ts(first_ts)
            self.fr, self.ts = frames[0], frames[1]

    def frame_at(self, ts):
        if not self.ok:
            return None
        dt = (parse_ts(ts) - self.t_first).total_seconds()
        if dt < -43200:
            dt += 86400  # midnight wrap
        # The first VdSwap at or after that moment ends the frame being built (Session 5's anchors.py rule).
        i = min(bisect.bisect_left(self.ts, dt), len(self.fr) - 1)
        return self.fr[i] - 1


# ----------------------------------------------------------------------------------------------- scenes

def anchor_kind(path):
    p = path.replace('/', '\\').lower()
    m = re.search(r'titantron\\(\d+)\.bik2$', p)
    if m:
        return 'tron', m.group(1)
    base = p.rsplit('\\', 1)[-1]
    if base == 'loadhd.pac':
        return 'loadhd', base
    if base == 'm3.pac':
        return 'm3', base
    if base.startswith('winning') and base.endswith('.pac'):
        return 'winning', base
    return None, None


def find_anchors(log, clock):
    """[(start_frame (+0), kind, key, path, source)], deduplicated. +0 = logged frame N + 1."""
    out = []
    last = {}
    diffs = []
    for ts, path, field in log['opens']:
        kind, key = anchor_kind(path)
        if not kind:
            continue
        by_ts = clock.frame_at(ts)
        if field is not None:
            n = field
            if by_ts is not None:
                diffs.append(field - by_ts)
            source = 'frame='
        else:
            n = by_ts
            source = 'timestamp'
        if n is None:
            continue
        start = n + 1
        prev = last.get((kind, key))
        if prev is not None and start - prev <= ANCHOR_DEDUP_FRAMES:
            continue
        last[(kind, key)] = start
        out.append((start, kind, key, path, source))
    out.sort(key=lambda a: a[0])
    return out, diffs


def build_scenes(anchors, marks, last_frame, match_frames):
    """[(name, kind, start, end)] with end exclusive; kind is 'tron', 'match' or 'mark'."""
    scenes = []
    seen = Counter()

    def name_for(base):
        seen[base] += 1
        return base if seen[base] == 1 else '%s_%d' % (base, seen[base])

    tron = None    # (name, start)
    match = None   # (name, start, limit)
    match_armed = True
    end_all = last_frame + 1

    def close_tron(at):
        nonlocal tron
        if tron and at > tron[1]:
            scenes.append((tron[0], 'tron', tron[1], at))
        tron = None

    def close_match(at):
        nonlocal match
        if match:
            end = min(at, match[2])
            if end > match[1]:
                scenes.append((match[0], 'match', match[1], end))
        match = None

    for start, kind, key, path, source in anchors:
        if kind == 'tron':
            close_tron(start)
            tron = (name_for('tron_' + key), start)
        elif kind == 'loadhd':
            close_tron(start)
            close_match(start)
            match_armed = True
        elif kind == 'm3':
            if match_armed and match is None:
                s = start + MATCH_START_OFFSET
                match = (name_for('match'), s, s + match_frames)
                match_armed = False
        elif kind == 'winning':
            close_match(start)
    close_tron(end_all)
    close_match(end_all)

    for k, (start, mid, name) in enumerate(marks):
        end = marks[k + 1][0] if k + 1 < len(marks) else end_all
        if name.lower() in ('end', 'stop', 'none'):
            continue
        if end > start:
            scenes.append((name_for('mark:' + name), 'mark', start, end))
    scenes.sort(key=lambda s: (s[2], s[0]))
    return scenes


def build_windows(scenes, specs):
    out = []
    for label, base, a, b in specs:
        for name, kind, s, e in scenes:
            if scene_base(name) != base:
                continue
            suffix = name[len(base):]
            ws, we = s + a, min(s + b, e)
            if we > ws:
                out.append((label + suffix, kind, ws, we, name))
    return out


def parse_window(spec):
    m = re.match(r'^([\w:.\-]+)=([\w:.\-]+):(-?\d+):(-?\d+)$', spec)
    if not m:
        raise SystemExit('bad --window %r (want LABEL=SCENE:START:END, offsets in guest frames)' % spec)
    return m.group(1), m.group(2), int(m.group(3)), int(m.group(4))


# ----------------------------------------------------------------------------------------------- stats

def scene_stats(kind, cp_frames, vd_rows, budget=None, slow=None):
    """Stats for one scene. cp_frames: rows from build_cp_frames in the scene (or []); vd_rows: frame_ms values
    from frames_NNN.csv for the scene's guest frames."""
    vdv = [x for x in vd_rows if x is not None]
    paced30 = kind == 'tron'
    if kind not in ('tron', 'match'):
        # Marked scenes: entrance-like (the game paces itself at 30 fps) when the median frame is >= 25 ms.
        ref = sorted(vdv) or sorted(f['span'] for f in cp_frames)
        paced30 = bool(ref) and pct(ref, 50) >= 25.0
    if slow is None:
        slow = ENTRANCE_SLOW_MS if paced30 else MATCH_SLOW_MS
    if budget is None:
        budget = ENTRANCE_BUDGET_MS if paced30 else MATCH_BUDGET_MS
    st = OrderedDict()
    st['guest_frames'] = len(vdv)
    if vdv:
        total_s = sum(vdv) / 1000.0
        st['fps'] = len(vdv) / total_s if total_s > 0 else float('nan')
        v = sorted(vdv)
        st['vd_ms_mean'] = sum(v) / len(v)
        st['vd_ms_p95'] = pct(v, 95)
        st['slow_vd'] = sum(1 for x in vdv if x >= slow)
    st['slow_ms'] = slow
    st['budget_ms'] = budget
    if not cp_frames:
        return st
    busy = [f['busy'] for f in cp_frames]
    d = dist(busy)
    st['cp_frames'] = len(cp_frames)
    for k, v in d.items():
        st['busy_' + k] = v
    st['busy_win60'] = max_window_mean(busy, 60)
    st['slow_cad'] = sum(1 for f in cp_frames if f['span'] >= slow)
    st['slow_cad_cpbound'] = sum(1 for f in cp_frames if f['span'] >= slow and f['busy'] >= budget)
    st['frames_over_budget'] = sum(1 for f in cp_frames if f['busy'] >= budget)
    for k in ('span', 'busy_nd', 'wrm', 'idle', 'hwait', 'intr', 'drain', 'endsub', 'vswap', 'asub'):
        st[k + '_mean'] = mean([f[k] for f in cp_frames])
    bn = sorted(f['bneck'] for f in cp_frames)
    st['bneck_mean'] = sum(bn) / len(bn)
    st['bneck_p95'] = pct(bn, 95)
    st['drained_frames'] = sum(1 for f in cp_frames if f['drain'] > 0)
    kk = [f['kicks'] for f in cp_frames if f['kicks'] is not None]
    st['kicks_mean'] = mean(kk)
    if 'gpu_ms' in cp_frames[0]:
        g = sorted(f['gpu_ms'] for f in cp_frames if f['gpu_ms'] is not None)
        if g:
            st['gpu_ms_mean'] = sum(g) / len(g)
            st['gpu_ms_p95'] = pct(g, 95)
    for name, cols in COUNTERS.items():
        if cols[0] not in cp_frames[0]:
            continue
        for col in cols:
            vals = [f[col] for f in cp_frames if f[col] is not None]
            if not vals:
                continue
            if col in NOT_SUMMED:
                if col == 'cp_cpu':
                    st['cp_cpu_top'] = ' '.join('%d:%d' % kv for kv in Counter(vals).most_common(3))
                continue
            st[col + '_mean'] = sum(vals) / len(vals)
    if 'draws' in cp_frames[0]:
        dsum = sum(f['draws'] or 0 for f in cp_frames)
        if dsum:
            st['us_per_draw'] = 1000.0 * sum(f['busy'] for f in cp_frames) / dsum
            st['us_per_draw_nd'] = 1000.0 * sum(f['busy_nd'] for f in cp_frames) / dsum
    return st


# ----------------------------------------------------------------------------------------------- one run

class Run:
    def __init__(self, n, logs, match_frames=3600, windows=None, quiet=False, expect=None):
        self.n = n
        self.logs = logs
        self.cli_expect = expect or []
        t_start = time.time()
        self.paths = dict(timeline=os.path.join(logs, 'timeline_%s.csv' % n),
                          frames=os.path.join(logs, 'frames_%s.csv' % n),
                          log=os.path.join(logs, 'wwe2k14_%s.log' % n))
        self.frames_csv = load_frames_csv(self.paths['frames'])
        self.log = load_log(logs, n)
        self.clock = FrameClock(self.log['first_frame_ts'], self.frames_csv)
        self.ev = load_timeline(self.paths['timeline']) if os.path.exists(self.paths['timeline']) else None
        self.cp, self.meta = (build_cp_frames(self.ev) if self.ev else (None, {}))
        self.vd_ms = {}
        if self.frames_csv:
            self.vd_ms = dict(zip(self.frames_csv[0], self.frames_csv[2]))
        last = max(self.vd_ms) if self.vd_ms else 0
        if self.meta.get('vdswaps'):
            last = max(last, self.meta['vd_times'][-1][1])
        self.last_frame = last
        self.anchors, self.anchor_diffs = find_anchors(self.log, self.clock)
        self.marks = self._marks()
        self.scenes = build_scenes(self.anchors, self.marks, last, match_frames)
        self.windows = build_windows(self.scenes, windows if windows is not None else DEFAULT_WINDOWS)
        self.manifest = load_manifest(n, logs, self.log['start_ts'])
        self.load_s = time.time() - t_start

    def _marks(self):
        out = []
        if not self.ev:
            return out
        for x in self.ev.get('mark', ()):
            mid, frame = hx(x[3]), hx(x[4])
            name = self.log['mark_names'].get(mid, str(mid) if mid < 100000 else '%08X' % mid)
            out.append((frame + 1, mid, name))
        out.sort()
        return out

    def cp_in(self, start, end):
        if not self.cp:
            return []
        return [f for f in self.cp if f['guest_frame'] is not None and start <= f['guest_frame'] < end]

    def vd_in(self, start, end):
        return [self.vd_ms.get(g) for g in range(start, end) if self.vd_ms.get(g) is not None]

    def stats(self, kind, start, end):
        st = scene_stats(kind, self.cp_in(start, end), self.vd_in(start, end))
        if not self.ev or not self.meta.get('vd_times'):
            return st
        # Timeline span of the scene: VdSwap of frame start-1 to VdSwap of frame end-1.
        vd = self.meta['vd_times']
        frames = [f for _, f in vd]
        i0 = bisect.bisect_left(frames, start - 1)
        i1 = bisect.bisect_left(frames, end - 1)
        if i0 >= len(vd):
            return st
        t_lo, t_hi = vd[i0][0], vd[min(i1, len(vd) - 1)][0]
        # vblank a = lateness in us (new SDK builds; 0 before).
        vb = self._vblank()
        k0, k1 = bisect.bisect_left(vb[0], t_lo), bisect.bisect_left(vb[0], t_hi)
        late = sorted(vb[1][k0:k1])
        if late and late[-1] > 0:
            st['vblank_late_us_mean'] = sum(late) / len(late)
            st['vblank_late_us_p99'] = pct(late, 99)
            st['vblank_late_us_max'] = late[-1]
        # asub_b b = submit-thread processor number (new SDK builds; 0 before).
        cpus = Counter(hx(b[4]) for t0, t1, b in self.meta.get('_asub_ivs', ()) if t_lo <= t0 < t_hi)
        if cpus and any(cpus):
            st['sub_cpu_top'] = ' '.join('%d:%d' % kv for kv in cpus.most_common(3))
        return st

    def _vblank(self):
        if not hasattr(self, '_vb'):
            ev = self.ev.get('vblank', ())
            self._vb = ([x[0] for x in ev], [hx(x[3]) for x in ev])
        return self._vb

    def kick_curve(self, start, end, length=VARIANT_FRAMES):
        """GPU kicks per guest frame from `start`, up to `length` frames or the scene end. A scene cut short by the
        next anchor (an entrance skipped with START) loses its last VARIANT_SKIP_TAIL frames: after a skip the game
        loads the next entrance (about 2 kicks/frame for 50-90 frames), which is not part of any camera variant."""
        if not self.cp:
            return None
        gk = self.meta['guest_kicks']
        stop = min(start + length, end)
        if end <= self.last_frame and end - start < length + VARIANT_SKIP_TAIL:
            stop = min(stop, end - VARIANT_SKIP_TAIL)
        return [gk.get(g) for g in range(start, max(stop, start))]

    def scene(self, name):
        for s in self.scenes:
            if s[0] == name:
                return s
        return None

    def errors_per_scene(self):
        out = {}
        if not self.clock.ok:
            return out
        frames = [self.clock.frame_at(ts) for ts in self.log['error_ts']]
        for name, kind, s, e in self.scenes:
            out[name] = sum(1 for f in frames if f is not None and s <= f + 1 < e)
        return out

    def script_path(self):
        """The input script: the manifest's absolute path (run_bench.ps1), else the one the log names, which is
        relative to the working directory (the exe folder, i.e. the logs folder's parent)."""
        mf = self.manifest or {}
        if mf.get('script'):
            return mf['script']
        rel = self.log.get('script')
        if not rel:
            return None
        if not os.path.isabs(rel):
            rel = os.path.join(os.path.dirname(os.path.abspath(self.logs)), rel)
        return os.path.normpath(rel)

    def script_id(self):
        p = self.script_path()
        return os.path.normcase(os.path.abspath(p)) if p else None

    def expectations(self):
        """B7 route checks: '# bench_expect: ...' lines in the input script, then --expect (wins per scene).
        Returns ([(scene, length, tol)], [problems reading them])."""
        exp, errs = OrderedDict(), []
        p = self.script_path()
        if p and os.path.exists(p):
            with open(p, encoding='utf-8', errors='replace') as f:
                for line in f:
                    m = EXPECT_LINE.match(line)
                    if m:
                        try:
                            for e in parse_expect(m.group(1)):
                                exp[e[0]] = e
                        except ValueError as err:
                            errs.append('script %s: %s' % (os.path.basename(p), err))
        for e in self.cli_expect:
            exp[e[0]] = e
        return list(exp.values()), errs

    def sync_lines(self):
        """B7: the input script's sync results ('line L: saw X after N frames'), quit frame, failures."""
        out = []
        for ts, text in self.log['input']:
            m = re.search(r"line (\d+)(?: \([^)]*\))?: saw '([^']*)' after (\d+) frames", text)
            if m:
                out.append((int(m.group(1)), m.group(2), int(m.group(3))))
        return out

    def quit_frame(self):
        for ts, text in self.log['input']:
            m = re.search(r'line \d+(?: \([^)]*\))?: quit \(frame (\d+)\)', text)
            if m:
                return int(m.group(1))
        return None

    def validity(self):
        problems = []
        for key, lines in self.log['hard'].items():
            if lines:
                problems.append('%s x%d' % (key, len(lines)))
        if self.manifest and not self.manifest.get('valid', True):
            problems.append('runner: ' + str(self.manifest.get('reason', 'invalid')))
        if self.ev and self.ev.get('run'):
            logged = hx(self.ev['run'][0][3])
            if logged != int(self.n):
                problems.append('timeline says run %d, file is %s' % (logged, self.n))
        if self.ev and self.meta.get('negative_lags'):
            problems.append('%d CP frames before their VdSwap (pairing broken?)' % self.meta['negative_lags'])
        if self.cp and self.meta.get('pairing') == 'time':
            problems.append('cp_swaps not paired with VdSwaps by index (time fallback: guest frame labels 1-2 frames '
                            'late)')
        # B7: the route's expected scenes (script '# bench_expect:' lines or --expect), e.g. Brock -> Bryan 1562+-3.
        expect, errs = self.expectations()
        problems += errs
        for name, length, tol in expect:
            sc = self.scene(name)
            if not sc:
                problems.append('expected scene %s missing' % name)
            elif length is not None and abs((sc[3] - sc[2]) - length) > tol:
                problems.append('scene %s is %d frames, expected %d+-%d' % (name, sc[3] - sc[2], length, tol))
        return problems

    def warnings(self, variants):
        """Not invalid, but not comparable as is: entrances whose camera variant is unknown (B3/B7: r below the cut
        against every reference, so a desync or a camera path without a reference)."""
        out = []
        for name, c in sorted(variants.items()):
            if c.get('variant') == 'unknown':
                out.append('%s variant unknown (best %s r %.3f, pattern %s): not compared' %
                           (name, c.get('best'), c.get('r', float('nan')), c.get('pattern')))
        return out


MANIFEST_DIR = [RUNS_DIR]


def _naive_ts(text):
    """'2026-10-08T09:12:33.1234567+11:00' or '2026-10-08 09:12:33.123' -> naive local datetime (seconds)."""
    m = re.match(r'(\d{4}-\d\d-\d\d)[T ](\d\d:\d\d:\d\d)', str(text or ''))
    return datetime.datetime.strptime(m.group(1) + ' ' + m.group(2), '%Y-%m-%d %H:%M:%S') if m else None


def load_manifest(n, logs=None, log_start=None):
    """run_bench.ps1's manifest for run NNN of this logs folder. run_bench.ps1 writes one subfolder per logs folder
    (out/bench/runs/<exe folder>_<hash>/run_NNN.json), because every exe folder numbers its runs from 001; a
    manifest directly in the folder is read too. Only a manifest whose logs_dir is this folder counts, and only if
    its start time is within MANIFEST_MAX_SKEW_S of the log's first line (older logs deleted, number reused)."""
    root = MANIFEST_DIR[0]
    paths = [os.path.join(root, 'run_%s.json' % n)] + sorted(glob.glob(os.path.join(root, '*', 'run_%s.json' % n)))
    same = lambda a, b: os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))
    t_log = _naive_ts(log_start)
    best = None
    for p in paths:
        if not os.path.exists(p):
            continue
        try:
            with open(p, encoding='utf-8-sig') as f:
                m = json.load(f)
        except (OSError, ValueError):
            continue
        if logs and m.get('logs_dir') and not same(m['logs_dir'], logs):
            continue
        t_run = _naive_ts(m.get('start'))
        skew = abs((t_log - t_run).total_seconds()) if t_log and t_run else 0.0
        if skew > MANIFEST_MAX_SKEW_S:
            continue
        if best is None or skew < best[0]:
            best = (skew, m)
    return best[1] if best else None


# ----------------------------------------------------------------------------------------------- variants (B3)

def smooth(vals, w=VARIANT_SMOOTH):
    if len(vals) < w:
        return []
    out = []
    s = sum(vals[:w])
    out.append(s / w)
    for k in range(w, len(vals)):
        s += vals[k] - vals[k - w]
        out.append(s / w)
    return out


def pearson(x, y):
    n = min(len(x), len(y))
    if n < 3:
        return float('nan')
    x, y = x[:n], y[:n]
    mx, my = sum(x) / n, sum(y) / n
    sxy = sum((a - mx) * (b - my) for a, b in zip(x, y))
    sxx = sum((a - mx) ** 2 for a in x)
    syy = sum((b - my) ** 2 for b in y)
    if sxx <= 0 or syy <= 0:
        return float('nan')
    return sxy / math.sqrt(sxx * syy)


def best_lag_r(x, y, maxlag=VARIANT_MAX_LAG):
    best = (0, float('nan'))
    for lag in range(-maxlag, maxlag + 1):
        a, b = (x[lag:], y) if lag >= 0 else (x, y[-lag:])
        r = pearson(a, b)
        if not math.isnan(r) and (math.isnan(best[1]) or r > best[1]):
            best = (lag, r)
    return best


def fill_curve(curve):
    """None (no data) -> the previous value, so smoothing keeps its length; leading Nones dropped."""
    out, last = [], None
    for v in curve:
        if v is None:
            if last is None:
                continue
            v = last
        out.append(float(v))
        last = v
    return out


def load_references(base):
    """{variant: {'mean': [...], 'runs': {run: [...]}}} from out/bench/reference/<scene>_<variant>.csv."""
    refs = {}
    for path in sorted(glob.glob(os.path.join(REF_DIR, base + '_*.csv'))):
        variant = os.path.basename(path)[len(base) + 1:-4]
        with open(path, newline='') as f:
            rows = list(csv.DictReader(f))
        if not rows:
            continue
        run_cols = [c for c in rows[0] if c.startswith('run_')]
        runs = {c[4:]: [float(r[c]) if r[c] not in ('', None) else None for r in rows] for c in run_cols}
        refs[variant] = dict(mean=[float(r['mean']) if r['mean'] else None for r in rows], runs=runs, path=path)
    return refs


def reference_curve(ref, exclude=None):
    """Mean kicks per offset over the reference runs, leaving `exclude` out when others remain."""
    runs = {k: v for k, v in ref['runs'].items() if k != exclude} or ref['runs']
    length = max(len(v) for v in runs.values())
    out = []
    for i in range(length):
        vals = [v[i] for v in runs.values() if i < len(v) and v[i] is not None]
        out.append(sum(vals) / len(vals) if vals else None)
    while out and out[-1] is None:
        out.pop()
    return out, sorted(runs)


def scene_base(name):
    """'tron_343_2' -> 'tron_343', 'match_2' -> 'match' (second occurrences in one run share the first's base)."""
    m = re.match(r'^(tron_\d+|.+?)(?:_\d+)?$', name)
    return m.group(1) if m else name


def best_lag_mad(x, y, maxlag=VARIANT_MAX_LAG):
    best = float('nan')
    for lag in range(-maxlag, maxlag + 1):
        a, b = (x[lag:], y) if lag >= 0 else (x, y[-lag:])
        n = min(len(a), len(b))
        if n < 3:
            continue
        d = sum(abs(p - q) for p, q in zip(a[:n], b[:n])) / n
        if math.isnan(best) or d < best:
            best = d
    return best


def curve_sd(v):
    if not v:
        return float('nan')
    m = sum(v) / len(v)
    return math.sqrt(sum((a - m) ** 2 for a in v) / len(v))


def segment_pattern(x, curves):
    """One letter per VARIANT_SEGMENT frames: the reference whose smoothed curve is within VARIANT_SEG_MAD
    kicks/frame there and clearly closer than the others; '=' when several match equally, '?' when none does.
    Shows runs that switch camera shots part-way (Session 6, run 077: X for ~300 frames, then Y-like)."""
    out = []
    n = min([len(x)] + [len(y) for y in curves.values()])
    for a in range(0, n, VARIANT_SEGMENT):
        b = min(a + VARIANT_SEGMENT, n)
        if b - a < 20:
            break
        mads = sorted((sum(abs(p - q) for p, q in zip(x[a:b], y[a:b])) / (b - a), v) for v, y in curves.items())
        close = [v for d, v in mads if d <= VARIANT_SEG_MAD]
        if not close:
            out.append('?')
        elif len(mads) > 1 and mads[1][0] - mads[0][0] < VARIANT_SEG_MARGIN:
            out.append('=' if len(close) > 1 else '?')
        else:
            out.append(mads[0][1][:1])
    return ''.join(out)


def classify_scene(run, scene_name, refs=None):
    """Best variant for one entrance scene of a run. Shape first: Pearson r of the smoothed kick curves over up to
    690 frames, best variant if r >= 0.9 (B3). A flat reference has no shape (Brock's light variant is exactly 11
    kicks/frame, so r is noise there): only against flat references (smoothed sd < 1 kick/frame over the first 300
    frames) the fallback is the level, mean |difference| over those 300 frames <= 0.5 kicks/frame. Otherwise
    'unknown'. `pattern` gives the closest reference per 115-frame segment (see segment_pattern)."""
    sc = run.scene(scene_name)
    if not sc or not run.cp:
        return None
    refs = refs if refs is not None else load_references(scene_base(scene_name))
    if not refs:
        return None
    raw = run.kick_curve(sc[2], sc[3])
    x = smooth(fill_curve(raw))
    scores = {}
    curves = {}
    for variant, ref in refs.items():
        y_raw, used = reference_curve(ref, exclude=run.n)
        y = smooth(fill_curve(y_raw))
        curves[variant] = y
        n = min(len(x), len(y))
        lag, r = best_lag_r(x[:n], y[:n])
        m = min(n, VARIANT_LEVEL_FRAMES)
        scores[variant] = dict(r=r, lag=lag, n=n, mad=best_lag_mad(x[:m], y[:m]), flat=curve_sd(y[:m]) < 1.0,
                               self_only=(used == [run.n]))
    shape = [(s['r'], v) for v, s in scores.items() if not math.isnan(s['r']) and s['r'] >= VARIANT_MIN_R]
    level = [(s['mad'], v) for v, s in scores.items()
             if s['flat'] and not math.isnan(s['mad']) and s['mad'] <= VARIANT_MAX_MAD]
    if shape:
        variant, method = max(shape)[1], 'shape'
    elif level:
        variant, method = min(level)[1], 'level'
    else:
        variant, method = 'unknown', '-'
    ranked = sorted(scores, key=lambda v: -1 if math.isnan(scores[v]['r']) else scores[v]['r'], reverse=True)
    best_v = variant if variant != 'unknown' else ranked[0]
    return dict(variant=variant, method=method, best=best_v, r=scores[best_v]['r'], lag=scores[best_v]['lag'],
                mad=scores[best_v]['mad'], frames=len(raw), scores=scores, pattern=segment_pattern(x, curves),
                kicks_mean=mean([v for v in raw if v is not None]),
                kicks_first150=mean([v for v in raw[:150] if v is not None]))


def classify_run(run):
    out = {}
    for name, kind, s, e in run.scenes:
        if kind != 'tron':
            continue
        c = classify_scene(run, name)
        if c:
            out[name] = c
    return out


def cmd_refs(args):
    os.makedirs(REF_DIR, exist_ok=True)
    curves = {}
    for spec in args.runs:
        n = run_id(spec)
        run = Run(n, args.logs, quiet=True)
        sc = run.scene(args.scene)
        if not sc:
            print('run %s: no scene %s (scenes: %s)' % (n, args.scene, ', '.join(s[0] for s in run.scenes)))
            continue
        curve = run.kick_curve(sc[2], sc[3], args.frames)
        if curve is None:
            print('run %s: no timeline' % n)
            continue
        curves[n] = curve
        print('run %s: %s +0 = guest frame %d, %d frames, %.2f kicks/frame' %
              (n, args.scene, sc[2], len(curve), mean(curve)))
    if not curves:
        raise SystemExit('no curves; reference not written')
    path = os.path.join(REF_DIR, '%s_%s.csv' % (args.scene, args.variant))
    length = max(len(c) for c in curves.values())
    with open(path, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['offset', 'mean'] + ['run_' + n for n in sorted(curves)])
        for i in range(length):
            vals = [curves[n][i] if i < len(curves[n]) else None for n in sorted(curves)]
            present = [v for v in vals if v is not None]
            w.writerow([i, '%.3f' % (sum(present) / len(present)) if present else ''] +
                       ['' if v is None else v for v in vals])
    print('wrote %s (%d runs, %d frames)' % (path, len(curves), length))


def cmd_classify(args):
    print('Entrance variants from GPU kicks per guest frame (%d frames from the titantron open, %d-frame smoothing, '
          'best lag within +-%d; the run is left out of its own reference when other runs remain). shape: Pearson '
          'r >= %.2f; else level: mean |diff| <= %.2f kicks/frame over the first %d frames; else unknown.' %
          (VARIANT_FRAMES, VARIANT_SMOOTH, VARIANT_MAX_LAG, VARIANT_MIN_R, VARIANT_MAX_MAD, VARIANT_LEVEL_FRAMES))
    for spec in args.runs:
        n = run_id(spec)
        run = Run(n, args.logs, quiet=True)
        if not run.cp:
            print('%s: no timeline' % n)
            continue
        res = classify_run(run)
        if not res:
            print('%s: no entrance scene with a reference (%s)' % (n, ', '.join(s[0] for s in run.scenes) or 'none'))
            continue
        for scene, c in res.items():
            scores = '  '.join('%s r %.3f mad %.2f%s' % (v, s['r'], s['mad'], '*' if s['self_only'] else '')
                               for v, s in sorted(c['scores'].items()))
            print('%s %-10s -> %-8s (%-5s) %-7s | %s | kicks/frame %.2f (first 150: %.2f), %d frames' %
                  (n, scene, c['variant'], c['method'], c['pattern'], scores, c['kicks_mean'], c['kicks_first150'],
                   c['frames']))
    print('(* = the reference holds only this run, so that score is the run against itself. The 6-letter pattern '
          'is the closest reference per %d-frame segment: = several match, ? none)' % VARIANT_SEGMENT)


# ----------------------------------------------------------------------------------------------- in-run A/B

def sign_test_p(pos, neg):
    n = pos + neg
    if n == 0:
        return float('nan')
    k = min(pos, neg)
    p = sum(math.comb(n, i) for i in range(0, k + 1)) / 2.0 ** n
    return min(1.0, 2 * p)


AB_MIN_PAIRS_CI = 5  # below this a bootstrap "CI" is just the range of a few values (2 pairs of one sign exclude 0)


def bootstrap_ci(diffs, iters=10000, seed=12345):
    if len(diffs) < AB_MIN_PAIRS_CI:
        return (float('nan'), float('nan'))
    try:
        import numpy as np
        rng = np.random.default_rng(seed)
        d = np.asarray(diffs, dtype=float)
        idx = rng.integers(0, len(d), size=(iters, len(d)))
        m = d[idx].mean(axis=1)
        return float(np.percentile(m, 2.5)), float(np.percentile(m, 97.5))
    except ImportError:
        import random
        rnd = random.Random(seed)
        ms = sorted(sum(rnd.choice(diffs) for _ in diffs) / len(diffs) for _ in range(iters))
        return pct(ms, 2.5), pct(ms, 97.5)


def ab_analysis(run, name, settle, scene=None, value_a=0, value_b=1, min_seg=3):
    """Split CP frames by the cvar's value (timeline 'cvar' events: a = fnv1a32(NAME), b = value, c = guest frame).
    The CP frame that contains a flip is dropped, then `settle` more frames. Returns a dict for printing."""
    if not run.cp:
        return dict(error='no timeline')
    if re.match(r'^0x[0-9A-Fa-f]+$', name):
        hashes = [int(name, 16)]
    else:
        hashes = [fnv1a32(name), fnv1a32(name.lower())]
    events = [(x[0], hx(x[4]), hx(x[5])) for x in run.ev.get('cvar', ()) if hx(x[3]) in hashes]
    if not events:
        seen = Counter(hx(x[3]) for x in run.ev.get('cvar', ()))
        return dict(error='no cvar events for %s (hash 0x%08X); present: %s' % (
            name, hashes[0], ', '.join('0x%08X%s x%d' % (h, ' (' + run.log['cvar_names'][h] + ')'
                                                          if h in run.log['cvar_names'] else '', c)
                                       for h, c in seen.items()) or 'none'))
    events.sort()
    flip_t = [t for t, v, g in events]
    # Where each segment ends. An event closes the segment before it, but the last segment has no closing event
    # (the value just stays in force to the end of the run), and a gap far longer than the others (toggling in two
    # blocks with a long wait between) spans other content. Both are cut at the median event-to-event interval, so
    # no segment covers much more of the scene than the ones it is paired with.
    gaps = sorted(flip_t[k + 1] - flip_t[k] for k in range(len(flip_t) - 1))
    typical = pct(gaps, 50) if gaps else None
    seg_end, capped = [], 0
    for k, t in enumerate(flip_t):
        natural = flip_t[k + 1] if k + 1 < len(flip_t) else None
        if typical is not None and (natural is None or natural - t > 2 * typical):
            seg_end.append(t + typical)
            capped += 1
        else:
            seg_end.append(natural)  # None only with a single event: nothing to pair it with anyway
    frames = run.cp
    if scene:
        sc = run.scene(scene) or next(((w[0], w[1], w[2], w[3]) for w in run.windows if w[0] == scene), None)
        if not sc:
            return dict(error='no scene or window %r in run %s' % (scene, run.n))
        lo_g, hi_g = sc[2], sc[3]
    else:
        lo_g, hi_g = -1, 1 << 60
    # Every cvar event starts a new segment (so A B B A gives four segments and alternating pair orders); the frame
    # containing an event is dropped, and after a value change the next `settle` frames too.
    segs = OrderedDict()   # event count before the segment -> (value, frames)
    prev_k, last_val, since_change = None, None, 0
    for f in frames:
        c0, c1 = f['_c0'], f['_c1']
        k = bisect.bisect_right(flip_t, c0)            # events at or before the frame start
        inside = bisect.bisect_left(flip_t, c1) > k    # an event during the frame
        val = events[k - 1][1] if k > 0 else None
        if k != prev_k:
            if val != last_val:
                since_change = 0
            prev_k, last_val = k, val
        if inside:
            continue
        if k > 0 and seg_end[k - 1] is not None and c1 > seg_end[k - 1]:
            continue  # past the cut end of a segment that had no closing event in time
        since_change += 1
        in_scene = f['guest_frame'] is not None and lo_g <= f['guest_frame'] < hi_g
        if since_change > settle and val is not None and in_scene:
            segs.setdefault(k, (val, []))[1].append(f)
    seglist = [(val, fs) for k, (val, fs) in segs.items() if len(fs) >= min_seg]
    metrics = ['busy', 'busy_nd', 'asub', 'bneck', 'span']
    if frames and 'draws' in frames[0]:
        metrics.append('draws')
    if frames and 'gpu_ms' in frames[0]:
        metrics.append('gpu_ms')
    arms = OrderedDict()
    for label, v in (('A', value_a), ('B', value_b)):
        fs = [f for val, s in seglist if val == v for f in s]
        arms[label] = dict(value=v, frames=len(fs), segments=sum(1 for val, s in seglist if val == v),
                           busy=dist([f['busy'] for f in fs]),
                           means={m: mean([f.get(m) for f in fs]) for m in metrics})
    # Non-overlapping adjacent pairs (s0,s1), (s2,s3)... of opposite arms, difference B - A.
    pairs = []
    k = 0
    while k + 1 < len(seglist):
        (va, fa), (vb, fb) = seglist[k], seglist[k + 1]
        if {va, vb} == {value_a, value_b}:
            a_fs, b_fs = (fa, fb) if va == value_a else (fb, fa)
            pair = {m: mean([f.get(m) for f in b_fs]) - mean([f.get(m) for f in a_fs]) for m in metrics}
            pair.update(a_busy=mean([f['busy'] for f in a_fs]), order='AB' if va == value_a else 'BA')
            pairs.append(pair)
            k += 2
        else:
            k += 1
    # Every adjacent opposite pair (overlapping): cancels a linear drift when the order alternates.
    adj = []
    for k in range(len(seglist) - 1):
        (va, fa), (vb, fb) = seglist[k], seglist[k + 1]
        if {va, vb} == {value_a, value_b}:
            a_fs, b_fs = (fa, fb) if va == value_a else (fb, fa)
            adj.append(mean([f['busy'] for f in b_fs]) - mean([f['busy'] for f in a_fs]))
    res = dict(name=run.log['cvar_names'].get(hashes[0], name), hash='0x%08X' % hashes[0], flips=len(events),
               settle=settle, scene=scene, capped=capped,
               typical_ms=typical / 1e6 if typical is not None else float('nan'),
               arms=arms, pairs=len(pairs), metrics={}, adjacent_mean=mean(adj) if adj else float('nan'),
               adjacent_n=len(adj), other_values=sorted({v for v, s in seglist if v not in (value_a, value_b)}))
    for m in metrics:
        d = [p[m] for p in pairs if p[m] is not None and not math.isnan(p[m])]
        if not d:
            continue
        pos = sum(1 for x in d if x > 0)
        neg = sum(1 for x in d if x < 0)
        lo, hi = bootstrap_ci(d)
        res['metrics'][m] = dict(mean=sum(d) / len(d), median=pct(sorted(d), 50), pos=pos, neg=neg,
                                 p_sign=sign_test_p(pos, neg), ci_lo=lo, ci_hi=hi)
    if pairs:
        a_mean = mean([p['a_busy'] for p in pairs])
        res['busy_pct'] = 100.0 * res['metrics']['busy']['mean'] / a_mean if a_mean else float('nan')
        res['orders'] = dict(Counter(p['order'] for p in pairs))
    return res


def print_ab(res):
    if 'error' in res:
        print('  A/B: ' + res['error'])
        return
    print('  A/B on %s (%s): %d cvar events, settle %d CP frames%s; %s' %
          (res['name'], res['hash'], res['flips'], res['settle'],
           ', scene ' + res['scene'] if res['scene'] else '',
           ('%d segment(s) cut at the median event interval %.0f ms (the last one never has a closing event)' %
            (res['capped'], res['typical_ms'])) if res['capped'] else
           'one cvar event: no segments to pair'))
    for label, arm in res['arms'].items():
        b = arm['busy']
        extra = '  '.join('%s %s' % (m, fmt(v)) for m, v in arm['means'].items() if m != 'busy')
        print('    arm %s (value %d): %d frames in %d segments | busy mean %s p50 %s p95 %s p99 %s | %s' %
              (label, arm['value'], arm['frames'], arm['segments'], fmt(b['mean']), fmt(b['p50']), fmt(b['p95']),
               fmt(b['p99']), extra))
    if not res['pairs']:
        print('    no adjacent A/B segment pairs')
        return
    print('    %d adjacent A/B pairs (orders %s), difference B - A per pair:' % (res['pairs'], res.get('orders')))
    for m, s in res['metrics'].items():
        ci = ('95%% CI [%+.3f, %+.3f]' % (s['ci_lo'], s['ci_hi']) if not math.isnan(s['ci_lo']) else
              'CI needs %d+ pairs' % AB_MIN_PAIRS_CI)
        print('      %-8s mean %+.3f  median %+.3f  %s  sign %d+/%d- (p %.3f)%s' %
              (m, s['mean'], s['median'], ci, s['pos'], s['neg'], s['p_sign'],
               '  = %+.1f%% of A' % res['busy_pct'] if m == 'busy' else ''))
    if res['adjacent_n']:
        print('      busy, every adjacent pair (drift-cancelling when the order alternates): mean %+.3f over %d' %
              (res['adjacent_mean'], res['adjacent_n']))
    if res['other_values']:
        print('    (segments with other values ignored: %s)' % res['other_values'])


# ----------------------------------------------------------------------------------------------- report

SCENE_COLS = ['busy_mean', 'busy_p50', 'busy_p95', 'busy_p99', 'busy_max', 'busy_win60']


def scene_line(label, start, end, st, variant=''):
    cp = st.get('cp_frames')
    head = '  %-18s %6d-%-6d' % (label, start, end - 1)
    vd = '%5s fps vd %s/%s' % (fmt(st.get('fps'), 1), fmt(st.get('slow_vd')), st.get('guest_frames', 0))
    if not cp:
        return head + ' (no timeline) ' + vd + ('  ' + variant if variant else '')
    busy = ' '.join('%6s' % fmt(st[c]) for c in SCENE_COLS)
    extra = ''
    if 'draws_mean' in st:
        extra += ' draws %s us/draw %s' % (fmt(st['draws_mean'], 0), fmt(st.get('us_per_draw'), 1))
    if 'gpu_ms_mean' in st:
        extra += ' gpu %s' % fmt(st['gpu_ms_mean'])
    return ('%s %5d %s | cad %3d (%d cp) %s | asub %5s bneck %5s drain %s (%d) | kicks %5s%s%s' %
            (head, cp, busy, st['slow_cad'], st['slow_cad_cpbound'], vd, fmt(st['asub_mean']), fmt(st['bneck_mean']),
             fmt(st['drain_mean']), st['drained_frames'], fmt(st['kicks_mean'], 1), extra,
             ('  ' + variant) if variant else ''))


def run_report(run, args, write=True):
    out = OrderedDict(run=run.n, logs=run.logs, load_s=round(run.load_s, 2))
    out['manifest'] = run.manifest
    out['timeline'] = bool(run.ev)
    out['meta'] = {k: v for k, v in run.meta.items() if not k.startswith('_') and k not in ('guest_kicks', 'vd_times')}
    out['anchors'] = [dict(frame=a[0], kind=a[1], key=a[2], path=a[3], source=a[4]) for a in run.anchors]
    if run.anchor_diffs:
        out['anchor_field_minus_timestamp'] = dict(Counter(run.anchor_diffs))
    out['variants'] = {k: {kk: vv for kk, vv in v.items() if kk != 'scores'} for k, v in classify_run(run).items()} \
        if run.cp else {}
    scenes = []
    for name, kind, s, e in run.scenes:
        st = run.stats(kind, s, e)
        scenes.append(OrderedDict(name=name, kind=kind, start=s, end=e, **st))
    windows = []
    for label, kind, s, e, parent in run.windows:
        st = run.stats(kind, s, e)
        windows.append(OrderedDict(name=label, kind=kind, start=s, end=e, parent=parent, **st))
    out['scenes'] = scenes
    out['windows'] = windows
    out['errors_per_scene'] = run.errors_per_scene()
    out['hard_fails'] = {k: v[:3] for k, v in run.log['hard'].items()}
    out['validity'] = run.validity()
    out['warnings'] = run.warnings(out['variants'])
    out['script'] = run.script_path()
    out['expect'] = ['%s%s' % (n, '' if ln is None else '=%d:%d' % (ln, tol)) for n, ln, tol in run.expectations()[0]]
    out['sync'] = run.sync_lines()
    out['quit_frame'] = run.quit_frame()
    out['error_kinds'] = dict(run.log['errors'].most_common())
    out['settings'] = dict(run.log['settings'])  # the game's "[wwe2k14] modified: NAME = 'VALUE'" lines
    if args.ab:
        out['ab'] = [ab_analysis(run, name, args.settle, args.ab_scene, args.ab_a, args.ab_b, args.min_seg)
                     for name in args.ab]
    if write:
        os.makedirs(REPORT_DIR, exist_ok=True)
        if run.cp and not args.no_frames_csv:
            path = os.path.join(args.out, 'cpframes_%s.csv' % run.n)
            scene_of = {}
            for name, kind, s, e in run.scenes:
                if kind != 'mark':
                    for g in range(s, e):
                        scene_of.setdefault(g, name)
            mark_of = {}
            for name, kind, s, e in run.scenes:
                if kind == 'mark':
                    for g in range(s, e):
                        mark_of.setdefault(g, name)
            cols = [c for c in run.cp[0] if not c.startswith('_')]
            ab_cols = []
            if args.ab and run.ev.get('cvar'):
                for name in args.ab:
                    hs = [int(name, 16)] if name.startswith('0x') else [fnv1a32(name), fnv1a32(name.lower())]
                    evs = sorted((x[0], hx(x[4])) for x in run.ev['cvar'] if hx(x[3]) in hs)
                    if evs:
                        ab_cols.append((name, [t for t, v in evs], [v for t, v in evs]))
            with open(path, 'w', newline='') as f:
                w = csv.writer(f)
                w.writerow(cols + ['scene', 'mark'] + ['cvar:' + n for n, _, _ in ab_cols])
                for fr in run.cp:
                    row = [fmt(fr[c], 4) if isinstance(fr[c], float) else ('' if fr[c] is None else fr[c])
                           for c in cols]
                    g = fr['guest_frame']
                    row += [scene_of.get(g, ''), mark_of.get(g, '')]
                    for n_, ts, vs in ab_cols:
                        k = bisect.bisect_right(ts, fr['_c0'])
                        inside = bisect.bisect_left(ts, fr['_c1']) > k
                        row.append('' if k == 0 or inside else vs[k - 1])
                    w.writerow(row)
            out['cpframes_csv'] = path
        with open(os.path.join(args.out, 'report_%s.json' % run.n), 'w') as f:
            json.dump(out, f, indent=1, default=str)
        with open(os.path.join(args.out, 'scenes_%s.csv' % run.n), 'w', newline='') as f:
            rows = scenes + windows
            keys = []
            for r in rows:
                for k in r:
                    if k not in keys:
                        keys.append(k)
            w = csv.DictWriter(f, fieldnames=['run'] + keys)
            w.writeheader()
            for r in rows:
                w.writerow(dict(run=run.n, **{k: (fmt(v, 4) if isinstance(v, float) else v) for k, v in r.items()}))
    return out


def print_report(run, rep, args):
    m = rep['meta']
    print('== run %s  (%s; loaded in %.1f s)' % (run.n, run.logs, rep['load_s']))
    if run.manifest:
        mf = run.manifest
        print('   manifest: label %s arm %s valid %s %s flags %s' % (
            mf.get('label'), mf.get('arm'), mf.get('valid'), mf.get('reason') or '', mf.get('flags')))
    if rep['timeline']:
        print('   timeline: %d events, %d CP frames, pairing %s (offset %s, negative lags %d), counters: %s%s' % (
            m.get('events', 0), m.get('cp_swaps', 1) - 1, m.get('pairing'), m.get('offset'), m.get('negative_lags', 0),
            ', '.join(m.get('counters') or []) or 'none',
            ', drain scopes %d' % m['drain_scopes'] if m.get('drain_scopes') else ''))
    else:
        print('   timeline: none (frames_NNN.csv and the log only)')
    src = Counter(a['source'] for a in rep['anchors'])
    print('   anchors: %s%s' % (', '.join('%s %d' % kv for kv in src.items()) or 'none',
                                ('; frame= minus timestamp: %s' % rep['anchor_field_minus_timestamp'])
                                if rep.get('anchor_field_minus_timestamp') else ''))
    print('   validity: %s%s' % ('; '.join(rep['validity']) or 'ok',
                                 ('  (expected scenes: %s)' % ' '.join(rep['expect'])) if rep['expect'] else ''))
    for w in rep['warnings']:
        print('   warning: %s' % w)
    for k, lines in rep['hard_fails'].items():
        for line in lines[:2]:
            print('     %s: %s' % (k, line))
    if rep['sync'] and args.sync:
        print('   sync: ' + '  '.join("L%d %s +%d" % s for s in rep['sync']))
    if not rep['scenes']:
        print('   no scenes found')
    else:
        print('   %-18s %13s %5s %6s %6s %6s %6s %6s %6s | slow frames (cp_swap cadence, CP-bound) / VdSwap '
              '| per frame ms' % ('scene', 'guest frames', 'CPfr', 'busy', 'p50', 'p95', 'p99', 'max', 'win60'))
        variants = rep['variants']
        for sc in rep['scenes']:
            v = variants.get(sc['name'])
            vs = ''
            if v:
                vs = 'variant %s (%s, %s)' % (v['variant'], ('mad %.2f' % v['mad']) if v['method'] == 'level' else
                                              ('r %.3f' % v['r']), v['pattern'])
            print(scene_line(sc['name'], sc['start'], sc['end'], sc, vs))
        for wd in rep['windows']:
            print(scene_line('  ' + wd['name'], wd['start'], wd['end'], wd))
    extra_keys = ('cp_cpu_top', 'sub_cpu_top', 'vblank_late_us_mean', 'vblank_late_us_p99', 'gpu_ms_p95')
    if args.counters and any(sc.get(c + '_mean') is not None or any(k in sc for k in extra_keys)
                             for sc in rep['scenes'] + rep['windows'] for cols in COUNTERS.values() for c in cols):
        print('   counters per CP frame (mean), processors (cpu:frames), vblank lateness (us):')
        for sc in rep['scenes'] + rep['windows']:
            vals = ['%s %s' % (c, fmt(sc.get(c + '_mean'), 1)) for cols in COUNTERS.values() for c in cols
                    if sc.get(c + '_mean') is not None]
            for k in extra_keys:
                if k in sc:
                    vals.append('%s %s' % (k.replace('_top', ''), sc[k] if isinstance(sc[k], str) else fmt(sc[k], 1)))
            print('     %-18s %s' % (sc['name'], '  '.join(vals)))
    eps = rep['errors_per_scene']
    errs = rep['error_kinds']
    print('   log: %d warning/error lines in %d kinds%s' % (
        sum(errs.values()), len(errs),
        ('; per scene: ' + ', '.join('%s %d' % kv for kv in eps.items())) if eps else ''))
    for k, c in list(errs.items())[:args.errors]:
        print('     %6d  %s' % (c, k[:150]))
    for ab in rep.get('ab', []):
        print_ab(ab)
    if rep.get('cpframes_csv'):
        print('   per-CP-frame CSV: %s' % rep['cpframes_csv'])


def cli_expect(args):
    try:
        return [e for spec in (getattr(args, 'expect', None) or []) for e in parse_expect(spec)]
    except ValueError as err:
        raise SystemExit('--expect: %s' % err)


def cmd_report(args):
    windows = DEFAULT_WINDOWS + [parse_window(w) for w in (args.window or [])]
    expect = cli_expect(args)
    os.makedirs(args.out, exist_ok=True)
    for spec in args.runs:
        n = run_id(spec)
        if not log_parts(args.logs, n) and not os.path.exists(os.path.join(args.logs, 'timeline_%s.csv' % n)):
            print('== run %s: nothing in %s' % (n, args.logs))
            continue
        run = Run(n, args.logs, args.match_frames, windows, expect=expect)
        rep = run_report(run, args, write=not args.no_write)
        print_report(run, rep, args)


# ----------------------------------------------------------------------------------------------- summary

SUMMARY_METRICS = ('busy_mean', 'busy_p95', 'busy_p99', 'busy_win60', 'bneck_mean')


def content_curve(run, start, end):
    """The game's GPU kicks per guest frame over [start, end), smoothed like the variant curves: what the scene
    showed (our optimizations don't change it)."""
    gk = run.meta.get('guest_kicks') or {}
    return smooth(fill_curve([gk.get(g) for g in range(start, end)]))


def curves_agree(x, y):
    """Same content? Shape: Pearson r >= VARIANT_MIN_R at the best lag within +-3. When either curve is flat (sd < 1
    kick/frame, where r is noise, e.g. Brock's light shot) by level instead: mean |difference| <= VARIANT_MAX_MAD."""
    n = min(len(x), len(y))
    if n < 3:
        return True, 'too short to check'
    x, y = x[:n], y[:n]
    if curve_sd(x) < 1.0 or curve_sd(y) < 1.0:
        d = best_lag_mad(x, y)
        return (not math.isnan(d) and d <= VARIANT_MAX_MAD), 'mad %.2f' % d
    lag, r = best_lag_r(x, y)
    return (not math.isnan(r) and r >= VARIANT_MIN_R), 'r %.3f' % r


def comparable_rows(kind, entries):
    """Makes one scene's (or window's) runs of one camera variant comparable, so a difference between the arms comes
    from the arms and not from what the runs showed:
      - a run much shorter than the others (an entrance skipped earlier, a match that ended early) is left out:
        shorter than SHORT_FRAC x the median length;
      - the rest are cut to the shortest remaining length L, so every run covers the same frames +0..+L (less
        VARIANT_SKIP_TAIL frames when the lengths differ: the shortest run ends in the next load);
      - in entrances, a run whose kick curve over +0..+L doesn't match at least half of the others is left out. The
        variant label only covers +0..+690, and a camera can switch shots later on (077), so this checks the rest.
    entries: [(run, arm, scene row)] in run order. Returns (rows [(run, arm, stats over +0..+L)], L, notes)."""
    notes = []
    lens = sorted(sc['end'] - sc['start'] for run, arm, sc in entries)
    med = pct(lens, 50) if lens else 0
    keep = []
    for run, arm, sc in entries:
        ln = sc['end'] - sc['start']
        if ln < SHORT_FRAC * med:
            notes.append('%s short (%d of median %d frames)' % (run.n, ln, med))
        else:
            keep.append((run, arm, sc))
    if not keep:
        return [], 0, notes
    kept = [sc['end'] - sc['start'] for run, arm, sc in keep]
    length = min(kept)
    if max(kept) - length > EXPECT_TOL:
        # The shortest run was cut by its next anchor (an entrance skipped with START, a match ending): its last
        # frames are the next load (about 2 kicks/frame), not the content the longer runs show at those offsets.
        length = max(length - VARIANT_SKIP_TAIL, 1)
    if kind == 'tron' and len(keep) >= 2:
        curves = [content_curve(run, sc['start'], sc['start'] + length) for run, arm, sc in keep]
        agree = {}
        for i in range(len(keep)):
            for j in range(i + 1, len(keep)):
                agree[(i, j)] = agree[(j, i)] = curves_agree(curves[i], curves[j])
        need = int(math.ceil((len(keep) - 1) / 2.0))
        ok = []
        for i, row in enumerate(keep):
            others = [j for j in range(len(keep)) if j != i]
            if sum(1 for j in others if agree[(i, j)][0]) >= need:
                ok.append(row)
            else:
                notes.append('%s content differs (%s)' % (
                    row[0].n, ', '.join('%s vs %s' % (agree[(i, j)][1], keep[j][0].n) for j in others)))
        keep = ok
    rows = []
    for run, arm, sc in keep:
        st = run.stats(sc['kind'], sc['start'], sc['start'] + length)
        if st.get('cp_frames'):
            rows.append((run, arm, st))
    return rows, length, notes


def compare_arms(kind, variant, rows, length, notes, arm_order, args):
    """Arm medians and the B6 verdict for one scene (or window) and camera variant, rows from comparable_rows."""
    tag = 'variant %-8s' % variant
    have = [a for a in arm_order if any(arm == a for run, arm, st in rows)]
    head = '   %s compared over +0..+%d: %s' % (tag, length,
                                               ' '.join('%s/%s' % (run.n, arm) for run, arm, st in rows) or 'no runs')
    if kind == 'tron' and variant == '-':
        head += ' (no variant reference for this entrance yet: runs matched by kick curve only)'
    if notes:
        head += ' | left out: ' + '; '.join(notes)
    print(head)
    if len(have) < 2:
        print('   %s fewer than two arms left: not compared' % tag)
        return
    by_arm = {a: [st for run, arm, st in rows if arm == a] for a in have}
    ref_arm = have[0]
    meds = {}
    for metric in SUMMARY_METRICS:
        for a in have:
            meds[(a, metric)] = pct(sorted(st[metric] for st in by_arm[a]), 50)
        b = meds[(ref_arm, metric)]
        parts = ['%s %s (n %d)' % (ref_arm, fmt(b), len(by_arm[ref_arm]))]
        for a in have[1:]:
            parts.append('%s %s (n %d, %+.1f%%)' % (a, fmt(meds[(a, metric)]), len(by_arm[a]),
                                                    100.0 * (meds[(a, metric)] - b) / b if b else 0))
        print('   %s %-11s medians: %s' % (tag, metric, '  '.join(parts)))
    # B6 regression guard: any scene's p99 +5%, and the match's p95 +5% (match metrics are a guard only).
    guards = [('busy_p99', 'p99')] + ([('busy_p95', 'p95')] if kind == 'match' else [])
    regress = []
    for a in have[1:]:
        for metric, label in guards:
            b = meds[(ref_arm, metric)]
            change = 100.0 * (meds[(a, metric)] - b) / b if b else 0.0
            if change >= REGRESSION_PCT:
                regress.append('%s %s REGRESSION %+.1f%%' % (a, label, change))
    if len(have) == 2:
        # B6: interleaved runs paired in run order (adjacent runs of opposite arms).
        seq = [(arm, st['busy_mean']) for run, arm, st in rows]
        deltas, k = [], 0
        while k + 1 < len(seq):
            (a1, v1), (a2, v2) = seq[k], seq[k + 1]
            if a1 != a2:
                ref_v, oth_v = (v1, v2) if a1 == ref_arm else (v2, v1)
                deltas.append(100.0 * (oth_v - ref_v) / ref_v)
                k += 2
            else:
                k += 1
        if deltas:
            pos = sum(1 for d in deltas if d > 0)
            neg = sum(1 for d in deltas if d < 0)
            med = pct(sorted(deltas), 50)
            # B6 starting rule (refine after the A/A calibration): the median per-pair change beats
            # max(3%, 2 x A/A sd) and every pair agrees in sign (4 of 5 or better from 5 pairs up).
            thr = max(WIN_MIN_PCT, 2.0 * args.aa_sd)
            need = len(deltas) if len(deltas) < 5 else len(deltas) - 1
            if kind == 'match':
                verdict = 'match: regression guard only (its content differs from run to run, B6)'
            elif len(deltas) < 2:
                verdict = 'one pair: not enough to decide'
            elif med <= -thr and neg >= need:
                verdict = 'FASTER (beats %.1f%%, signs agree)' % thr
            elif med >= thr and pos >= need:
                verdict = 'SLOWER (beats %.1f%%, signs agree)' % thr
            else:
                verdict = 'no measurable change at %.1f%%' % thr
            print('   %s busy_mean per pair (%s vs %s, run order): %s | median %+.1f%%, %d+/%d-, sign p %.3f -> %s' % (
                tag, have[1], ref_arm, ' '.join('%+.1f%%' % d for d in deltas), med, pos, neg,
                sign_test_p(pos, neg), verdict))
    print('   %s regression guard (p99%s +%.0f%% vs %s): %s' % (
        tag, ' and p95' if kind == 'match' else '', REGRESSION_PCT, ref_arm, '; '.join(regress) or 'ok'))


def cmd_summary(args):
    """One block per scene: one row per run (arm, variant, key metrics), then arms compared within a variant."""
    windows = DEFAULT_WINDOWS + [parse_window(w) for w in (args.window or [])]
    expect = cli_expect(args)
    runs = []
    for spec in args.runs:
        n = run_id(spec)
        if not log_parts(args.logs, n):
            print('run %s: no log in %s' % (n, args.logs))
            continue
        run = Run(n, args.logs, args.match_frames, windows, expect=expect)
        rep = run_report(run, args, write=not args.no_write)
        arm = (run.manifest or {}).get('arm') or (run.manifest or {}).get('label') or '-'
        runs.append((run, rep, arm))
    if not runs:
        return
    # B7 across runs: runs of one input script must reach the same scenes. A scene missing in some of them (or found
    # in only some of them) means the route went differently: a desync. Such runs are invalid.
    by_script = OrderedDict()
    for run, rep, arm in runs:
        by_script.setdefault(run.script_id(), []).append((run, rep))
    for sid, members in by_script.items():
        if sid is None or len(members) < 2:
            continue
        have = {run.n: {sc['name'] for sc in rep['scenes']} for run, rep in members}
        seen = Counter(name for names in have.values() for name in names)
        for name, count in sorted(seen.items()):
            if count == len(members):
                continue
            for run, rep in members:
                if name not in have[run.n] and 2 * count >= len(members):
                    rep['validity'].append('scene %s missing (in %d of %d runs of this script)' %
                                           (name, count, len(members)))
                elif name in have[run.n] and 2 * count <= len(members):
                    rep['validity'].append('scene %s only in %d of %d runs of this script' %
                                           (name, count, len(members)))
    print('%-4s %-10s %-6s %s' % ('run', 'arm', 'valid', 'notes'))
    for run, rep, arm in runs:
        notes = '; '.join(rep['validity']) or 'ok'
        if rep['warnings']:
            notes += '; ' + '; '.join(rep['warnings'])
        var = ', '.join('%s=%s' % (k, v['variant']) for k, v in rep['variants'].items())
        mf = run.manifest or {}
        print('%-4s %-10s %-6s %s%s%s' % (run.n, arm, 'no' if rep['validity'] else 'yes', notes,
                                          ('  variants: ' + var) if var else '',
                                          ('  flags: ' + str(mf.get('flags'))) if mf.get('flags') else ''))
    # Settings that differ between the runs, from the game's own "modified:" lines (checks the arm flags took).
    keys = sorted({k for run, rep, arm in runs for k in rep['settings']})
    differing = [k for k in keys if len({rep['settings'].get(k, '(default)') for run, rep, arm in runs}) > 1]
    if differing:
        print('settings that differ between these runs (from the logs):')
        for k in differing:
            print('   %-40s %s' % (k, '  '.join('%s=%s' % (run.n, rep['settings'].get(k, '(default)'))
                                               for run, rep, arm in runs)))
    names, kinds = [], {}
    for run, rep, arm in runs:
        for sc in rep['scenes'] + rep['windows']:
            if sc['name'] not in names:
                names.append(sc['name'])
                kinds[sc['name']] = sc['kind']
    arm_order = []
    for run, rep, arm in runs:
        if arm not in arm_order:
            arm_order.append(arm)
    print('\nPer run: the whole scene. Arm comparisons: valid runs of one camera variant, over the frames they all '
          'cover (+0..+L), entrances also matched by kick curve; unknown variants are never compared.')
    for name in names:
        print('\n-- %s' % name)
        print('   %-4s %-10s %-10s %5s %6s %6s %6s %6s %6s %6s %5s %5s %6s %6s' % (
            'run', 'arm', 'variant', 'len', 'busy', 'p50', 'p95', 'p99', 'max', 'win60', 'cad', 'vd', 'asub', 'bneck'))
        groups = OrderedDict()   # variant -> [(run, arm, row)] of valid runs, in run order
        for run, rep, arm in runs:
            sc = next((s for s in rep['scenes'] + rep['windows'] if s['name'] == name), None)
            if not sc:
                continue
            v = rep['variants'].get(sc.get('parent', name))
            variant = v['variant'] if v else '-'
            ok = not rep['validity']
            if sc.get('cp_frames'):
                print('   %-4s %-10s %-10s %5d %s %5d %5s %6s %6s%s' % (
                    run.n, arm, variant, sc['end'] - sc['start'], ' '.join('%6s' % fmt(sc[c]) for c in SCENE_COLS),
                    sc['slow_cad'],
                    fmt(sc.get('slow_vd')), fmt(sc['asub_mean']), fmt(sc['bneck_mean']), '' if ok else '  INVALID'))
                if ok:
                    groups.setdefault(variant, []).append((run, arm, sc))
            else:
                print('   %-4s %-10s %-10s (no timeline) %s fps, %s slow VdSwap frames' % (
                    run.n, arm, variant, fmt(sc.get('fps'), 1), fmt(sc.get('slow_vd'))))
        if len({arm for entries in groups.values() for run, arm, sc in entries}) < 2:
            continue
        for variant, entries in groups.items():
            if variant == 'unknown':
                print('   variant unknown : %s not compared (r below %.2f against every reference: a desync, or a '
                      'camera path without a reference yet)' % (' '.join(r.n for r, a, sc in entries), VARIANT_MIN_R))
                continue
            if len({arm for run, arm, sc in entries}) < 2:
                continue
            rows, length, notes = comparable_rows(kinds[name], entries)
            compare_arms(kinds[name], variant, rows, length, notes, arm_order, args)
    # B7: each sync point of a script should land within a few frames in every run of it. Keyed by the text waited
    # for and its occurrence, not the line number: a repeat block logs the same line on every pass, and an edited
    # script moves line numbers.
    for sid, members in by_script.items():
        if sid is None or len(members) < 2:
            continue
        sync = OrderedDict()
        for run, rep in members:
            occ = Counter()
            for line_no, text, frames in rep['sync']:
                occ[text] += 1
                sync.setdefault((text, occ[text]), OrderedDict())[run.n] = (frames, line_no)
        if not sync:
            continue
        print('\nscript sync points, %s (frames waited per run; spread > 10 = possible desync):' %
              os.path.basename(sid))
        for (text, k), per in sync.items():
            vals = [f for f, ln in per.values()]
            spread = max(vals) - min(vals)
            lines = '/'.join(str(ln) for ln in sorted({ln for f, ln in per.values()}))
            missing = [run.n for run, rep in members if run.n not in per]
            print('   L%-7s %-26s %s  spread %d%s%s' % (
                lines, (text + ('' if k == 1 else ' #%d' % k))[:26], ' '.join('%s:%d' % (r, f) for r, (f, ln) in
                                                                               per.items()), spread,
                '  <-- CHECK' if spread > 10 else '', ('  missing in ' + ' '.join(missing)) if missing else ''))
    # Error kinds that appear in one arm and not another (B8).
    by_arm = defaultdict(set)
    for run, rep, arm in runs:
        by_arm[arm] |= set(rep['error_kinds'])
    if len(by_arm) >= 2:
        arms = sorted(by_arm)
        for a in arms:
            others = set().union(*[by_arm[b] for b in arms if b != a])
            new = sorted(by_arm[a] - others)
            if new:
                print('\nlog message kinds only in arm %s (%d):' % (a, len(new)))
                for k in new[:15]:
                    print('   ' + k[:160])
    if args.ab:
        for run, rep, arm in runs:
            for ab in rep.get('ab', []):
                print('\nrun %s:' % run.n)
                print_ab(ab)


# ----------------------------------------------------------------------------------------------- verify

def cmd_verify(args):
    """Means over a guest-frame range, with timeline.py's definitions (to check this extractor against it)."""
    n = run_id(args.run)
    t0 = time.time()
    ev = load_timeline(os.path.join(args.logs, 'timeline_%s.csv' % n))
    frames, meta = build_cp_frames(ev)
    lo, hi = (int(x) for x in args.frames.split('-'))
    sel = [f for f in frames if f['guest_frame'] is not None and lo <= f['guest_frame'] <= hi and f['vd_ms'] is not None]
    print('run %s guest frames %d-%d: %d CP frames (%.1f s)' % (n, lo, hi, len(sel), time.time() - t0))
    keys = ('span', 'busy', 'idle', 'wrm', 'hwait', 'intr', 'endsub', 'vswap', 'lag', 'asub', 'drain', 'busy_nd')
    print('  command processor per frame (ms): ' + '  '.join('%s %.4f' % (k, mean([f[k] for f in sel])) for k in keys))
    print('  GPU kicks (wptr) per frame: %.2f; guest frame %.2f ms avg' %
          (mean([f['kicks'] for f in sel]), mean([f['vd_ms'] for f in sel])))


# ----------------------------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    sub = ap.add_subparsers(dest='cmd')

    def common(p):
        p.add_argument('--logs', default=DEFAULT_LOGS, help='folder with timeline/frames/log files (default: %(default)s)')
        p.add_argument('--manifests', default=RUNS_DIR, help='run_bench.ps1 manifests (default: %(default)s)')

    def report_opts(p):
        common(p)
        p.add_argument('runs', nargs='+', help='run numbers (73, 073, or a file name)')
        p.add_argument('--out', default=REPORT_DIR, help='where cpframes_NNN.csv, report_NNN.json, scenes_NNN.csv go')
        p.add_argument('--no-write', action='store_true', help='print only')
        p.add_argument('--no-frames-csv', action='store_true', help='skip the per-CP-frame CSV')
        p.add_argument('--match-frames', type=int, default=3600, help='longest match scene, from M3.PAC + 300')
        p.add_argument('--window', action='append', help='extra window LABEL=SCENE:START:END (repeatable)')
        p.add_argument('--ab', action='append', help='cvar NAME (or 0xHASH) toggled in-run with the script\'s cvar command')
        p.add_argument('--ab-scene', help='restrict the A/B to one scene or window')
        p.add_argument('--settle', type=int, default=DEFAULT_SETTLE, help='CP frames dropped after each flip')
        p.add_argument('--ab-a', type=int, default=0, help='arm A value (default 0)')
        p.add_argument('--ab-b', type=int, default=1, help='arm B value (default 1)')
        p.add_argument('--min-seg', type=int, default=3, help='ignore A/B segments shorter than this (CP frames)')
        p.add_argument('--errors', type=int, default=8, help='error kinds to print')
        p.add_argument('--counters', action='store_true', help='print counter means per scene')
        p.add_argument('--sync', action='store_true', help='print the input script sync lines')
        p.add_argument('--aa-sd', type=float, default=DEFAULT_AA_SD_PCT,
                       help='summary: A/A run-to-run sd in %% for the win threshold (default %(default)s)')
        p.add_argument('--expect', action='append',
                       help="scenes the route must reach, e.g. 'tron_343=1562:3 tron_324 match' (SCENE or "
                            "SCENE=LENGTH[:TOL], guest frames); adds to the script's '# bench_expect:' lines")

    report_opts(sub.add_parser('report', help='per-run scene report (+ per-CP-frame CSV)'))
    report_opts(sub.add_parser('summary', help='compact multi-run table, arms compared within a variant'))
    p = sub.add_parser('classify', help='entrance camera variants (B3)')
    common(p)
    p.add_argument('runs', nargs='+')
    p = sub.add_parser('refs', help='build or refresh a variant reference curve from runs')
    common(p)
    p.add_argument('--scene', required=True, help='e.g. tron_324')
    p.add_argument('--variant', required=True, help='e.g. X')
    p.add_argument('--frames', type=int, default=VARIANT_FRAMES)
    p.add_argument('runs', nargs='+')
    p = sub.add_parser('verify', help="timeline.py's per-frame means over a guest-frame range")
    common(p)
    p.add_argument('run')
    p.add_argument('--frames', required=True, help='guest frame range A-B (inclusive)')

    argv = sys.argv[1:]
    if argv and argv[0] not in ('report', 'summary', 'classify', 'refs', 'verify', '-h', '--help'):
        argv = ['report'] + argv
    args = ap.parse_args(argv)
    MANIFEST_DIR[0] = getattr(args, 'manifests', RUNS_DIR)
    if args.cmd in ('report', None):
        cmd_report(args)
    elif args.cmd == 'summary':
        cmd_summary(args)
    elif args.cmd == 'classify':
        cmd_classify(args)
    elif args.cmd == 'refs':
        cmd_refs(args)
    elif args.cmd == 'verify':
        cmd_verify(args)


if __name__ == '__main__':
    main()
