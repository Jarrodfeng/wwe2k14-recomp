"""tools/regress: one command, short pass/fail (Session 8, task 3). Docs: docs/harness/regress.md.

    py tools\\regress\\regress.py                      run every route, compare with the baseline, exit 0 = all pass
    py tools\\regress\\regress.py --only lesnar,logo   some routes only
    py tools\\regress\\regress.py --extra-args="--some_flag=true"   test a flag (added to every route's own flags)
    py tools\\regress\\regress.py --exe C:\\path\\to\\other\\wwe2k14.exe
    py tools\\regress\\regress.py --dry-run            print what would be launched, launch nothing
    py tools\\regress\\regress.py --runs selftest=176 logo=177 lesnar=178     check existing runs, launch nothing
    py tools\\regress\\regress.py --set-baseline --runs lesnar=178,183,187 ...  baseline = median, threshold from spread
    py tools\\regress\\regress.py --list               routes and their checks
    py tools\\regress\\regress.py --show-baseline      the baseline as a table (runs, values, median, spread, threshold)
    py tools\\regress\\regress.py --parallel           functional routes side by side first, then the exclusive ones (L1)

Game runs go through tools\\bench\\run_list.ps1 (frozen user data mirrored before each run, timeline on, a run still
going after 720 s is killed) under ONE hold of the run lock (tools\\harness\\runlock.py ensure_lock), so no other
agent's run or build lands between the routes. With --parallel N (task L1; docs/harness/regress.md "Parallel
functional part") the FUNCTIONAL routes (no perf checks, no fixed seed: selftest, logo) first run through one
`run_list.ps1 -Parallel N` call, each in its own SHARED slot, and then the EXCLUSIVE routes (timed: lesnar, taker4k;
fixed seed: pause, see needs_exclusive) run as before, one after the other under one EXCLUSIVE hold (this script
re-runs itself inside it and reuses the functional runs, passed on in the file named by WWE2K14_REGRESS_PHASE1).
A build fingerprint (sha256 of the exe and the staged
rex*.dll) is taken before and after the parallel part and again inside the exclusive hold; if it changed (a build or
SDK restage landed in between), the functional runs are set aside and those routes run again inside the hold, so a
verdict never mixes two builds. Everything written goes to out\\harness\\regress\\ (gitignored):
baseline.json, regress_<stamp>.log (details) and regress_<stamp>.json (every metric and check result).
A FAILED route also gets a one-line crash-triage verdict under its line (task C: run_list's "[triage]" part, a
verdict saved earlier for this exe and run in out\\harness\\triage\\ (run_NNN.json for the default build,
<exe-folder-tag>_run_NNN.json for any other), or tools\\triage\\triage.py --json run now and saved there).

Extending (task 16 and later): a route's 'checks' list names functions registered with @check(NAME). A file
tools\\regress\\checks_<anything>.py that defines register(api) is imported at start (api.check, api.routes,
api.Result, api.compare, api.REPO, api.OUT, api.SCENES): it can register new checks and append their names to
routes (e.g. a screenshot diff against references kept in out\\harness\\), e.g.

    def register(api):
        @api.check('shot_diff')
        def shot_diff(ctx):
            return [api.Result('shot_diff', 'pass', '...')]
        for r in ('selftest', 'logo'):
            api.routes[r]['checks'].append('shot_diff')
A check gets a Ctx (route, run number, logs and captures folders, log lines, run_list line, metrics, baseline) and
returns a list of Result(name, status, detail) with status 'pass', 'fail', 'warn' (shown, does not fail), 'skip' or 'info'.
"""
import argparse
import bisect
import collections
import datetime
import glob
import hashlib
import importlib.util
import json
import math
import os
import re
import statistics
import subprocess
import sys
import time

REPO = r'C:\Users\Jarrod\wwe2k14-recomp'
DEFAULT_EXE = os.path.join(REPO, 'out', 'build', 'win-amd64-release', 'wwe2k14.exe')
RUN_LIST = os.path.join(REPO, 'tools', 'bench', 'run_list.ps1')
SCRIPTS_DIR = os.path.join(REPO, 'tools', 'input_scripts')
OUT = os.path.join(REPO, 'out', 'harness', 'regress')
BASELINE = os.path.join(OUT, 'baseline.json')
HERE = os.path.dirname(os.path.abspath(__file__))

sys.path.insert(0, os.path.join(REPO, 'tools', 'bench'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'harness'))
import bench_report  # noqa: E402  (HARD_FAILS, log_parts: the same hard-fail patterns as every bench report)

# ------------------------------------------------------------------------------------------------------- routes
# shots: screenshots the route's input script takes ("screenshot LABEL" -> captures\NNN_LABEL.png, .bmp before S17).
ROUTES = collections.OrderedDict()
# Session 12: the fast-load defaults (fast loader, load turbo, boot skip) change boot and load timing; every route
# pins them off so the baselines (recorded with the game's own pacing) stay comparable.
# Session 15b: the native zlib and the fast guest reads (defaults since 2026-10-13) are pinned off too (the baseline,
# runs 1167-1170, predates them); the in-game vblank rate only applies inside load-turbo windows, which are off here.
LOAD_PIN = '--wwe2k14_fast_loader=false --wwe2k14_load_turbo=false --wwe2k14_load_turbo_menu=false --wwe2k14_load_turbo_boot=false --wwe2k14_skip_boot_screens=false --wwe2k14_native_zlib=false --wwe2k14_fast_guest_reads=false'
# Session 24 follow-up (Jarrod, 2026-10-17): the timed routes pin present_guest_frames_once off (a Session 22 default).
# With it on, the old look's 30 fps entrances leave the card resting between frames at a low clock (P8 / P5,
# 230-1,170 MHz) and their GPU ms measure the clock, not the work (lesnar 14.4 ms vs 6.7 with it off at 2,685 MHz,
# runs 1723 / 1726); the baseline (2026-10-13) was taken with it off.
GPU_PIN = '--present_guest_frames_once=false'
# Flags a route pins to the SDK's built-in default: the log prints no "modified" line for them (the value doesn't
# change), so the flags check accepts them when the log shows no other value (e.g. one the app sets at runtime).
BUILTIN_DEFAULT_FLAGS = {'present_guest_frames_once': 'false'}
# Session 29 (Jarrod: "small wins"): the two functional routes run with the real load defaults (fast loader, load
# turbo, boot skip, native zlib, fast guest reads all on, as players run the game): no LOAD_PIN. Faster boot, and T2
# tests what ships. References rebuilt with these flags. The timed routes keep LOAD_PIN (their perf baselines).
ROUTES['selftest'] = dict(
    script='selftest_title.txt', extra='--wwe2k14_preset_1440p60=false', timeout=720,  # original look: baseline/refs
    shots=['title', 'storage_notice', 'main_menu'],
    checks=['flags', 'hard_fails', 'clean_exit', 'screenshots'])
ROUTES['logo'] = dict(
    script='repro_logo_lettering.txt', extra='--wwe2k14_preset_1440p60=false', timeout=720,  # original look
    shots=['logo_grid', 'editor', 'stamp_categories', 'after_lettering'],
    checks=['flags', 'hard_fails', 'clean_exit', 'pass_line', 'screenshots'])
ROUTES['lesnar'] = dict(
    script='bench_lesnar_bryan.txt', extra='--wwe2k14_preset_1440p60=false --vulkan_gpu_timestamps=true ' + LOAD_PIN + ' ' + GPU_PIN,
    timeout=720,
    shots=['b_arena', 'b_brock100', 'b_second440', 'b_match600', 'b_match3600'],
    checks=['flags', 'hard_fails', 'clean_exit', 'screenshots', 'scenes', 'perf'])
# The heaviest known case, launched like 7c's run 170 (only the 4K preset + GPU timestamps on top of defaults).
ROUTES['taker4k'] = dict(
    script='bench_taker_wm.txt', extra='--wwe2k14_preset_4k60=true --vulkan_gpu_timestamps=true ' + LOAD_PIN + ' ' + GPU_PIN,
    timeout=720,
    shots=['b_arena', 'b_brock100', 'b_second440', 'b_match600', 'b_match3600'],
    checks=['flags', 'hard_fails', 'clean_exit', 'screenshots', 'scenes', 'perf', 'bar60'])
# Command-line flags run_list.ps1 adds to every run (not part of a route's identity).
# Session 30: the test window flags (fullscreen behind other windows without the focus; -Windowed adds the last two).
# Session 32: the background throttling opt-out run_list adds with them (src/background_speed.h).
RUNLIST_FLAGS = {'user_data_root', 'cache_root', 'wwe2k14_timeline', 'window_activate_when_shown',
                 'window_show_behind', 'fullscreen', 'window_position', 'wwe2k14_background_full_speed'}

SCENES = ['brock', 'second', 'match']
SCENE_LABEL = {'brock': 'Brock', 'second': '2nd', 'match': 'match'}

# Compared with the baseline. dir: 'low' = lower is a regression, 'high' = higher is, 'both' = any change.
# Threshold = max(K x A/A spread, floor); floor(base) below. Rationale and the A/A numbers: docs/harness/regress.md.
SPREAD_K = 3.0
METRICS = collections.OrderedDict([
    ('fps', dict(dir='low', floor=lambda b: 0.25, fmt='%.2f')),
    ('n45', dict(dir='high', floor=lambda b: 3.0, fmt='%d')),
    ('gpu_ms', dict(dir='high', floor=lambda b: max(0.5, 0.10 * b), fmt='%.2f')),
    ('wall_s', dict(dir='both', floor=lambda b: max(0.5, 0.01 * b), fmt='%.1f')),
])
BAR60 = dict(window=600, min_window_fps=59.5, max_pct20=0.5)

Result = collections.namedtuple('Result', 'name status detail')
SETTING_BASELINE = False   # --set-baseline: 'perf' skips instead of failing for want of a baseline


# ------------------------------------------------------------------------------------------------- check registry
CHECKS = collections.OrderedDict()


def check(name):
    def deco(fn):
        CHECKS[name] = fn
        return fn
    return deco


class Ctx:
    def __init__(self, route_name, run, logs, captures, runlist_line=None, baseline=None, extra_args=''):
        self.route_name = route_name
        self.route = ROUTES[route_name]
        self.run = run                      # '183'
        self.logs = logs
        self.captures = captures
        self.runlist_line = runlist_line    # run_list.ps1's line for this run, None when checking an old run
        self.baseline = baseline            # baseline['routes'][route] or None
        self.extra_args = extra_args        # --extra-args of this call (expected on the run's command line)
        self._lines = None
        self._bench = None

    @property
    def cmdline_flags(self):
        """{flag: value} from the log's 'modified: X = 'V' (source: command line)' lines."""
        out = {}
        for x in self.lines[:400]:
            m = CMDLINE_FLAG.search(x)
            if m:
                out[m.group(1)] = m.group(2)
        return out

    @property
    def start_time(self):
        """The log's first timestamp (datetime) or None."""
        for x in self.lines[:5]:
            m = re.match(r'\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)', x)
            if m:
                return datetime.datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S')
        return None

    @property
    def lines(self):
        if self._lines is None:
            self._lines = []
            for p in bench_report.log_parts(self.logs, self.run):
                with open(p, encoding='utf-8', errors='replace') as f:
                    self._lines.extend(f.readlines())
        return self._lines

    @property
    def bench(self):
        if self._bench is None:
            self._bench = bench_metrics(self.logs, self.run, self.lines)
        return self._bench


CMDLINE_FLAG = re.compile(r"\] modified: (\w+) = '([^']*)' \(source: command line\)")
ANY_MODIFIED = re.compile(r"\] modified: (\w+) = '([^']*)' \(source: ([^)]*)\)")


def parse_flag_string(s):
    """'--a=1 --b' -> {'a': '1', 'b': 'true'}"""
    out = {}
    for tok in (s or '').split():
        if tok.startswith('--'):
            k, _, v = tok[2:].partition('=')
            out[k] = v.strip('"\'') if _ else 'true'
    return out


def expected_flags(route, extra_args):
    return parse_flag_string(route_extra(route, extra_args))


@check('flags')
def check_flags(ctx):
    """The run used this route's input script and exactly its flags (+ --extra-args); matters for --runs."""
    got = ctx.cmdline_flags
    if not got:
        return [Result('flags', 'fail', 'no command-line "modified:" lines in the log')]
    script = got.get('wwe2k14_input_script', '')
    if os.path.basename(script.replace('\\', '/')).lower() != ctx.route['script'].lower():
        return [Result('flags', 'fail', 'input script %r is not this route\'s %s' % (script, ctx.route['script']))]
    want = {k: v for k, v in expected_flags(ctx.route, ctx.extra_args).items() if k not in RUNLIST_FLAGS}
    have = {k: v for k, v in got.items() if k not in RUNLIST_FLAGS and k != 'wwe2k14_input_script'}
    diffs = []
    # Session 16: regress runs every route with -RealTime; a --vsync=false here means the run was fast-forwarded.
    for k, v in want.items():
        if k not in have:
            if BUILTIN_DEFAULT_FLAGS.get(k, '').lower() == v.lower():
                others = [m.group(2) for m in map(ANY_MODIFIED.search, ctx.lines) if m and m.group(1) == k]
                if not others:
                    continue  # left at the built-in default, which is the pinned value
                diffs.append('--%s=%s (route pins %s)' % (k, others[-1], v))
                continue
            diffs.append('missing --%s=%s' % (k, v))
        elif have[k].lower() != v.lower():
            diffs.append('--%s=%s (route wants %s)' % (k, have[k], v))
    for k, v in have.items():
        if k not in want:
            diffs.append('unexpected --%s=%s' % (k, v))
    if diffs:
        return [Result('flags', 'fail', 'run flags differ from the route: ' + '; '.join(diffs))]
    return [Result('flags', 'pass', '%s %s' % (ctx.route['script'], ' '.join('--%s=%s' % kv for kv in want.items())))]


def sdk_pin_of(logs, run):
    """The SDK pin line logs\\sdk_pin_runs.log has for wwe2k14_NNN (written by run_list from task 1 on), or None."""
    p = os.path.join(logs, 'sdk_pin_runs.log')
    pin = None
    try:
        with open(p, encoding='utf-8', errors='replace') as f:
            for line in f:
                if line.startswith('wwe2k14_%s ' % run):
                    m = re.search(r'sdk pin: (.*)$', line.rstrip())
                    pin = m.group(1) if m else line.strip()
    except OSError:
        pass
    return pin


@check('hard_fails')
def check_hard_fails(ctx):
    if not ctx.lines:
        return [Result('log', 'fail', 'no log wwe2k14_%s.log in %s' % (ctx.run, ctx.logs))]
    hits = []
    for line in ctx.lines:
        for key, pat in bench_report.HARD_FAILS:
            if pat.search(line):
                hits.append('%s: %s' % (key, bench_report.LOG_PREFIX.sub('', line.rstrip())[:200]))
    if hits:
        return [Result('hard_fails', 'fail', '%d hard-fail lines; first: %s' % (len(hits), hits[0]))]
    return [Result('hard_fails', 'pass', 'none (%s)' % ', '.join(k for k, _ in bench_report.HARD_FAILS))]


QUIT_LINE = re.compile(r'\]\[input\] line \d+(?: \([^)]*\))?: quit \(frame \d+\)')


@check('clean_exit')
def check_clean_exit(ctx):
    if ctx.runlist_line is not None:
        m = re.search(r'EXITED after ([\d.,]+)s, exit code (-?\d+)', ctx.runlist_line)
        if m and m.group(2) == '0':
            return [Result('clean_exit', 'pass', 'EXITED after %ss, exit code 0' % m.group(1))]
        return [Result('clean_exit', 'fail', 'run_list: %s' % ctx.runlist_line.strip()[:200])]
    # An old run: the script's own quit line, then the app's shutdown lines.
    quit_ok = any(QUIT_LINE.search(x) for x in ctx.lines)
    end_ok = any('Execution complete' in x or 'hard-exiting process' in x for x in ctx.lines[-50:])
    if quit_ok and end_ok:
        return [Result('clean_exit', 'pass', 'log: script quit + shutdown lines (no run_list line for an old run)')]
    return [Result('clean_exit', 'fail', 'log: script quit line %s, shutdown line %s' %
                   ('found' if quit_ok else 'MISSING', 'found' if end_ok else 'MISSING'))]


PASS_LINE = re.compile(r'\]\[input\] (?:line \d+(?: \([^)]*\))?: )?\S+: PASS\b')


@check('pass_line')
def check_pass_line(ctx):
    for x in ctx.lines:
        if PASS_LINE.search(x):
            return [Result('pass_line', 'pass', bench_report.LOG_PREFIX.sub('', x.rstrip())[:120])]
    return [Result('pass_line', 'fail', 'no "<tag>: PASS" input-script line in the log')]


def shot_path(captures, run, label):
    """Session 17: screenshots are PNG by default (src/input_script.h); older runs / --wwe2k14_screenshot_format=bmp
    give BMPs."""
    png = os.path.join(captures, '%s_%s.png' % (run, label))
    return png if os.path.exists(png) else os.path.join(captures, '%s_%s.bmp' % (run, label))


def bmp_problem(p):
    """None when p is a complete screenshot: a BMP with header 'BM' and bfSize == file size, or a PNG that ends with
    its IEND chunk (the game writes NAME.png.part and renames it when complete)."""
    if not os.path.exists(p):
        return 'missing'
    size = os.path.getsize(p)
    if p.lower().endswith('.png'):
        with open(p, 'rb') as f:
            head = f.read(8)
            f.seek(max(0, size - 12))
            tail = f.read(12)
        if head != b'\x89PNG\r\n\x1a\n':
            return 'not a PNG'
        return None if tail[4:8] == b'IEND' else "truncated PNG"

    if size < 54:
        return '%d bytes' % size
    with open(p, 'rb') as f:
        h = f.read(6)
    if h[:2] != b'BM':
        return 'not a BMP'
    want = int.from_bytes(h[2:6], 'little')
    if want != size:
        return 'truncated (%d of %d bytes)' % (size, want)
    return None


LAST_SHOT_QUIT_FRAMES = 60   # PLAYBOOK: a script must wait ~60 frames between its last screenshot and quit


@check('screenshots')
def check_screenshots(ctx):
    """Each shot: complete BMP (header size == file size) AND this run's log has its 'screenshot saved' line.
    The route's LAST shot lost to a quit < 60 frames later (no saved line) is a script defect, not a build
    regression: reported as 'warn' (shown on the summary line), not 'fail'."""
    shots = ctx.route.get('shots', [])
    bad, race = [], []
    for i, label in enumerate(shots):
        path = shot_path(ctx.captures, ctx.run, label)
        fname = os.path.basename(path)
        prob = bmp_problem(path)
        saved = any(fname in x and 'screenshot saved' in x for x in ctx.lines)
        if prob is None and saved:
            continue
        taken = [x for x in ctx.lines if "screenshot '%s' (frame" % label in x]
        fr = re.search(r'\(frame (\d+)\)', taken[-1]) if taken else None
        quit_gap = None
        if fr:
            for x in ctx.lines:
                q = QUIT_LINE.search(x)
                if q:
                    qf = int(re.search(r'\(frame (\d+)\)', x).group(1))
                    if qf >= int(fr.group(1)):
                        quit_gap = qf - int(fr.group(1))
        why = '%s: %s, %s, %s' % (label, prob or 'file ok', 'saved line found' if saved else 'no "screenshot saved" line',
                                  'never taken' if not taken else
                                  ('quit %d frames later' % quit_gap if quit_gap is not None else 'taken'))
        if (i == len(shots) - 1 and not saved and quit_gap is not None and quit_gap < LAST_SHOT_QUIT_FRAMES):
            race.append(why + ' (the async save lost the race with quit: the script needs wait_frames 60 first)')
        else:
            bad.append(why)
    n = len(shots)
    out = []
    if bad:
        out.append(Result('screenshots', 'fail', '%d/%d ok; %s' % (n - len(bad) - len(race), n, '; '.join(bad))))
    elif not race:
        out.append(Result('screenshots', 'pass', '%d/%d complete + saved (%s)' % (n, n, ','.join(shots))))
    if race:
        if not bad:
            out.append(Result('screenshots', 'pass', '%d/%d complete + saved' % (n - len(race), n)))
        out.append(Result('screenshots.last_race', 'warn', '; '.join(race)))
    return out


@check('scenes')
def check_scenes(ctx):
    b = ctx.bench
    missing = [s for s in SCENES if s not in b['scenes']]
    if missing:
        return [Result('scenes', 'fail', 'scene(s) not found: %s (anchors %s)' % (','.join(missing), b['anchors']))]
    return [Result('scenes', 'pass', 'anchors %s' % b['anchors'])]


def compare(metric, value, base, thr):
    """-> (status, text). No baseline value -> skip; a baseline value but no value in the run -> fail (e.g. the
    GPU timestamps or the timeline CSV broke)."""
    spec = METRICS[metric]
    if base is None:
        return 'skip', 'n/a (no baseline value)'
    if value is None or (isinstance(value, float) and math.isnan(value)):
        return 'fail', '%s: no value in this run, baseline has %s' % (metric, base)
    d = value - base
    bad = (spec['dir'] == 'low' and d < -thr) or (spec['dir'] == 'high' and d > thr) or \
          (spec['dir'] == 'both' and abs(d) > thr)
    f = spec['fmt']
    return ('fail' if bad else 'pass'), ('%s ' + f + ' vs base ' + f + ' (%+.2f, thr %.2f)') % (metric, value, base, d, thr)


@check('perf')
def check_perf(ctx):
    if not ctx.baseline or not ctx.baseline.get('scenes'):
        if SETTING_BASELINE:
            return [Result('perf', 'skip', 'setting the baseline from these runs')]
        return [Result('perf', 'fail', 'no baseline for this route, nothing to compare (set one with --set-baseline)')]
    out = []
    for s in SCENES:
        sc = ctx.bench['scenes'].get(s)
        bs = ctx.baseline['scenes'].get(s, {})
        if sc is None:
            out.append(Result('perf.%s' % s, 'fail', 'scene missing'))
            continue
        for metric in METRICS:
            bm = bs.get(metric)
            if not bm:
                continue
            st, txt = compare(metric, sc.get(metric), bm['base'], bm['thr'])
            out.append(Result('perf.%s.%s' % (s, metric), st, txt))
    return out


@check('bar60')
def check_bar60(ctx):
    """7b/7c's 60-fps bar on every scene: every full 600-frame window >= 59.5 fps, < 0.5% of frames > 20 ms.
    The bar's third part (wall time = real time) is the wall_s comparison in 'perf'.
    Reported as 'warn', not 'fail': it is an absolute target, not a threshold from A/A noise, and the 4K Brock
    entrance sits right at it on an unchanged build (0.43-0.46% in 4 runs, 0.92% in run 202). Regressions are
    caught by 'perf' (fps / frames >= 45 ms / GPU ms / wall time against A/A thresholds)."""
    out = []
    for s in SCENES:
        sc = ctx.bench['scenes'].get(s)
        if sc is None:
            out.append(Result('bar60.%s' % s, 'fail', 'scene missing'))
            continue
        mw = sc['min_full_window_fps']
        ok_w = mw is not None and mw >= BAR60['min_window_fps']
        ok_p = sc['pct20'] < BAR60['max_pct20']
        out.append(Result('bar60.%s' % s, 'pass' if ok_w and ok_p else 'warn',
                          'min full-window fps %s (bar %.1f), >20 ms %d = %.2f%% (bar < %.1f%%)' %
                          ('n/a' if mw is None else '%.2f' % mw, BAR60['min_window_fps'], sc['n20'], sc['pct20'],
                           BAR60['max_pct20'])))
    return out


# ------------------------------------------------------------------------------------------------ bench metrics
# Scene boundaries are fps60.py's exactly (Session 7b): Brock = 343.bik2 open .. second titantron open; second
# entrance = that open .. M3.PAC - 80; match = M3.PAC + 120 .. last frame - 30. Frame times from frames_NNN.csv
# (guest VdSwap intervals); GPU ms = gpu_ts submission durations between the scene's first and last VdSwap / frames.
FILE_FRAME = re.compile(r"\[file\] \S+ '([^']+)'.*frame=(\d+)")


def bench_metrics(logs, n, lines):
    anch = {}
    for line in lines:
        m = FILE_FRAME.search(line)
        if m:
            p = m.group(1).lower()
            for k in ('343.bik2', '103.bik2', '324.bik2', 'm3.pac'):
                if k in p and k not in anch:
                    anch[k] = int(m.group(2))
    frames = {}
    fpath = os.path.join(logs, 'frames_%s.csv' % n)
    if os.path.exists(fpath):
        with open(fpath, newline='') as f:
            f.readline()
            for line in f:
                p = line.strip().split(',')
                if len(p) != 3:
                    break
                try:
                    frames[int(p[0])] = float(p[2])
                except ValueError:
                    break
    vd, ts = [], []
    tpath = os.path.join(logs, 'timeline_%s.csv' % n)
    if os.path.exists(tpath):
        with open(tpath, newline='') as f:
            f.readline()
            for line in f:
                p = line.rstrip('\r\n').split(',')
                if len(p) < 6:
                    break
                try:
                    if p[2] == 'vdswap':
                        vd.append((int(p[0]), int(p[3], 16)))
                    elif p[2] == 'gpu_ts':
                        ts.append((int(p[0]), int(p[4], 16)))
                except ValueError:
                    break
    vd.sort()
    ts.sort()
    ts_t = [t for t, _ in ts]
    ts_cum = [0]
    for _, d in ts:
        ts_cum.append(ts_cum[-1] + d)

    def t_of(frame):
        for t, fr in vd:
            if fr >= frame:
                return t
        return None

    def gpu_ms(a, b):
        if not vd or not ts:
            return None
        t0, t1 = t_of(a), t_of(b)
        if t0 is None or t1 is None:
            return None
        i, j = bisect.bisect_left(ts_t, t0), bisect.bisect_left(ts_t, t1)
        return (ts_cum[j] - ts_cum[i]) / 1000 / max(1, b - a)

    def scene(a, b):
        sel = [(fr, frames[fr]) for fr in range(a, b) if fr in frames]
        ms = [m for _, m in sel]
        if not ms:
            return None
        wins = []
        W = BAR60['window']
        for k in range(0, len(sel), W):
            c = sel[k:k + W]
            cm = [m for _, m in c]
            wins.append(dict(frames=len(cm), fps=round(1000 * len(cm) / sum(cm), 2),
                             gpu_ms=_r(gpu_ms(c[0][0], c[-1][0] + 1))))
        full = [w['fps'] for w in wins if w['frames'] == W]
        return dict(start=a, end=b, frames=len(ms), wall_s=round(sum(ms) / 1000, 2),
                    fps=round(1000 * len(ms) / sum(ms), 3),
                    n20=sum(m > 20 for m in ms), pct20=round(100 * sum(m > 20 for m in ms) / len(ms), 3),
                    n25=sum(m > 25 for m in ms), n35=sum(m > 35 for m in ms), n45=sum(m >= 45 for m in ms),
                    max_ms=round(max(ms), 1), gpu_ms=_r(gpu_ms(a, b)),
                    min_full_window_fps=min(full) if full else None, windows=wins)

    scenes = {}
    second = '103.bik2' if '103.bik2' in anch else '324.bik2'
    if '343.bik2' in anch and second in anch:
        scenes['brock'] = scene(anch['343.bik2'], anch[second])
    if second in anch and 'm3.pac' in anch:
        scenes['second'] = scene(anch[second], anch['m3.pac'] - 80)
    if 'm3.pac' in anch and frames:
        scenes['match'] = scene(anch['m3.pac'] + 120, max(frames) - 30)
    scenes = {k: v for k, v in scenes.items() if v}
    return dict(anchors=anch, second=second.split('.')[0], scenes=scenes, has_gpu_ts=bool(ts),
                total_frames=len(frames))


def _r(x, nd=3):
    return None if x is None else round(x, nd)


# --------------------------------------------------------------------------------------------------- launching
def ps_quote(s):
    return "'" + s.replace("'", "''") + "'"


def script_arg(script, exe):
    """Exe-relative path for the default exe (as every reference run logged it), absolute for another exe."""
    if os.path.normcase(os.path.abspath(exe)) == os.path.normcase(DEFAULT_EXE):
        return '..\\..\\..\\tools\\input_scripts\\' + script
    return os.path.join(SCRIPTS_DIR, script)


def route_extra(route, extra_args):
    return ' '.join(x for x in (route['extra'], extra_args or '') if x)


def run_list_command(route, exe, extra_args):
    # Session 16: run_list fast-forwards (--vsync=false) by default; every regress route STARTS at normal speed
    # (-RealTime): the timed ones measure speed. Session 29: selftest / logo then switch speed themselves (`cvar vsync`:
    # fast-forwarded where states drive them, normal-speed screenshot windows; references rebuilt from runs 2173-2182).
    # Session 30: routes that measure speed keep the focused fullscreen window their baselines were measured with
    # (-Foreground); the others open behind other windows without the focus.
    return '& %s -Scripts @(%s) -Extra %s -Exe %s -WaitSeconds %d -RealTime%s' % (
        ps_quote(RUN_LIST), ps_quote(script_arg(route['script'], exe)), ps_quote(route_extra(route, extra_args)),
        ps_quote(exe), route.get('timeout', 720), foreground_arg(route))


def foreground_arg(route):
    return ' -Foreground' if set(route['checks']) & {'perf', 'bar60'} else ''


RUNLIST_LINE = re.compile(r'^(\S+\.txt) -> (?:wwe2k14_(\d+))?')
# run_list.ps1 (task C) ends a failed run's line with "  [triage] CLASS (confidence C), culprit X, saved PATH".
RUNLIST_TRIAGE = re.compile(r'\s\[triage\] (.*)$')
TRIAGE_PY = os.path.join(REPO, 'tools', 'triage', 'triage.py')
TRIAGE_OUT = os.path.join(REPO, 'out', 'harness', 'triage')


TRIAGE_TIMEOUT = 300  # s; longer than triage.py's own limits (event-log query 120 s, after which it goes on without it)


def triage_save_path(num, exe):
    """Where run NNN's triage verdict for this exe is kept; run_list.ps1 Get-TriageSavePath uses the same rule.

    The default build keeps triage.py --save's own name, run_NNN.json. Any other exe folder (autobisect snapshots,
    sdk_rebase scratch exes, the coverage build: their logs number from 001 again) gets <tag>_run_NNN.json, tag = that
    folder relative to the repo with every run of non-word characters as '_', lower case."""
    exe_dir = os.path.dirname(os.path.abspath(exe))
    name = 'run_%03d.json' % num
    if os.path.normcase(exe_dir) != os.path.normcase(os.path.dirname(DEFAULT_EXE)):
        try:
            rel = os.path.relpath(exe_dir, REPO)
        except ValueError:  # another drive
            rel = exe_dir
        name = re.sub(r'\W+', '_', rel).strip('_').lower() + '_' + name
    return os.path.join(TRIAGE_OUT, name)


def saved_verdict_problem(v, saved, num, exe):
    """None when the verdict saved in `saved` may be reused for run NNN of this exe, else why not."""
    logs = os.path.normcase(os.path.join(os.path.dirname(os.path.abspath(exe)), 'logs'))
    vlog = v.get('log') if isinstance(v, dict) else None
    if not vlog or v.get('run') != num or os.path.normcase(os.path.dirname(os.path.abspath(vlog))) != logs:
        return 'it is for another exe folder or run'
    if v.get('class') == 'in_progress':
        return 'it was taken while the run was still going'
    try:
        if os.path.getmtime(vlog) > os.path.getmtime(saved) + 1:
            return 'the run log was written after it'
    except OSError:  # log gone: the saved verdict is all there is
        pass
    return None


def triage_verdict(run, rl_line, exe):
    """One-line crash-triage verdict for a FAILED route run (never raises, never contains '->').

    run_list's own [triage] part when its line has one (triaged right after the run, before any relink). Otherwise
    (a check failed on a clean run, or --runs on an old run): the verdict saved earlier at triage_save_path() is
    reused when it is for this exe's logs folder and this run, was not taken mid-run (in_progress) and is newer than
    the run log (re-triaging after a relink would overwrite resolved host offsets with "map stale"); else
    py tools\\triage\\triage.py NNN --json now, saved there unless the verdict is in_progress."""
    m = RUNLIST_TRIAGE.search(rl_line or '')
    if m:
        return m.group(1).strip().replace('->', '>')
    if run is None:
        return 'not run: no run number'
    try:
        num = int(run)
        saved = triage_save_path(num, exe)
        why_not = None
        if os.path.exists(saved):
            try:
                with open(saved, encoding='utf-8') as f:
                    old = json.load(f)
            except ValueError as e:
                old, why_not = None, 'it is not valid JSON (%s)' % e
            if old is not None:
                why_not = saved_verdict_problem(old, saved, num, exe)
                if why_not is None:
                    return ('%s (confidence %s), culprit %s, saved earlier %s' % (
                        old.get('class'), old.get('confidence'), (old.get('culprit') or 'unknown')[:120],
                        saved)).replace('->', '>')
        p = subprocess.Popen([sys.executable, TRIAGE_PY, str(num), '--json', '--exe-dir',
                              os.path.dirname(os.path.abspath(exe))], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True, errors='replace', cwd=REPO)
        try:
            so, se = p.communicate(timeout=TRIAGE_TIMEOUT)
        except subprocess.TimeoutExpired:  # stop triage and its event-log pwsh child, not just python.exe
            subprocess.run(['taskkill', '/PID', str(p.pid), '/T', '/F'], capture_output=True)
            try:
                p.communicate(timeout=15)
            except subprocess.TimeoutExpired:
                pass
            return 'triage timed out after %d s' % TRIAGE_TIMEOUT
        try:
            v = json.loads(so or '')
        except ValueError:
            v = None
        if not isinstance(v, dict) or not v.get('class'):
            tail = [x for x in ((se or '') + '\n' + (so or '')).splitlines() if x.strip()]
            return ('triage error (exit %s): %s' % (p.returncode, (tail[-1].strip() if tail else 'no output')[:160])
                    ).replace('->', '>')
        if v['class'] == 'in_progress':
            where = 'NOT saved (run still going; triage it again once it has ended)'
        else:
            try:
                os.makedirs(os.path.dirname(saved), exist_ok=True)
                with open(saved, 'w', encoding='utf-8') as f:
                    f.write(so)
                where = 'saved %s' % saved
            except OSError as e:
                where = 'NOT saved (%s)' % e
        if why_not:
            where += ' (the verdict saved there before was not reused: %s)' % why_not
        return ('%s (confidence %s), culprit %s, %s' % (v['class'], v.get('confidence'),
                                                        (v.get('culprit') or 'unknown')[:120], where)
                ).replace('->', '>')
    except Exception as e:  # noqa: BLE001  (triage must never break the regression set)
        return ('triage error: %s: %s' % (type(e).__name__, e)).replace('->', '>')


# ------------------------------------------------------------------------- task L1: parallel functional part
# --parallel N: routes that need no exclusive lane (needs_exclusive: no timing check, no fixed seed) run side by side
# in shared run-lock slots (run_list.ps1 -Parallel N); the others stay exclusive. 1 (--no-parallel) = every route
# exclusive, one after the other (the behaviour before task L1). Default 3 since task L1's verification: with pause
# exclusive, a full --parallel pass gave the same 69 check verdicts as a --no-parallel one (448-452 vs 435-439) and
# every parallel selftest / logo run matched its exclusive runs (docs/harness/regress.md "Parallel functional part").
PARALLEL_DEFAULT = 3
PHASE1_ENV = 'WWE2K14_REGRESS_PHASE1'   # the parallel part's results, for this script re-run inside the lock
TIMED_CHECKS = ('perf', 'bar60', 'scenes')


def is_timed(route):
    """Timed routes (perf numbers): lesnar, taker4k."""
    return bool(route.get('timed')) or any(c in route['checks'] for c in TIMED_CHECKS)


def needs_exclusive(route):
    """Routes that never run next to another game: the timed ones, any route marked exclusive=True, and routes that
    compare a deterministic game state, i.e. run with a fixed seed (the pause route compares a paused frame of a
    CPU vs CPU match). The match's determinism rests on thread races (docs/systems/determinism.md), and next to other
    games it did not hold (task L1; overlaps from the start/end times of every wwe2k14 log under out\\, all exe
    folders): of the 5 default-flag pause runs that booted while another game ran, 2 failed their references (442,
    booted with selftest 440 and logo 441: the match diverged by match frame 600, b_match600 MAD 32.5 against 425 /
    430 / 437, which are pixel-identical to each other; 445, booted while task L3's lesnar runs 010 / 011 played
    their matches, then 446 / 447 joined: pause shot MAD 0.070 > 0.05) and 3 passed (430, booted next to L3's 4K
    taker run 002; 446; 447). All 9 that booted with no other game running passed (309, 310, 314, 319, 324, 425,
    437, 450, 457)."""
    return is_timed(route) or bool(route.get('exclusive')) or 'wwe2k14_fixed_seed' in (route.get('extra') or '')


def build_fingerprint(exe):
    """sha256 over the exe and the rex*.dll staged next to it: what a build or an SDK restage changes."""
    h = hashlib.sha256()
    d = os.path.dirname(os.path.abspath(exe))
    for p in [exe] + sorted(glob.glob(os.path.join(d, 'rex*.dll'))):
        h.update(os.path.basename(p).lower().encode() + b'\0')
        try:
            with open(p, 'rb') as f:
                for chunk in iter(lambda: f.read(1 << 20), b''):
                    h.update(chunk)
        except OSError as e:
            h.update(('unreadable: %s' % e).encode())
    return h.hexdigest()


def run_list_parallel_command(rnames, exe, extra_args, n):
    """One run_list call for several routes: each route's own flags via -ScriptExtras (placed before -Extra, so the
    game's command line is the same as route_extra() gives one route), --extra-args as -Extra."""
    return '& %s -Scripts @(%s) -ScriptExtras @(%s) -Extra %s -Exe %s -WaitSeconds %d -Parallel %d -RealTime' % (
        ps_quote(RUN_LIST), ', '.join(ps_quote(script_arg(ROUTES[r]['script'], exe)) for r in rnames),
        ', '.join(ps_quote(ROUTES[r]['extra']) for r in rnames), ps_quote(extra_args or ''), ps_quote(exe),
        max(ROUTES[r].get('timeout', 720) for r in rnames), n)


def launch_parallel(rnames, exe, extra_args, n, log):
    """-> {route: (run number or None, run_list line)} from one run_list.ps1 -Parallel N call."""
    cmd = run_list_parallel_command(rnames, exe, extra_args, n)
    log('$ pwsh -NoProfile -Command "%s"' % cmd)
    p = subprocess.run(['pwsh', '-NoProfile', '-Command', cmd], capture_output=True, text=True, errors='replace')
    out = (p.stdout or '') + (p.stderr or '')
    for line in out.splitlines():
        log('  | ' + line)
    got = {}
    for line in out.splitlines():
        m = RUNLIST_LINE.match(line.strip())
        if m:
            for r in rnames:
                if m.group(1) == ROUTES[r]['script'] and r not in got:
                    got[r] = (m.group(2), line.strip())
    tail = out.strip().splitlines()[-1] if out.strip() else 'no output from run_list.ps1'
    return {r: got.get(r, (None, tail)) for r in rnames}


def run_phase1(func, exe, extra_args, n, argv, t_start):
    """The parallel part (no run-lock hold of our own: run_list's workers take shared slots)."""
    log = ['', '=== parallel part (task L1): %s through run_list.ps1 -Parallel %d (shared slots)' % (','.join(func), n)]
    for rn in func:
        print('%-9s running %s ... (parallel part: up to %d at once, shared slots)' % (rn, ROUTES[rn]['script'], n),
              flush=True)
    fp_before = build_fingerprint(exe)
    t0 = time.time()
    res = launch_parallel(func, exe, extra_args, n, log.append)
    wall = time.time() - t0
    fp_after = build_fingerprint(exe)
    log.append('parallel part: %.1f s wall; build fingerprint before %s, after %s' % (wall, fp_before[:16], fp_after[:16]))
    print('%-9s parallel part done in %.0f s' % ('', wall), flush=True)
    return dict(argv=argv, n=n, routes={r: list(v) for r, v in res.items()}, fp_before=fp_before, fp_after=fp_after,
                wall_s=round(wall, 1), t_start=t_start, log=log)


def exe_shared_safe(exe):
    """True when the exe logs "run log number" (built with the per-run log number), i.e. it may run in a shared
    run-lock slot. The same test as tools\\runlock.ps1 Test-RunLockExeSharedSafe (the format string in the binary);
    run_list.ps1 runs any other exe exclusive, one script at a time, even with -Parallel (fixer, review finding 4a)."""
    needle = b'run log number: {'
    carry = b''
    try:
        with open(exe, 'rb') as f:
            for chunk in iter(lambda: f.read(8 << 20), b''):
                if needle in carry + chunk:
                    return True
                carry = chunk[-(len(needle) - 1):]
    except OSError:
        return False
    return False


def parent_stamp():
    """This process's identity for a hand-off to its re-run inside the run lock: pid + start time (runlock.py's unit),
    so the re-run can check that the process that wrote the hand-off is the one still waiting on it."""
    import runlock
    return dict(pid=os.getpid(), pid_start=runlock._proc_start_ticks(os.getpid()) or 0)


def parent_waiting(st):
    """The hand-off's writer (parent_stamp) is still alive (it waits in ensure_lock while its re-run works)."""
    import runlock
    par = st.get('parent') or {}
    return bool(par.get('pid')) and runlock._owner_alive(par)


# Fixer (review finding 2): this call's start time, handed to its re-run inside the run lock, so "wall time" / wall_s
# always counts from the start of the call (queue waits included), whichever path took the lock.
T0_ENV = 'WWE2K14_REGRESS_T0'


def hand_off_t0(t_start, argv):
    os.environ[T0_ENV] = json.dumps(dict(t=t_start, argv=argv, parent=parent_stamp()))


def take_t0(argv):
    """The parent call's start time when this process is its re-run inside the run lock (else None). The variable is
    removed, so nothing this process starts sees it; it is only used when its writer still waits on this re-run and
    the command line is the same."""
    raw = os.environ.pop(T0_ENV, None)
    if not raw:
        return None
    try:
        st = json.loads(raw)
        if st.get('argv') == argv and parent_waiting(st):
            return float(st['t'])
    except (ValueError, TypeError, KeyError, AttributeError):
        pass
    return None


def save_phase1(st):
    path = os.path.join(OUT, 'phase1_%s_%d.json' % (datetime.datetime.now().strftime('%Y%m%d_%H%M%S_%f'), os.getpid()))
    st = dict(st, parent=parent_stamp())
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(st, f, indent=1)
    return path


def take_phase1(argv):
    """The parallel part's results when this process is the re-run inside the lock (else None). The variable is
    removed, so nothing this process starts sees it. Fixer (review finding 4d): the file is only used when the process
    that wrote it still waits on this re-run (pid + start time) and the command line is the same, and it is deleted
    once taken (its content goes into this call's JSON as 'parallel'), so it can never be reused later."""
    path = os.environ.pop(PHASE1_ENV, None)
    if not path:
        return None
    try:
        with open(path, encoding='utf-8') as f:
            st = json.load(f)
    except (OSError, ValueError) as e:
        print('WARNING: %s=%s is unreadable (%s): every route runs here' % (PHASE1_ENV, path, e), flush=True)
        return None
    if st.get('argv') != argv:
        print('WARNING: %s=%s is for another command line (%s): every route runs here' % (PHASE1_ENV, path,
                                                                                         st.get('argv')), flush=True)
        return None
    if not parent_waiting(st):
        print('WARNING: %s=%s was written by a process that is no longer waiting on this one (pid %s): every route '
              'runs here' % (PHASE1_ENV, path, (st.get('parent') or {}).get('pid')), flush=True)
        return None
    try:
        os.remove(path)
    except OSError:
        pass
    st['from_parent'] = path
    return st


def launch(route, exe, extra_args, log):
    cmd = run_list_command(route, exe, extra_args)
    log('$ pwsh -NoProfile -Command "%s"' % cmd)
    p = subprocess.run(['pwsh', '-NoProfile', '-Command', cmd], capture_output=True, text=True, errors='replace')
    out = (p.stdout or '') + (p.stderr or '')
    for line in out.splitlines():
        log('  | ' + line)
    for line in out.splitlines():
        m = RUNLIST_LINE.match(line.strip())
        if m and m.group(1) == route['script']:
            return (m.group(2), line.strip())
    return (None, out.strip().splitlines()[-1] if out.strip() else 'no output from run_list.ps1')


# ---------------------------------------------------------------------------------------------------- baseline
class BaselineError(Exception):
    pass


def load_baseline(path):
    """None when the file does not exist; BaselineError when it exists but is unreadable / not a baseline."""
    if not os.path.exists(path):
        return None
    try:
        with open(path, encoding='utf-8') as f:
            b = json.load(f)
    except (OSError, ValueError) as e:
        raise BaselineError('baseline %s is unreadable: %s' % (path, e))
    if not isinstance(b, dict) or not isinstance(b.get('routes'), dict):
        raise BaselineError('baseline %s has no "routes" table' % path)
    return b


def make_baseline(results, exe_info, warn):
    """results: {route: [ {run, bench, status, identity} ... ]} -> baseline dict. A route with no usable run is
    left out (the caller keeps its old entry), so a --set-baseline on bad runs cannot switch perf checks off."""
    out = dict(created=datetime.datetime.now().isoformat(timespec='seconds'), exe=exe_info,
               rule='thr = max(%.0f x (max - min over the baseline runs), floor); base = median' % SPREAD_K,
               floors={'fps': '0.25 fps', 'n45': '3 frames', 'gpu_ms': 'max(0.5 ms, 10% of base)',
                       'wall_s': 'max(0.5 s, 1% of base)'},
               routes={})
    all_ok = []
    for rname, runs in results.items():
        # Only runs that passed every check feed the numbers (a crashed or cut-short run is not noise).
        ok = [x for x in runs if x['status'] == 'PASS']
        all_ok.extend(ok)
        if not ok:
            warn('WARNING: %s: no run passed (%s); its old baseline entry is kept' %
                 (rname, ','.join(x['run'] or '-' for x in runs)))
            continue
        r = dict(runs=[x['run'] for x in ok], left_out=[x['run'] for x in runs if x['status'] != 'PASS'],
                 identity={x['run']: x.get('identity') for x in ok})
        # Runs before task 1 have no pin line (None): only two different known pins are worth a note.
        pins = sorted(set((x.get('identity') or {}).get('sdk_pin') for x in ok) - {None})
        if len(pins) > 1:
            warn('NOTE: %s baseline runs have different SDK pin lines: %s' % (rname, ' | '.join(pins)))
        bench_runs = [x for x in ok if x.get('bench') and x['bench'].get('scenes')]
        if 'perf' in ROUTES[rname]['checks'] and not bench_runs:
            warn('WARNING: %s: no passing run has scene numbers; its old baseline entry is kept' % rname)
            continue
        if bench_runs:
            r['scenes'] = {}
            for s in SCENES:
                r['scenes'][s] = {}
                for metric, spec in METRICS.items():
                    vals = [x['bench']['scenes'][s][metric] for x in bench_runs
                            if s in x['bench']['scenes'] and x['bench']['scenes'][s].get(metric) is not None]
                    if not vals:
                        continue
                    base = statistics.median(vals)
                    spread = max(vals) - min(vals)
                    thr = max(SPREAD_K * spread, spec['floor'](base))
                    r['scenes'][s][metric] = dict(base=round(base, 3), spread=round(spread, 3), thr=round(thr, 3),
                                                  n=len(vals), values=vals,
                                                  runs=[x['run'] for x in bench_runs
                                                        if s in x['bench']['scenes'] and
                                                        x['bench']['scenes'][s].get(metric) is not None])
                    if len(vals) < 2:
                        warn('WARNING: %s %s %s has only %d run: spread 0, threshold = floor only' %
                             (rname, s, metric, len(vals)))
        out['routes'][rname] = r
    # The exe on disk is only the baseline's exe if every baseline run started after it was built.
    older = [x['run'] for x in all_ok if (x.get('identity') or {}).get('older_than_exe')]
    if older:
        out['exe'] = dict(exe_info, sha256=None,
                          note='runs %s started before this exe was built: the build of the numbers is unknown'
                               % ','.join(older))
        warn('WARNING: runs %s started before the exe on disk was built, so the baseline cannot name its build '
             '(every later check prints the "another exe build" NOTE)' % ','.join(older))
    return out


# ------------------------------------------------------------------------------------------------------- misc
def exe_info(exe):
    info = dict(path=exe)
    try:
        st = os.stat(exe)
        info['size'] = st.st_size
        info['mtime'] = datetime.datetime.fromtimestamp(st.st_mtime).isoformat(timespec='seconds')
        h = hashlib.sha256()
        with open(exe, 'rb') as f:
            for chunk in iter(lambda: f.read(1 << 20), b''):
                h.update(chunk)
        info['sha256'] = h.hexdigest()
    except OSError as e:
        info['error'] = str(e)
    return info


def load_plugins(log):
    for path in sorted(glob.glob(os.path.join(HERE, 'checks_*.py'))):
        name = os.path.splitext(os.path.basename(path))[0]
        spec = importlib.util.spec_from_file_location('regress_' + name, path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        if hasattr(mod, 'register'):
            api = argparse.Namespace(check=check, routes=ROUTES, Result=Result, compare=compare, REPO=REPO, OUT=OUT,
                                     SCENES=SCENES)
            mod.register(api)
            log('plugin %s registered' % name)


def summary_line(rname, run, status, results, bench, base, rl_line=None):
    parts = []
    m = re.search(r'EXITED after ([\d.,]+)s', rl_line or '')
    if m:
        parts.append('%ss' % m.group(1).split('.')[0])
    if bench and bench['scenes']:
        for s in SCENES:
            sc = bench['scenes'].get(s)
            if not sc:
                continue
            bs = (base or {}).get('scenes', {}).get(s, {})
            fps_b = bs.get('fps', {}).get('base')
            g = '' if sc['gpu_ms'] is None else ' gpu %.1f' % sc['gpu_ms']
            parts.append('%s %.2ffps%s 45ms:%d%s' % (SCENE_LABEL[s], sc['fps'], '' if fps_b is None else
                                                    '(%.2f)' % fps_b, sc['n45'], g))
    else:
        for r in results:
            if r.name in ('screenshots', 'pass_line') and r.status == 'pass':
                parts.append('PASS line' if r.name == 'pass_line' else 'shots ' + r.detail.split(' ')[0])
    fails = [r for r in results if r.status == 'fail']
    warns = [r for r in results if r.status == 'warn']
    skips = [r for r in results if r.status == 'skip']
    if skips:
        parts.append('skipped %d (%s)' % (len(skips), ','.join(r.name for r in skips[:4])))
    head = '%-9s %-4s run %s' % (rname, status, run or '---')
    tail = ''
    if fails:
        tail = '  FAILED: ' + '; '.join('%s (%s)' % (r.name, r.detail[:150]) for r in fails[:3])
        if len(fails) > 3:
            tail += '; +%d more' % (len(fails) - 3)
    if warns:
        tail += '  WARN: ' + '; '.join('%s (%s)' % (r.name, r.detail[:150]) for r in warns[:2])
        if len(warns) > 2:
            tail += '; +%d more' % (len(warns) - 2)
    return head + '  ' + ' | '.join(parts) + tail


def main():
    ap = argparse.ArgumentParser(description='Regression set: short pass/fail per route (docs/harness/regress.md)')
    ap.add_argument('--only', help='comma-separated routes (%s)' % ','.join(ROUTES))
    ap.add_argument('--extra-args', default='', help='flags added to every route, ONE string: --extra-args="--x=1"')
    ap.add_argument('--exe', default=DEFAULT_EXE, help='another build\'s wwe2k14.exe (logs/captures next to it)')
    ap.add_argument('--dry-run', action='store_true', help='print the launch plan and baseline status only')
    ap.add_argument('--runs', nargs='+', metavar='ROUTE=N[,N...]',
                    help='check existing runs instead of launching (several per route with --set-baseline)')
    ap.add_argument('--set-baseline', action='store_true', help='write the baseline from the runs of this call')
    ap.add_argument('--baseline', default=BASELINE, help='baseline file (default out\\harness\\regress\\baseline.json)')
    ap.add_argument('--list', action='store_true', help='routes and their checks')
    ap.add_argument('--show-baseline', action='store_true', help='print the baseline as a table (runs, spread, thr)')
    ap.add_argument('--parallel', type=int, nargs='?', const=3, default=PARALLEL_DEFAULT, choices=(1, 2, 3),
                    metavar='N', help='functional routes (no perf checks, no fixed seed: selftest, logo) first, N at a time in '
                    'shared run-lock slots (bare --parallel = 3), then the others (lesnar, taker4k, pause) exclusive '
                    'one after the other; 1 = every route '
                    'exclusive (default %d)' % PARALLEL_DEFAULT)
    ap.add_argument('--no-parallel', dest='parallel', action='store_const', const=1,
                    help='every route exclusive, one after the other (= --parallel 1)')
    args = ap.parse_args()

    os.makedirs(OUT, exist_ok=True)
    plugin_log = []
    load_plugins(plugin_log.append)

    if args.list:
        for n, r in ROUTES.items():
            print('%-9s %-28s extra %-55s checks %s' % (n, r['script'], repr(r['extra']), ','.join(r['checks'])))
        return 0

    if args.show_baseline:
        try:
            b = load_baseline(args.baseline)
        except BaselineError as e:
            print('ERROR: %s' % e)
            return 2
        if not b:
            print('no baseline at %s' % args.baseline)
            return 1
        print('baseline %s, created %s; %s' % (args.baseline, b.get('created'), b.get('rule')))
        print('| Route | Scene | Metric | Runs | Values | Base (median) | Spread | Threshold |')
        print('|---|---|---|---|---|---|---|---|')
        for rn, r in b.get('routes', {}).items():
            if not r.get('scenes'):
                print('| %s | - | pass/fail only | %s | | | | |' % (rn, ', '.join(r.get('runs', []))))
                continue
            for s, ms in r['scenes'].items():
                for m, v in ms.items():
                    print('| %s | %s | %s | %s | %s | %s | %s | %s |' % (
                        rn, s, m, ', '.join(v['runs']), ' / '.join(str(x) for x in v['values']), v['base'],
                        v['spread'], v['thr']))
        return 0

    routes = list(ROUTES)
    if args.only:
        routes = [x.strip() for x in args.only.split(',') if x.strip()]
        bad = [x for x in routes if x not in ROUTES]
        if bad:
            ap.error('unknown route(s): %s (have %s)' % (','.join(bad), ','.join(ROUTES)))

    given = None
    if args.runs:
        given = collections.OrderedDict()
        for spec in args.runs:
            if '=' not in spec:
                ap.error('--runs wants ROUTE=N[,N...], got %r' % spec)
            k, v = spec.split('=', 1)
            if k not in ROUTES:
                ap.error('unknown route %r' % k)
            given[k] = [bench_report.run_id(x) for x in v.split(',') if x.strip()]
        routes = [r for r in routes if r in given]

    exe = os.path.abspath(args.exe)
    logs = os.path.join(os.path.dirname(exe), 'logs')
    captures = os.path.join(os.path.dirname(exe), 'captures')
    try:  # an unreadable baseline stops here (exit 2), before any launch or wait in the lock queue
        stored_baseline = load_baseline(args.baseline)
    except BaselineError as e:
        print('ERROR: %s (fix or delete it, or pass --baseline)' % e)
        return 2
    global SETTING_BASELINE
    SETTING_BASELINE = args.set_baseline
    baseline = None if args.set_baseline else stored_baseline
    if baseline:
        base_status = '%s (created %s)' % (args.baseline, baseline.get('created'))
    elif args.set_baseline:
        base_status = 'setting %s from this call\'s runs' % args.baseline
    else:
        base_status = 'NONE at %s: every perf check FAILS (set one with --set-baseline)' % args.baseline

    func = [r for r in routes if not needs_exclusive(ROUTES[r])]   # task L1: may run side by side (--parallel)
    timed = [r for r in routes if needs_exclusive(ROUTES[r])]      # always exclusive, one after the other
    use_parallel = not given and args.parallel > 1 and len(func) >= 2
    no_shared_note = None
    if use_parallel and not exe_shared_safe(exe):
        # run_list.ps1 would run such an exe exclusive, one script at a time, in a hold of its own: then regress
        # would take two exclusive holds. Run every route in regress's one hold instead (as --no-parallel).
        use_parallel = False
        no_shared_note = ('NOTE: --parallel %d: %s does not log "run log number" (built before the per-run log '
                          'number), so it cannot run in a shared slot: every route runs exclusive, one after the '
                          'other' % (args.parallel, exe))

    if args.dry_run:
        print('baseline: %s' % base_status)
        if no_shared_note:
            print(no_shared_note)
        if use_parallel:
            print('%-9s %s: pwsh -NoProfile -Command "%s"' % ('parallel', ','.join(func), run_list_parallel_command(
                func, exe, args.extra_args, args.parallel)))
        for rn in routes:
            if given:
                print('%-9s check runs %s (no launch)' % (rn, ','.join(given[rn])))
            elif not (use_parallel and rn in func):
                print('%-9s pwsh -NoProfile -Command "%s"' % (rn, run_list_command(ROUTES[rn], exe, args.extra_args)))
        print('checks: ' + '; '.join('%s=%s' % (rn, ','.join(ROUTES[rn]['checks'])) for rn in routes))
        return 0

    phase1 = None   # task L1: the functional routes' runs from the parallel part (this process's, or its parent's)
    t_start = time.time()   # the call's start (replaced below by the parent's when this process is its re-run)
    t_self = t_start        # this process's start
    in_hold = False         # this process ran inside an exclusive hold (its own re-run, or a caller's)
    if not given:
        sys.argv[0] = os.path.abspath(sys.argv[0])
        from runlock import ensure_lock, held_by_us
        t_call = take_t0(sys.argv[1:])   # set by this call's first process just before it queued for the lock
        if t_call:
            t_start = t_call
        phase1 = take_phase1(sys.argv[1:])   # this process is the re-run inside the lock after the parallel part
        if phase1 and held_by_us() != 'exclusive':   # only the re-run inside the exclusive hold may reuse them
            print('WARNING: %s is set but this process does not hold the exclusive run lock: ignored' % PHASE1_ENV,
                  flush=True)
            phase1 = None
        if no_shared_note and not t_call:   # once per call (not again in the re-run inside the lock)
            print(no_shared_note, flush=True)
        if phase1 is None and use_parallel:
            held = held_by_us()
            if held == 'exclusive':
                print('NOTE: --parallel %d: this call runs inside an exclusive run-lock hold, so every route runs in '
                      'it, one at a time' % args.parallel, flush=True)
            elif held:
                pass   # a shared hold: ensure_lock below stops with its error (regress needs the exclusive lock)
            else:
                phase1 = run_phase1(func, exe, args.extra_args, args.parallel, sys.argv[1:], t_start)
                if timed or phase1['fp_before'] != phase1['fp_after']:
                    # The timed routes (or a re-run after a build landed) need the exclusive hold: re-run this script
                    # inside it; the re-run reuses the parallel part's runs (or sets them aside if the build changed).
                    os.environ[PHASE1_ENV] = save_phase1(phase1)
                    hand_off_t0(t_start, sys.argv[1:])
                    if timed:
                        print('%-9s exclusive routes %s next: waiting for the EXCLUSIVE run lock ...' % (
                            '', ','.join(timed)), flush=True)
                    ensure_lock('regress ' + ','.join(timed or func))   # exits with the re-run's exit code
        if phase1 is None or phase1.get('from_parent'):
            # One hold of the run lock for the whole set (re-runs this script under runlock.ps1 unless already held).
            if held_by_us() != 'exclusive':
                hand_off_t0(t_start, sys.argv[1:])   # so the re-run's wall time counts from this call's start
            ensure_lock('regress ' + ','.join(routes))
            in_hold = held_by_us() == 'exclusive'
        if phase1 and phase1.get('from_parent'):
            t_start = t_call or phase1.get('t_start') or t_start
            fp_now = build_fingerprint(exe)
            if not (phase1['fp_before'] == phase1['fp_after'] == fp_now):
                note = ('NOTE: the build changed during the parallel part or before this exclusive hold (fingerprint '
                        '%s -> %s -> %s now: a build or SDK restage landed in between): its runs %s are set aside and '
                        'the functional routes run again here, exclusive, on this build' % (
                            phase1['fp_before'][:12], phase1['fp_after'][:12], fp_now[:12],
                            ','.join(str(v[0]) for v in phase1['routes'].values())))
                print(note, flush=True)
                phase1['log'].append(note)
                phase1['set_aside'] = phase1['routes']
                phase1['routes'] = {}

    # Unique per call (two agents checking runs in the same second must not overwrite each other's details).
    stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M%S_%f')[:-3] + '_%d' % os.getpid()
    detail_path = os.path.join(OUT, 'regress_%s.log' % stamp)
    json_path = os.path.join(OUT, 'regress_%s.json' % stamp)
    dlog = open(detail_path, 'x', encoding='utf-8')

    def log(s=''):
        dlog.write(s + '\n')
        dlog.flush()

    def warn(s):
        log(s)
        print(s, flush=True)

    log('regress %s  argv %s' % (stamp, sys.argv[1:]))
    for x in plugin_log:
        log(x)
    einfo = exe_info(exe)
    try:
        exe_mtime = datetime.datetime.fromtimestamp(os.path.getmtime(exe))
    except OSError:
        exe_mtime = None
    log('exe %s' % json.dumps(einfo))
    print('baseline: %s' % base_status, flush=True)
    log('baseline: %s' % base_status)
    base_pins = set()
    if baseline:
        bsha = (baseline.get('exe') or {}).get('sha256')
        log('baseline exe sha256 %s' % (bsha or 'unknown'))
        if bsha != einfo.get('sha256'):
            warn('NOTE: the baseline was taken with another or an unknown exe build (sha256 %s, this one %s...)' % (
                (bsha or 'unknown')[:12] + ('...' if bsha else ''), einfo.get('sha256', '?')[:12]))
        for r in baseline.get('routes', {}).values():
            for idn in (r.get('identity') or {}).values():
                if idn and idn.get('sdk_pin'):
                    base_pins.add(idn['sdk_pin'])
    if args.extra_args:
        log('extra args for every route: %s' % args.extra_args)
    if phase1:   # task L1: the parallel part ran before this details log existed
        for x in phase1.get('log', []):
            log(x)
        if phase1.get('from_parent'):
            log('parallel part results from %s' % phase1['from_parent'])
        log('')
        log('=== exclusive part: %s' % (','.join(r for r in routes if r not in phase1['routes']) or 'none'))

    all_results = collections.OrderedDict()
    lines_out = []
    overall = True
    for rn in routes:
        route = ROUTES[rn]
        rbase = (baseline or {}).get('routes', {}).get(rn)
        todo = [(n, None) for n in given[rn]] if given else None
        mode = None if given else 'exclusive'
        if todo is None and phase1 and phase1['routes'].get(rn):   # task L1: run in the parallel part
            todo = [tuple(phase1['routes'][rn])]
            # Fixer (review finding 4a): from run_list's own line ("  [slot N]" = it ran in shared slot N), not assumed.
            m = re.search(r'  \[slot (\d+)\]', phase1['routes'][rn][1] or '')
            mode = ('shared slot %s, parallel x%d' % (m.group(1), phase1['n']) if m else
                    'not shared (no [slot N] on run_list\'s line), parallel part x%d' % phase1['n'])
        if todo is None:
            print('%-9s running %s ...' % (rn, route['script']), flush=True)
            log('')
            log('=== %s: launching %s' % (rn, route['script']))
            run, rl_line = launch(route, exe, args.extra_args, log)
            todo = [(run, rl_line)]
        all_results[rn] = []
        for run, rl_line in todo:
            log('')
            log('=== %s run %s' % (rn, run))
            results = []
            ctx = None
            if run is None:
                results.append(Result('launch', 'fail', rl_line or 'no run number'))
            else:
                ctx = Ctx(rn, run, logs, captures, rl_line, rbase, args.extra_args)
                for cname in route['checks']:
                    fn = CHECKS.get(cname)
                    if fn is None:
                        results.append(Result(cname, 'fail', 'no such check registered'))
                        continue
                    try:
                        results.extend(fn(ctx))
                    except Exception as e:  # a broken check is a failure, not a crash of the whole set
                        results.append(Result(cname, 'fail', 'check raised %s: %s' % (type(e).__name__, e)))
            identity = None
            if ctx is not None:
                st = ctx.start_time
                identity = dict(start=st.isoformat() if st else None, sdk_pin=sdk_pin_of(logs, run),
                                flags={k: v for k, v in ctx.cmdline_flags.items() if k not in RUNLIST_FLAGS},
                                older_than_exe=bool(st and exe_mtime and st < exe_mtime))
                log('  identity: %s' % json.dumps(identity))
                if identity['older_than_exe']:
                    results.append(Result('identity', 'info', 'run started %s, before the exe on disk was built (%s): '
                                          'its build is not the one hashed above' % (st, exe_mtime)))
                if base_pins and identity['sdk_pin'] and identity['sdk_pin'] not in base_pins:
                    results.append(Result('identity', 'info', 'SDK pin differs from the baseline runs: %s' %
                                          identity['sdk_pin']))
            status = 'FAIL' if any(r.status == 'fail' for r in results) else 'PASS'
            overall = overall and status == 'PASS'
            for r in results:
                log('  [%s] %-24s %s' % (r.status.upper(), r.name, r.detail))
            bench = ctx._bench if ctx is not None else None
            if bench:
                for s in SCENES:
                    sc = bench['scenes'].get(s)
                    if sc:
                        log('  %-6s frames %d-%d: %d frames, wall %.1f s, %.2f fps, >20 ms %d (%.2f%%), >25 %d, >35 %d, '
                            '>=45 %d, max %.1f ms, GPU %s ms/frame' %
                            (s, sc['start'], sc['end'], sc['frames'], sc['wall_s'], sc['fps'], sc['n20'], sc['pct20'],
                             sc['n25'], sc['n35'], sc['n45'], sc['max_ms'],
                             'n/a' if sc['gpu_ms'] is None else '%.2f' % sc['gpu_ms']))
                        log('         per %d frames fps/GPU ms: %s' % (BAR60['window'], ' '.join(
                            '%.1f/%s' % (w['fps'], 'n/a' if w['gpu_ms'] is None else '%.1f' % w['gpu_ms'])
                            for w in sc['windows'])))
            line = summary_line(rn, run, status, results, bench, rbase, rl_line)
            lines_out.append(line)
            print(line, flush=True)
            tri = None
            if status == 'FAIL':  # crash-triage verdict for the failed run (task C); clean routes skip it
                tri = triage_verdict(run, rl_line, exe)
                tline = '%-9s      triage: %s' % ('', tri)
                lines_out.append(tline)
                log(tline)
                print(tline, flush=True)
            all_results[rn].append(dict(run=run, status=status, runlist_line=rl_line, identity=identity,
                                        checks=[r._asdict() for r in results], bench=bench, triage=tri,
                                        lock_mode=mode))

    n_runs = sum(len(v) for v in all_results.values())
    n_pass = sum(x['status'] == 'PASS' for v in all_results.values() for x in v)
    n_warn = sum(any(c['status'] == 'warn' for c in x['checks']) for v in all_results.values() for x in v)
    if n_runs == 0:
        overall = False
        verdict = 'REGRESS NOTHING CHECKED  (0 runs: check --only / --runs)'
    else:
        verdict = 'REGRESS %s  (%d/%d runs pass%s)' % ('PASS' if overall else 'FAIL', n_pass, n_runs,
                                                       ', %d with warnings' % n_warn if n_warn else '')
    if args.set_baseline and n_runs:
        b = make_baseline(all_results, einfo, warn)
        old = stored_baseline
        if old:  # routes not in this call (or with no usable run) keep their old baseline
            for rn, r in old.get('routes', {}).items():
                if rn not in b['routes']:
                    b['routes'][rn] = r
        with open(args.baseline, 'w', encoding='utf-8') as f:
            json.dump(b, f, indent=1)
        log('')
        log('baseline written: %s' % args.baseline)
        for rn, r in b['routes'].items():
            for s, ms in r.get('scenes', {}).items():
                log('  %-8s %-6s %s' % (rn, s, '  '.join('%s %s +-%s (spread %s, n %d)' % (
                    m, v['base'], v['thr'], v['spread'], v['n']) for m, v in ms.items())))
        print('baseline written: %s' % args.baseline)
    t_end = time.time()
    wall = t_end - t_start                              # from the start of the call, queue waits included
    hold = (t_end - t_self) if in_hold else None        # the part inside the exclusive hold (this process)
    # e.g. "wall time: 1342 s from the start of the call (queue waits included), 864 s of it in the exclusive hold"
    wall_txt = '%.0f s from the start of the call (queue waits included)%s%s' % (
        wall, '' if hold is None else ', %.0f s of it in the exclusive hold' % hold,
        '' if not phase1 else '; functional routes in parallel: %.0f s%s' % (
            phase1['wall_s'], ' (runs set aside: the build changed)' if phase1.get('set_aside') else ''))
    log('')
    for line in lines_out:
        log(line)
    log(verdict)
    if not given:
        log('wall time: %s' % wall_txt)
    dlog.close()
    par = None
    if phase1:
        par = {k: phase1.get(k) for k in ('n', 'routes', 'set_aside', 'fp_before', 'fp_after', 'wall_s', 'from_parent')}
    with open(json_path, 'w', encoding='utf-8') as f:
        # wall_s: from the call's start (its first process, before any queue wait); hold_s: inside the exclusive hold
        # (None when this call took none). Fixer (review finding 2): wall_s used to restart inside the hold.
        json.dump(dict(stamp=stamp, argv=sys.argv[1:], exe=einfo, baseline=args.baseline if baseline else None,
                       extra_args=args.extra_args, verdict=verdict, routes=all_results, parallel=par,
                       wall_s=None if given else round(wall, 1),
                       hold_s=None if (given or hold is None) else round(hold, 1)), f, indent=1)
    print(verdict)
    if not given:
        print('wall time: %s' % wall_txt)
    print('details: %s' % detail_path)
    if n_runs == 0:
        return 2
    return 0 if overall else 1


if __name__ == '__main__':
    sys.exit(main())
