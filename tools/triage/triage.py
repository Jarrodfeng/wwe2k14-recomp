"""Crash auto-triage for one run (Session 8, task 13).

    py tools\\triage\\triage.py RUN|LOG [--json] [--brief] [--no-events] [--exe-dir DIR] [--map MAP]
                                        [--switch sub_XXXXXXXX] [--save]

RUN is a run number (30, 030, wwe2k14_030) looked up in <exe dir>\\logs, or a path to any part of a run log
(rotated parts wwe2k14_NNN.K.log are read too, oldest first). What it reads:
  - the run log: FATAL lines, [wwe2k14][crash] blocks (host backtrace + guest context), guest access violations,
    GPU crash diagnostics (device lost, device fault, checkpoints, the records around them), readback-buffer
    eviction lines, input-script results, shutdown lines, file opens, [frames] summaries, [dump] words;
  - logs\\frames_NNN.csv (did frames stop before the log did?), logs\\flight_NNN.txt if present;
  - the Windows event log (tools\\triage\\winevents.ps1, read-only): Application event 1000 entries of THIS run's
    process only (same exe path, process start time 0-3 s before the log's first line; exception code, faulting
    module + offset, exe time stamp) and System nvlddmkm event 153 (GPU resets) between the previous run's last
    line and the next run's first line. An event never overrides a log that shows a normal shutdown.
Faults outside wwe2k14.exe (the crash handler prints every address relative to the exe; past SizeOfImage = an SDK
or system DLL) are never resolved against the exe map; SDK DLL frames are symbolised with their PDBs
(llvm-symbolizer) while nothing has been rebuilt since the run. A logged guest access violation is the cause only
when it sits right before the crash block (or where log and frames stopped); probe reads that carry on are noted.
Then it classifies the end of the run and runs the automatable parts of the PLAYBOOK recipes:
  missing_function       [FATAL] unregistered function: GAP_START / IN_GAP / INSIDE_FUNC against the code index
                          (classify_candidates.py + check_boundaries.py logic), hint files that already list it,
                          data words / lis pairs that point at it (index), the log's own [scan] evidence.
  switch_out_of_range    0xC000001D (ud2 = generated 'Switch case out of range'): host offset -> map -> sub_,
                          ud2 bytes checked in the exe, the function's switches with the true table length read from
                          the image dump, and the --wwe2k14_dump_words command for each table.
  guest_access_violation 0xC0000005 / 'Unhandled guest access violation': faulting function from the map, host
                          backtrace resolved, guest lr/ctr/stack resolved, register hints (non-volatile reg = 0 after
                          a call -> setjmp/longjmp lesson), db annotations.
  device_lost            GPU crash diagnostics summary (fault types, checkpoints, last records by kind), readback
                          eviction state, GPU resets from the event log; D3D12 runs without a dump use abort +
                          GPU resets.
  script_failure         the FAILED line, failure screenshot, last steps and files.
  hang / ended_abruptly  frames stopped (frames CSV) or the log ends with no shutdown and no crash record.
  clean_exit             shutdown lines present, nothing failed.
  startup_error          the run stopped before guest code ran.
  host_access_violation / host_exception: a fault in our app code, or outside the exe (SDK / system DLL: module
                          named from the backtrace or the event, confidence medium).
  no_log                 the file has no timestamped lines.
Any run with [dump] lines (--wwe2k14_dump_words) also gets every dumped table re-measured with the live words.
Host offsets are resolved only when the map is from the build that ran (map link time and exe mtime before the
run's first log line); otherwise it says 'map stale' and resolves nothing from it.
Game-derived output (function names, addresses, records) is printed only; --save writes it under out\\harness\\triage.
"""
import argparse
import bisect
import collections
import datetime as dt
import glob
import json
import os
import pickle
import re
import sqlite3
import struct
import subprocess
import sys

REPO = r'C:\Users\Jarrod\wwe2k14-recomp'
EXE_DIR = os.path.join(REPO, 'out', 'build', 'win-amd64-release')
INDEX = os.path.join(REPO, 'out', 'harness', 'index', 'index.db')
IMAGE = os.path.join(REPO, 'out', 'harness', 'index', 'image.bin')
IMAGE_BASE = 0x82000000
CACHE = os.path.join(REPO, 'out', 'harness', 'triage', 'cache')
GEN = os.path.join(REPO, 'generated', 'default')
TS_RE = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]')
UNCOND = re.compile(r'^(blr|bctr|b 0x[0-9a-f]+)$')


def parse_ts(s):
    return dt.datetime.strptime(s, '%Y-%m-%d %H:%M:%S.%f')


def host_thread(body):
    """Host thread tag of a log line ('[t22780]'), or None."""
    m = re.search(r'\[t(\d+)\]', body[:60])
    return m.group(1) if m else None


# ----------------------------------------------------------------------------------------------- log parsing

def find_parts(arg, exe_dir):
    """-> (run number, [log parts oldest first], logs dir)"""
    if os.path.isfile(arg):
        logs = os.path.dirname(os.path.abspath(arg))
        m = re.search(r'wwe2k14_(\d+)(?:\.\d+)?\.log$', os.path.basename(arg))
        if not m:
            return None, [os.path.abspath(arg)], logs
        num = int(m.group(1))
    else:
        m = re.fullmatch(r'(?:wwe2k14_)?(\d+)(?:\.log)?', arg.strip())
        if not m:
            sys.exit(f'triage: not a run number or log file: {arg}')
        num = int(m.group(1))
        logs = os.path.join(exe_dir, 'logs')
    parts = glob.glob(os.path.join(logs, f'wwe2k14_{num:03d}.log')) + \
        glob.glob(os.path.join(logs, f'wwe2k14_{num:03d}.*.log'))
    if not parts:
        sys.exit(f'triage: no log for run {num:03d} in {logs}')

    def order(p):  # wwe2k14_NNN.K.log: larger K = older; the bare .log is the newest part
        m2 = re.search(r'\.(\d+)\.log$', p)
        return -int(m2.group(1)) if m2 else 0
    parts.sort(key=order)
    return num, parts, logs


class Run:
    def __init__(self):
        self.num = None
        self.parts = []
        self.lines = 0
        self.t_first = self.t_last = None
        self.backend = None
        self.settings = {}
        self.fatal = []            # (ts, address)
        self.crashes = []          # dicts
        self.guest_av = []         # (ts, kind, guest addr, thread)
        self.devlost = None        # dict
        self.dev_records = []      # (id, kind, rest) after device lost
        self.readback = {'reached': [], 'over_cap': [], 'would_destroy': 0, 'kept_in_flight': 0, 'lines': 0}
        self.script_failed = []    # (ts, text)
        self.script_tail = collections.deque(maxlen=6)
        self.script_steps = collections.deque(maxlen=6)  # only 'line N: ...' lines (the script's own steps)
        self.script_pass = []
        self.script_quit = None
        self.screenshots = []
        self.shutdown = []
        self.files = collections.deque(maxlen=5)
        self.file_fail = collections.Counter()
        self.frames_summary = None  # (ts, t seconds, text)
        self.scan = {}             # target -> (pointer at, kind)
        self.dumps = {}            # guest addr -> word ([wwe2k14][dump] lines)
        self.dump_ranges = []      # [start, words] per --wwe2k14_dump_words range, in log order
        self.flight_dumps = []     # (ts, number, reason, path, detail)
        self.flight_tests = []     # (ts, text): --wwe2k14_flight_test forced failures
        self.counts = collections.Counter()
        self.tail = collections.deque(maxlen=12)
        self.errors_first = []     # first few [error]/[critical] lines (for startup errors)


def parse_log(num, parts):
    r = Run()
    r.num, r.parts = num, parts
    block = None  # current multi-line block: ('crash', dict) or ('guest', dict)
    for path in parts:
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                line = line.rstrip('\n')
                r.lines += 1
                m = TS_RE.match(line)
                if not m:
                    if block is not None:
                        _continuation(block, line)
                    continue
                block = None
                ts = m.group(1)
                if r.t_first is None:
                    r.t_first = ts
                r.t_last = ts
                r.tail.append(line)
                body = line[26:]
                if ('[error]' in body[:12] or '[critical]' in body[:12]) and len(r.errors_first) < 5:
                    r.errors_first.append(line)
                if '[wwe2k14]' in body:
                    block = _wwe2k14_line(r, ts, body)
                    continue
                if 'GPU crash diagnostics' in body:
                    _gpu_diag(r, ts, body)
                elif 'Failed to await submission completion' in body:
                    r.devlost = r.devlost or {'ts': ts}
                    r.devlost.setdefault('await_failed', ts)
                elif 'Unhandled guest access violation' in body:
                    m2 = re.search(r'(read|write) of guest 0x([0-9A-Fa-f]+).*?on thread (0x[0-9A-Fa-f]+)', body)
                    if m2:  # (ts, kind, guest addr, guest thread, host thread tag [tNNNN])
                        r.guest_av.append((ts, m2.group(1), int(m2.group(2), 16), m2.group(3), host_thread(body)))
                elif '[FATAL]' in body:
                    m2 = re.search(r'unregistered function at guest address 0x([0-9A-Fa-f]+)', body)
                    r.fatal.append((ts, int(m2.group(1), 16) if m2 else None, body.split('[FATAL]', 1)[1].strip()))
                elif 'Readback buffers' in body:
                    _readback(r, body)
                elif "Vulkan device '" in body:
                    r.backend = 'vulkan'
                elif 'Direct3D 12 device' in body and r.backend is None:
                    r.backend = 'd3d12'
                elif 'Window closing' in body or 'Execution complete' in body or 'hard-exiting process' in body:
                    r.shutdown.append((ts, body.split('] ', 3)[-1][:80]))
                elif 'tw/td trap hit' in body:
                    r.counts['ppc_trap'] += 1
                elif 'Failed in backend' in body:
                    r.counts['draw_failed_in_backend'] += 1
                elif 'Resolve region is empty' in body:
                    r.counts['resolve_region_empty'] += 1
                elif 'VulkanPresenter: Failed' in body:
                    r.counts['presenter_failed'] += 1
    return r


def _wwe2k14_line(r, ts, body):
    i = body.find('[wwe2k14]')
    msg = body[i:]
    if msg.startswith('[wwe2k14][file]'):
        m = re.search(r"\[file\] (\w+) '([^']*)' .*?-> (0x[0-9A-Fa-f]+)", msg)
        if m:
            r.files.append((ts, m.group(1), m.group(2), m.group(3)))
            if m.group(3) != '0x00000000':
                r.file_fail[m.group(2)] += 1
        r.counts['file_opens'] += 1
        return None
    if msg.startswith('[wwe2k14][crash]'):
        if 'abort() called' in msg:
            c = {'ts': ts, 'kind': 'abort', 'code': None, 'offset': None, 'frames': [], 'thread': host_thread(body)}
            r.crashes.append(c)
            return ('crash', c)
        m = re.search(r'unhandled exception 0x([0-9A-Fa-f]+) at wwe2k14\.exe\+0x([0-9A-Fa-f]+)', msg)
        if m:
            # src\crash_handler.h prints (fault address - exe base) for ANY module: a fault inside an SDK DLL gives
            # a huge (or wrapped) "wwe2k14.exe+" offset. triage checks it against the exe's SizeOfImage.
            c = {'ts': ts, 'kind': 'exception', 'code': int(m.group(1), 16), 'offset': int(m.group(2), 16),
                 'frames': [], 'thread': host_thread(body)}
            r.crashes.append(c)
            return ('crash', c)
        if 'guest context' in msg:
            g = {'regs': {}, 'lr': None, 'ctr': None, 'lit': None, 'ret': [], 'raw': [], 'thread': None,
                 'note': msg.split('guest context:', 1)[1].strip()}
            if r.crashes:
                r.crashes[-1]['guest'] = g
            return ('guest', g)
        return None
    if msg.startswith('[wwe2k14][input]'):
        text = msg[len('[wwe2k14][input]'):].strip()
        r.counts['input_lines'] += 1
        if 'script FAILED' in text:
            r.script_failed.append((ts, text))
        elif re.search(r'\bPASS\b', text):
            r.script_pass.append((ts, text))
        if re.match(r'line \d+(?: \([^)]*\))?: quit\b', text):
            r.script_quit = (ts, text)
        if text.startswith('screenshot saved:'):
            r.screenshots.append(text.split(':', 1)[1].strip())
        else:
            r.script_tail.append((ts, text))
            if re.match(r'line \d+(?: \([^)]*\))?:', text):
                r.script_steps.append((ts, text))
        return None
    if msg.startswith('[wwe2k14][frames]'):
        m = re.search(r't=(\d+) s', msg)
        if m:
            r.frames_summary = (ts, int(m.group(1)), msg[len('[wwe2k14][frames]'):].strip())
        return None
    if msg.startswith('[wwe2k14][scan]'):
        m = re.search(r'\[scan\] (candidate|lone) 0x([0-9A-F]{8}) \(pointer at 0x([0-9A-F]{8})\)', msg)
        if m:
            r.scan[int(m.group(2), 16)] = (int(m.group(3), 16), m.group(1))
        return None
    if msg.startswith('[wwe2k14][flight]'):  # flight recorder (task 11): dumps and forced test failures
        m = re.search(r'dump #(\d+) \((\w+)\) written to (.*?): (.*)', msg)
        if m:
            r.flight_dumps.append((ts, int(m.group(1)), m.group(2), m.group(3), m.group(4)[:160]))
        elif '] test: ' in msg or 'test armed' in msg:
            r.flight_tests.append((ts, msg[len('[wwe2k14][flight]'):].strip()[:160]))
        return None
    if msg.startswith('[wwe2k14][dump]'):
        m = re.search(r'\[dump\] 0x([0-9A-F]{8}) \[(\d+)\] = 0x([0-9A-F]{8})', msg)
        if m:
            a = int(m.group(1), 16)
            r.dumps[a] = int(m.group(3), 16)
            if m.group(2) == '0' or not r.dump_ranges:  # [0] starts a new --wwe2k14_dump_words range
                r.dump_ranges.append([a, 0])
            r.dump_ranges[-1][1] += 1
        return None
    m = re.match(r"\[wwe2k14\] (?:modified: )?(\w+) = '(.*)' \(source: ([^)]*)\)", msg)
    if m:
        r.settings[m.group(1)] = (m.group(2), m.group(3))
    return None


def _continuation(block, line):
    kind, d = block
    if kind == 'crash':
        m = re.match(r'\s*#(\d+) (\S+)\+0x([0-9A-Fa-f]+)', line)
        if m:
            d['frames'].append((m.group(2), int(m.group(3), 16)))
        return
    m = re.search(r'guest thread id (\d+)', line)
    if m:
        d['thread'] = int(m.group(1))
    for name, val in re.findall(r'(r\d+)\s*=([0-9A-F]{8})', line):
        d['regs'][name] = int(val, 16)
    m = re.search(r'lr=([0-9A-F]{8})\s+ctr=([0-9A-F]{8})\s+last_indirect_target=([0-9A-F]{8})', line)
    if m:
        d['lr'], d['ctr'], d['lit'] = (int(x, 16) for x in m.groups())
    m = re.search(r'return addresses:((?: [0-9A-F]{8})*)', line)
    if m:
        d['ret'] = [int(x, 16) for x in m.group(1).split()]
    m = re.search(r'16 words at r1=0x[0-9A-F]{8}:((?: [0-9A-F]{8})*)', line)
    if m:
        d['raw'] = [int(x, 16) for x in m.group(1).split()]


def _gpu_diag(r, ts, body):
    text = body.split('GPU crash diagnostics:', 1)[1].strip()
    d = r.devlost = r.devlost or {'ts': ts}
    m = re.match(r'device lost; current submission (\d+), completed (\d+), frame (\d+)(.*)', text)
    if m:
        d.update(ts_dump=ts, current=int(m.group(1)), completed=int(m.group(2)), frame=int(m.group(3)),
                 extra=m.group(4).strip(' ;'))
        return
    m = re.match(r'address \d+: (\S+) at (0x[0-9A-Fa-f]+)', text)
    if m:
        d.setdefault('faults', []).append((m.group(1), m.group(2)))
        return
    m = re.match(r'stage (0x[0-9A-Fa-f]+): id (\d+) \((\S+)\)', text)
    if m:
        d.setdefault('checkpoints', []).append((m.group(1), int(m.group(2)), m.group(3)))
        return
    m = re.match(r'(\d+) queue checkpoint\(s\); last id issued (\d+)', text)
    if m:
        d['n_checkpoints'], d['last_issued'] = int(m.group(1)), int(m.group(2))
        return
    m = re.match(r'#(\d+) (\S+) (.*)', text)
    if m:
        if len(r.dev_records) < 20000:
            r.dev_records.append((int(m.group(1)), m.group(2), m.group(3)))
        return
    if text.startswith('device fault'):
        d['fault_header'] = text[:120]
    elif text:
        d.setdefault('other', []).append(text[:160])


def _readback(r, body):
    rb = r.readback
    rb['lines'] += 1
    m = re.search(r'map reached (\d+) entries at frame (\d+)', body)
    if m:
        rb['reached'].append((int(m.group(1)), int(m.group(2))))
    m = re.search(r'(\d+) entries over the cap of (\d+) at frame (\d+): evicted (\d+)', body)
    if m:  # original policy (Session 3 run 025 format)
        rb['over_cap'].append((int(m.group(3)), int(m.group(1)), int(m.group(4)), None))
    m = re.search(r'(\d+) entries \(cap (\d+)\), frame (\d+).*?evicted (\d+) \((\d+) in flight\)', body)
    if m:
        rb['over_cap'].append((int(m.group(3)), int(m.group(1)), int(m.group(4)), int(m.group(5))))
    m = re.search(r'kept (\d+) in flight; original policy would destroy (\d+) in flight', body)
    if m:
        rb['kept_in_flight'] += int(m.group(1))
        rb['would_destroy'] += int(m.group(2))


def read_frames_csv(logs, num):
    path = os.path.join(logs, f'frames_{num:03d}.csv')
    if not os.path.exists(path):
        return None
    n, last_frame, last_t, truncated = 0, None, None, False
    worst, worst_frame, stalls = 0.0, None, 0
    with open(path, encoding='utf-8', errors='replace') as f:
        next(f, None)
        for line in f:
            p = line.strip().split(',')
            if len(p) < 3:
                truncated = True
                continue
            try:
                last_frame, last_t = int(p[0]), float(p[1])
                ms = float(p[2])
                n += 1
            except ValueError:
                truncated = True
                continue
            if ms > worst:
                worst, worst_frame = ms, last_frame
            if ms >= 5000:
                stalls += 1
    return {'path': path, 'frames': n, 'last_frame': last_frame, 'last_t': last_t, 'truncated_tail': truncated,
            'worst_ms': worst, 'worst_frame': worst_frame, 'stalls_5s': stalls}


# ----------------------------------------------------------------------------------------------- event log

_EVENT_CACHE = {}


def windows_events(t0, t1):
    """Application event 1000 (wwe2k14) and nvlddmkm 153 between t0 and t1 (datetimes). Cached per window."""
    key = (t0, t1)
    if key in _EVENT_CACHE:
        return _EVENT_CACHE[key]
    ps = os.path.join(REPO, 'tools', 'triage', 'winevents.ps1')
    try:
        out = subprocess.run(['pwsh', '-NoProfile', '-File', ps, '-Start', t0.strftime('%Y-%m-%dT%H:%M:%S'),
                              '-End', t1.strftime('%Y-%m-%dT%H:%M:%S')], capture_output=True, text=True,
                             timeout=120).stdout
        data = json.loads(out.strip() or '{}')
    except Exception as e:  # noqa: BLE001
        data = {'error': str(e)}
    app = data.get('app') or []
    if isinstance(app, dict):
        app = [app]
    gpu = data.get('gpu') or []
    if isinstance(gpu, str):
        gpu = [gpu]
    res = {'app': sorted((dict(a, t=dt.datetime.fromisoformat(a['time'])) for a in app), key=lambda a: a['t']),
           'gpu': sorted(dt.datetime.fromisoformat(g) for g in gpu), 'error': data.get('error')}
    _EVENT_CACHE[key] = res
    return res


def preload_events(t0, t1):
    """Batch mode: one query for a long window, served to every run inside it."""
    ev = windows_events(t0, t1)
    _EVENT_CACHE['batch'] = (t0, t1, ev)


def next_run_start(logs, num):
    """First timestamp of the next run's log (its oldest part), or None: an event after it belongs to that run."""
    if num is None:
        return None
    nxt = sorted(glob.glob(os.path.join(logs, f'wwe2k14_{num + 1:03d}.*log')),
                 key=lambda p: -int(re.search(r'\.(\d+)\.log$', p).group(1)) if re.search(r'\.(\d+)\.log$', p) else 0)
    for p in nxt[:1]:
        try:
            with open(p, encoding='utf-8', errors='replace') as f:
                m = TS_RE.match(f.readline())
            return parse_ts(m.group(1)) if m else None
        except OSError:
            return None
    return None


def prev_run_end(logs, num):
    """Last timestamp of the previous run's log (its newest part), or None: a GPU reset before it is not ours."""
    if num is None or num < 1:
        return None
    p = os.path.join(logs, f'wwe2k14_{num - 1:03d}.log')
    try:
        with open(p, 'rb') as f:
            f.seek(0, 2)
            f.seek(max(0, f.tell() - 65536))
            tail = f.read().decode('utf-8', 'replace').splitlines()
    except OSError:
        return None
    for line in reversed(tail):
        m = TS_RE.match(line)
        if m:
            return parse_ts(m.group(1))
    return None


def all_app_events():
    """Every event-1000 entry for wwe2k14 (fixed key: one query per triage process)."""
    return windows_events(dt.datetime(2000, 1, 1), dt.datetime(2100, 1, 1))


def events_for(t_first, t_last, t_prev=None, t_next=None):
    """-> {'app': event-1000 entries from just before the run to 5 min after its last line (match_app_events then
    keeps only the ones of this run's process), 'gpu': nvlddmkm 153 resets inside the run's own window. GPU resets
    carry no process identity, so their window ends at the next run's first line and starts no earlier than the
    previous run's last line (back-to-back runs: 91 of 279 release pairs start < 5 s after the previous log ends)."""
    lo_app, hi_app = t_first - dt.timedelta(seconds=5), t_last + dt.timedelta(minutes=5)
    lo_gpu, hi_gpu = t_first - dt.timedelta(seconds=5), t_last + dt.timedelta(seconds=90)
    if t_prev is not None:
        lo_gpu = max(lo_gpu, t_prev)
    if t_next is not None:
        hi_gpu = max(min(hi_gpu, t_next), t_last)
    lo, hi = min(lo_app, lo_gpu), max(hi_app, hi_gpu)
    ev = None
    if 'batch' in _EVENT_CACHE:
        b0, b1, bev = _EVENT_CACHE['batch']
        if b0 <= lo and hi <= b1:
            ev = bev
    if ev is None:
        ev = windows_events(lo, hi)
    return {'app': [a for a in ev['app'] if lo_app <= a['t'] <= hi_app],
            'gpu': [g for g in ev['gpu'] if lo_gpu <= g <= hi_gpu], 'error': ev['error']}


# Log's first line minus the crashed process's start time. Measured on all 27 wwe2k14 event-1000 entries in the
# event log (runs 002-279): +0.04 .. +0.59 s. Two runs never start within 3 s of each other's process start.
START_TOL = (-1.0, 3.0)


def match_app_events(app, exe_path, t_first, t_last, t_prev=None, t_next=None):
    """Split event-1000 entries into (this run's, not this run's). An entry is this run's when its faulting
    application path is this exe (not the coverage build, a bisect snapshot or another program) and its process
    started 0-3 s before the log's first line ('Faulting application start time'). Entries without a start time
    (not seen so far) fall back to the run's own window, bounded by the previous and next runs."""
    want = os.path.normcase(os.path.abspath(exe_path))
    mine, other = [], []
    for a in app:
        path = (a.get('path') or '').strip()
        same_exe = bool(path) and os.path.normcase(os.path.abspath(path)) == want
        ok = False
        if same_exe and a.get('pstart'):
            try:
                d = (t_first - dt.datetime.fromisoformat(a['pstart'])).total_seconds()
                ok = START_TOL[0] <= d <= START_TOL[1]
            except ValueError:
                ok = False
        elif same_exe:
            lo, hi = t_first - dt.timedelta(seconds=5), t_last + dt.timedelta(seconds=90)
            if t_prev is not None:
                lo = max(lo, t_prev)
            if t_next is not None:
                hi = max(min(hi, t_next), t_last)
            ok = lo <= a['t'] <= hi
        (mine if ok else other).append(a)
    return mine, other


# ----------------------------------------------------------------------------------------------- map / exe

class HostMap:
    def __init__(self, path):
        self.path = path
        self.link_time = None
        self.base = 0x140000000
        self.limit = None  # SizeOfImage of the exe this map belongs to (set by the caller): lookups past it fail
        st = os.stat(path)
        os.makedirs(CACHE, exist_ok=True)
        cache = os.path.join(CACHE, f'map_{st.st_size}_{int(st.st_mtime)}.pkl')
        if os.path.exists(cache):
            try:
                with open(cache, 'rb') as f:
                    self.link_time, self.base, self.rvas, self.names = pickle.load(f)
                return
            except Exception:  # noqa: BLE001  truncated / corrupt cache (a killed triage): parse the map again
                pass
        syms = []
        sym_re = re.compile(r'\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s')
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                if self.link_time is None and 'Timestamp is' in line:
                    m = re.search(r'Timestamp is ([0-9a-f]+)', line)
                    self.link_time = dt.datetime.fromtimestamp(int(m.group(1), 16))
                elif 'Preferred load address is' in line:
                    self.base = int(line.split()[-1], 16)
                m = sym_re.match(line)
                if m:
                    syms.append((int(m.group(2), 16) - self.base, m.group(1)))
        syms.sort()
        self.rvas = [s[0] for s in syms]
        self.names = [s[1] for s in syms]
        tmp = f'{cache}.{os.getpid()}.tmp'
        try:
            with open(tmp, 'wb') as f:
                pickle.dump((self.link_time, self.base, self.rvas, self.names), f)
            os.replace(tmp, cache)  # atomic: a killed triage never leaves a truncated cache behind
            old = sorted(glob.glob(os.path.join(CACHE, 'map_*.pkl')), key=os.path.getmtime)
            for p in old[:-3]:  # about 6 MB per relink: keep the newest 3
                os.remove(p)
        except OSError:
            pass

    @staticmethod
    def link_time_only(path):
        with open(path, encoding='utf-8', errors='replace') as f:
            for _ in range(10):
                line = f.readline()
                m = re.search(r'Timestamp is ([0-9a-f]+)', line)
                if m:
                    return dt.datetime.fromtimestamp(int(m.group(1), 16))
        return None

    def lookup(self, rva):
        if rva < 0 or (self.limit is not None and rva >= self.limit):
            return None, None  # not inside wwe2k14.exe (an SDK DLL / system module address)
        i = bisect.bisect_right(self.rvas, rva) - 1
        if i < 0:
            return None, None
        return self.names[i], rva - self.rvas[i]


def still_running(log_path):
    """True when the log was written within 60 s and a wwe2k14.exe process exists (the run is in progress)."""
    try:
        if dt.datetime.now().timestamp() - os.path.getmtime(log_path) > 60:
            return False
        out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq wwe2k14.exe', '/FO', 'CSV', '/NH'],
                             capture_output=True, text=True, timeout=20).stdout
        return 'wwe2k14.exe' in out.lower()
    except (OSError, subprocess.SubprocessError):
        return False


def guest_of_symbol(name):
    m = re.fullmatch(r'(?:__imp__)?(sub_[0-9A-F]{8})', name or '')
    return m.group(1) if m else None


def exe_bytes_at(exe, rva, n=2):
    """Bytes at an RVA of a PE file (section table walk), or None."""
    try:
        with open(exe, 'rb') as f:
            hdr = f.read(4096)
            pe = struct.unpack_from('<I', hdr, 0x3C)[0]
            nsec = struct.unpack_from('<H', hdr, pe + 6)[0]
            opt = struct.unpack_from('<H', hdr, pe + 20)[0]
            sec = pe + 24 + opt
            for i in range(nsec):
                # IMAGE_SECTION_HEADER: +8 VirtualSize, +12 VirtualAddress, +16 SizeOfRawData, +20 PointerToRawData
                vsize, va, size_raw, ptr_raw = struct.unpack_from('<IIII', hdr, sec + 40 * i + 8)
                if va <= rva < va + max(vsize, size_raw):
                    f.seek(ptr_raw + rva - va)
                    return f.read(n)
    except OSError:
        return None
    return None


def exe_pe_info(exe):
    """-> (PE TimeDateStamp, SizeOfImage) of an exe/dll, or (None, None)."""
    try:
        with open(exe, 'rb') as f:
            hdr = f.read(4096)
        pe = struct.unpack_from('<I', hdr, 0x3C)[0]
        if hdr[pe:pe + 4] != b'PE\0\0':
            return None, None
        stamp = struct.unpack_from('<I', hdr, pe + 8)[0]
        size = struct.unpack_from('<I', hdr, pe + 24 + 56)[0]  # optional header +56 = SizeOfImage (PE32 and PE32+)
        return stamp, size
    except (OSError, struct.error):
        return None, None


DISPATCH_MODULES = ('ntdll.dll', 'kernelbase.dll', 'kernel32.dll')


def fault_frame(frames):
    """The faulting frame of a crash-handler backtrace: #00 is the handler itself, then the exception dispatch
    (KERNELBASE / ntdll), then the faulting instruction (run 279: #07 wwe2k14.exe+0x6C229BC = the fault offset)."""
    i = 0
    while i < len(frames) and frames[i][0].lower() not in DISPATCH_MODULES:
        i += 1
    if i >= len(frames):
        return None
    while i < len(frames) and frames[i][0].lower() in DISPATCH_MODULES:
        i += 1
    return frames[i] if i < len(frames) else None


SYMBOLIZER = r'C:\Program Files\LLVM\bin\llvm-symbolizer.exe'
SDK_DLLS = ('rexgpu-xenos.dll', 'rexruntime.dll')


def symbolize_dll(exe_dir, module, offsets, run_start, build_fresh):
    """{offset: 'function (file:line)'} for frames in an SDK DLL next to the exe, using its PDB through
    llvm-symbolizer, only when that DLL is the one that ran (nothing rebuilt since the run started). -> (dict, why)"""
    dll = os.path.join(exe_dir, module)
    if module.lower() not in SDK_DLLS or not os.path.exists(dll):
        return {}, f'{module} is not an SDK DLL next to the exe'
    if not os.path.exists(SYMBOLIZER):
        return {}, 'llvm-symbolizer not found'
    mt = dt.datetime.fromtimestamp(os.path.getmtime(dll))
    if not build_fresh or (run_start and mt > run_start):
        return {}, f'{module} not symbolised: rebuilt or relinked after the run (same rule as the map)'
    try:
        out = subprocess.run([SYMBOLIZER, f'--obj={dll}', '--relative-address', '--no-inlines', '--demangle']
                             + [f'0x{o:X}' for o in offsets], capture_output=True, text=True, timeout=60).stdout
    except (OSError, subprocess.SubprocessError) as e:
        return {}, f'llvm-symbolizer failed: {e}'
    blocks = [b.strip().splitlines() for b in out.strip().split('\n\n')]
    res = {}
    for o, b in zip(offsets, blocks):
        if b and b[0] not in ('??', ''):
            loc = b[1].strip() if len(b) > 1 else ''
            loc = re.sub(r'^.*?[\\/](?=src[\\/]|thirdparty[\\/])', '', loc)  # keep paths readable
            res[o] = f'{b[0][:120]} ({loc})' if loc and not loc.startswith('??') else b[0][:120]
    return res, f'{module} symbolised with its PDB (llvm-symbolizer; DLL written before the run)'


def map_status(exe_dir, run_start, map_override=None):
    """-> (map path or None, fresh?, reason)"""
    mp = map_override or os.path.join(exe_dir, 'wwe2k14.map')
    exe = os.path.join(exe_dir, 'wwe2k14.exe')
    if not os.path.exists(mp):
        return None, False, f'no map at {mp}'
    lt = HostMap.link_time_only(mp)
    exe_mtime = dt.datetime.fromtimestamp(os.path.getmtime(exe)) if os.path.exists(exe) else None
    fmt = lambda t: f'{t:%Y-%m-%d %H:%M:%S}' if t else 'unknown'  # noqa: E731
    if map_override:
        return mp, True, f'map given on the command line (link {fmt(lt)}); trusted as the build that ran'
    known = [t for t in (lt, exe_mtime) if t is not None]
    if not known:
        return mp, False, (f'map has no link timestamp and there is no exe next to it: cannot tell which build it '
                           f'is from; host offsets NOT resolved (pass --map MAP to trust a saved map)')
    if not run_start:
        return mp, False, 'the log has no timestamps: cannot tell whether the map is from the build that ran'
    newest = max(known)
    if newest > run_start:
        return mp, False, (f'map stale: linked {fmt(lt)}, exe written {fmt(exe_mtime)}, after the run started '
                           f'{fmt(run_start)}; host offsets NOT resolved (they belong to a build that no longer '
                           f'exists)')
    return mp, True, f'map linked {fmt(lt)}, before the run started: same build'


# ----------------------------------------------------------------------------------------------- code index

class CodeIndex:
    def __init__(self):
        self.ok = os.path.exists(INDEX)
        self.funcs = []
        self.starts = []
        if not self.ok:
            return
        self.db = sqlite3.connect(f'file:{INDEX}?mode=ro', uri=True)
        self.funcs = self.db.execute(
            'select addr, name, size, insns, file, line, has_body from funcs order by addr').fetchall()
        self.starts = [f[0] for f in self.funcs]
        meta = dict(self.db.execute('select key, value from meta').fetchall())
        self.code_lo, self.code_hi = 0x82250000, 0x835C34F4
        try:
            cr = json.loads(meta.get('code_range', 'null'))
            if cr:
                self.code_lo, self.code_hi = cr
        except (ValueError, TypeError):
            pass
        self.stale = []
        stamp = os.path.join(GEN, 'codegen.stamp')
        fp = meta.get('codegen_fingerprint')
        if fp and os.path.exists(stamp):
            try:
                if fp not in open(stamp, encoding='utf-8', errors='replace').read():
                    self.stale.append('codegen changed since the index was built (py tools\\q\\build_index.py)')
            except OSError:
                pass

    def containing(self, addr):
        i = bisect.bisect_right(self.starts, addr) - 1
        return self.funcs[i] if i >= 0 else None

    def next_after(self, addr):
        i = bisect.bisect_right(self.starts, addr)
        return self.funcs[i] if i < len(self.funcs) else None

    def name(self, addr):
        if not addr:
            return '?'
        f = self.containing(addr)
        if not f:
            return '?'
        off = addr - f[0]
        inside = '' if off < max(f[2], 4) else ' (past its end)'
        return f'{f[1]}+0x{off:X}{inside}' if off else f[1]

    def body(self, func):
        """Generated body lines of a function (from its file:line in the index)."""
        addr, name, size, insns, file, line, has_body = func
        if not has_body or not file:
            return []
        path = os.path.join(GEN, file)
        out = []
        try:
            with open(path, encoding='utf-8', errors='replace') as f:
                for i, l in enumerate(f, 1):
                    if i < line:
                        continue
                    if i > line and (l.startswith('DEFINE_REX_FUNC(') or l.startswith('}')):
                        if l.startswith('}'):
                            out.append(l.rstrip('\n'))
                        break
                    out.append(l.rstrip('\n'))
        except OSError:
            return []
        return out

    def walk(self, func):
        """[(guest address, instruction text)], labels, switches [{bctr, reg, labels, table}]"""
        lines = self.body(func)
        addr = func[0]
        ins, labels, switches = [], [], []
        # Constant tracking within a block (reset at labels): lis/addi/ori build addresses. The load before the bctr
        # names the table: lwzx = absolute 32-bit targets; lhzx / lbzx = 16 / 8-bit offsets added to a base (add).
        regs = {}
        load_dest, shift = None, 0
        load = None    # (op, table address) of the last indexed load
        base = None    # base address of the last add with a constant operand (relative tables)
        bounds = {}    # register -> (immediate, address) of its last cmplwi (the index bound check)
        pending_bctr = None
        i = 0
        while i < len(lines):
            s = lines[i].strip()
            lm = re.match(r'loc_([0-9A-F]{8}):', s)
            if lm:
                addr = int(lm.group(1), 16)
                labels.append(addr)
                regs, load, base = {}, None, None  # bound survives labels (loop heads sit between it and the table)
            elif s.startswith('// '):
                text = s[3:].strip()
                ins.append((addr, text))
                m = re.match(r'(lis|addi|ori|lwzx|lhzx|lbzx|add|cmplwi|rlwinm) (\S+)$', text)
                if m:
                    op, ar = m.group(1), m.group(2).split(',')
                    if op == 'lis' and len(ar) == 2:
                        regs[ar[0]] = (int(ar[1]) & 0xFFFF) << 16
                    elif op in ('addi', 'ori') and len(ar) == 3 and ar[1] in regs:
                        v = regs[ar[1]] + int(ar[2]) if op == 'addi' else regs[ar[1]] | (int(ar[2]) & 0xFFFF)
                        regs[ar[0]] = v & 0xFFFFFFFF
                    elif op in ('lwzx', 'lhzx', 'lbzx') and len(ar) == 3:
                        t = regs.get(ar[1], regs.get(ar[2]))
                        load = (op, t)
                        load_dest, shift = ar[0], 0
                        regs.pop(ar[0], None)
                    elif op == 'rlwinm' and len(ar) == 5 and ar[1] == load_dest and ar[0] == load_dest:
                        sh, mb, me = (int(x) for x in ar[2:])
                        if mb == 0 and me == 31 - sh:
                            shift = sh  # offsets scaled (e.g. byte offsets in units of 4 bytes)
                    elif op == 'add' and len(ar) == 3:
                        b = regs.get(ar[1], regs.get(ar[2]))
                        if b is not None:
                            base = b
                        regs.pop(ar[0], None)
                    elif op == 'cmplwi' and len(ar) >= 2:
                        try:
                            bounds[ar[-2]] = (int(ar[-1]), addr)  # per compared register
                        except ValueError:
                            pass
                if text == 'bctr':
                    pending_bctr = addr
                addr += 4
            elif pending_bctr is not None and s.startswith('switch (ctx.r'):
                reg = int(re.match(r'switch \(ctx\.r(\d+)', s).group(1))
                cases = []
                j = i + 1
                while j < len(lines) and not lines[j].strip().startswith('default:'):
                    cm = re.search(r'goto loc_([0-9A-F]{8});', lines[j])
                    if cm:
                        cases.append(int(cm.group(1), 16))
                    j += 1
                op, table = load if load else (None, None)
                switches.append({'bctr': pending_bctr, 'reg': reg, 'labels': cases, 'load': op,
                                 'table': table, 'base': base if op in ('lhzx', 'lbzx') else None,
                                 'shift': shift if op in ('lhzx', 'lbzx') else 0,
                                 'bound': (lambda b: b[0] if b and 0 < pending_bctr - b[1] <= 0x100 else None)(
                                     bounds.get(f'r{reg}'))})
                pending_bctr = None
                i = j
            elif pending_bctr is not None and s:
                pending_bctr = None  # this bctr is an indirect jump/tail call, not a switch
            i += 1
        return ins, labels, switches

    def data_refs(self, value, limit=10):
        try:
            ptrs = self.db.execute('select at, kind from ptrs where value=? limit ?', (value, limit)).fetchall()
            imms = self.db.execute('select func, site, op from imms where value=? limit ?', (value, limit)).fetchall()
        except sqlite3.Error:
            return [], []
        return ptrs, imms


class Image:
    def __init__(self):
        self.data = None
        if os.path.exists(IMAGE):
            with open(IMAGE, 'rb') as f:
                self.data = f.read()

    def word(self, addr):
        off = addr - IMAGE_BASE
        if self.data is None or off < 0 or off + 4 > len(self.data):
            return None
        return struct.unpack_from('>I', self.data, off)[0]


def load_db_entries():
    out = {}
    for path in glob.glob(os.path.join(REPO, 'db', '*.json')):
        try:
            data = json.load(open(path, encoding='utf-8'))
        except (OSError, ValueError):
            continue
        for e in data if isinstance(data, list) else data.get('entries', []):
            try:
                out[int(e['address'], 16)] = (e.get('name'), e.get('confidence'), os.path.basename(path))
            except (KeyError, ValueError, TypeError):
                pass
    return out


def hint_files_listing(addr):
    hits = []
    pat = re.compile(rf'0x0*{addr:X}\b', re.I)
    for path in sorted(glob.glob(os.path.join(REPO, 'config', '*.toml'))) + [os.path.join(REPO, 'wwe2k14_manifest.toml')]:
        try:
            for n, l in enumerate(open(path, encoding='utf-8', errors='replace'), 1):
                if pat.search(l.split('#', 1)[0]):
                    hits.append(f'{os.path.relpath(path, REPO)}:{n}')
        except OSError:
            pass
    return hits


# ----------------------------------------------------------------------------------------------- recipes

def recipe_missing_function(addr, run, idx, out):
    """PLAYBOOK Session 2 classifier: GAP_START / IN_GAP / INSIDE_FUNC (+ check_boundaries acceptance)."""
    culprit = f'0x{addr:08X}'
    if not idx.ok:
        out.append('code index missing (py tools\\q\\build_index.py): classifier skipped')
        return culprit, 'unclassified'
    hints = hint_files_listing(addr)
    if not (idx.code_lo <= addr < idx.code_hi):
        out.append(f'0x{addr:08X} is outside the code range 0x{idx.code_lo:08X}-0x{idx.code_hi:08X}: data or a '
                   f'corrupt pointer, never a hint')
        return culprit, 'OUTSIDE_CODE'
    f = idx.containing(addr)
    cls, verdict = None, None
    if f and f[0] == addr:
        cls = 'REGISTERED'
        out.append(f'0x{addr:08X} is a registered function in the current codegen ({f[1]}, {f[2]} bytes): '
                   f'already fixed since this run' + (f' by {", ".join(hints)}' if hints else ''))
    elif f and addr < f[0] + f[2]:
        cls = 'INSIDE_FUNC'
        out.append(f'INSIDE_FUNC: 0x{addr:08X} = {f[1]}+0x{addr - f[0]:X} (size 0x{f[2]:X}). A mid-function '
                   f'address (switch label / exception-scope entry): do NOT hint it; find who computed it')
    else:
        prev_end = f[0] + f[2] if f else idx.code_lo
        gap = addr - prev_end
        nxt = idx.next_after(addr)
        cls = 'GAP_START' if gap == 0 else 'IN_GAP'
        out.append(f'{cls}: 0x{addr:08X} after {f[1] if f else "?"} (ends 0x{prev_end:08X}, gap 0x{gap:X}); '
                   f'next function {nxt[1] if nxt else "?"} (uncovered span 0x{prev_end:08X}-'
                   f'0x{nxt[0] if nxt else 0:08X})')
        if cls == 'GAP_START' and f:
            ins, labels, _ = idx.walk(f)
            last = ins[-1][1] if ins else ''
            if not UNCOND.match(last):
                verdict = f'REJECT: {f[1]} ends with "{last}" (could fall through into 0x{addr:08X})'
            elif labels and max(labels) >= addr:
                verdict = f'REJECT: {f[1]} has a label at/after 0x{addr:08X}'
            else:
                verdict = f'ACCEPT (check_boundaries rule): {f[1]} ends with "{last}"'
        else:
            earlier = sorted(t for t in run.scan if prev_end <= t < addr)
            verdict = (f'IN_GAP: accept only as a chain behind an accepted target (log [scan] targets earlier in '
                       f'this gap: {", ".join(f"0x{t:08X}" for t in earlier) or "none"}) or when the pointer '
                       f'table is a vtable')
        out.append('  ' + verdict)
    if addr in run.scan:
        at, kind = run.scan[addr]
        out.append(f'  this run\'s data scan saw it: [scan] {kind} 0x{addr:08X} (pointer at 0x{at:08X})')
    ptrs, imms = idx.data_refs(addr)
    if ptrs:
        out.append('  data words pointing at it (index): ' + ', '.join(f'0x{a:08X} ({k})' for a, k in ptrs[:6]))
    if imms:
        out.append('  code building it (index lis pairs): ' + ', '.join(
            f'{idx.name(fn)} @0x{site:08X} ({op})' for fn, site, op in imms[:6]))
    if hints and cls != 'REGISTERED':
        out.append(f'  listed in: {", ".join(hints)}')
    if cls in ('GAP_START', 'IN_GAP') and not hints:
        out.append(f'  if accepted: add "0x{addr:08X} = {{}} # wwe2k14_{run.num:03d}.log, <what was happening>" '
                   f'to config\\hints_live.toml (live crash = rule-5 evidence), rebuild, then '
                   f'py tools\\analysis\\postcheck_hints.py hints_live')
    return culprit, cls


def recipe_switch(guest_fn, idx, image, run, out):
    """PLAYBOOK Session 2: find the function's switches, read the true table length, print the dump command."""
    if not idx.ok:
        out.append('code index missing: switch recipe skipped')
        return []
    f = next((x for x in idx.funcs if x[1] == guest_fn), None) if guest_fn else None
    if not f:
        out.append(f'{guest_fn}: not in the code index')
        return []
    nxt = idx.next_after(f[0])
    fend = nxt[0] if nxt else f[0] + f[2]
    _, _, switches = idx.walk(f)
    if not switches:
        out.append(f'{guest_fn} has no bctr switch in the generated code: the ud2 is something else '
                   f'(check the host disassembly)')
        return []
    cmds = []

    def word(a):
        w = run.dumps.get(a) if run.dumps else None
        return w if w is not None else image.word(a)

    for sw in switches:
        n = len(sw['labels'])
        bound = f', bound check cmplwi {sw["bound"]} (= {sw["bound"] + 1} cases)' if sw.get('bound') is not None else ''
        src = 'image dump' if image.data is not None else None
        if sw.get('load') in ('lhzx', 'lbzx') and sw.get('table') is not None and sw.get('base') is not None:
            # Relative table: 16/8-bit offsets at `table`, target = base + offset. Measuring by "target inside the
            # function" is weak here (any small offset lands inside), so the bound check is the better witness.
            size = 2 if sw['load'] == 'lhzx' else 1
            table, base = sw['table'], sw['base']
            true_n = 0
            while src and true_n <= 4096:
                a = table + size * true_n
                w = word(a & ~3)
                if w is None:
                    break
                off = ((w >> (8 * (4 - size - (a & 3)))) & ((1 << (8 * size)) - 1)) << sw.get('shift', 0)
                if not (f[0] <= base + off < fend and (base + off) % 4 == 0):
                    break
                true_n += 1
            words = (max(n, true_n) * size + 3) // 4 + 1
            cmd = f'--wwe2k14_dump_words=0x{table & ~3:08X}:{words}'
            cmds.append(cmd)
            if sw.get('bound') is not None:  # the bound check is the witness for offset tables
                status = 'ok (bound = labels)' if sw['bound'] + 1 == n else 'BOUND != LABELS'
            else:
                status = ('ok?' if true_n == n else 'TOO SHORT?' if true_n > n else 'shorter?') if src else '?'
            out.append(f'switch r{sw["reg"]} bctr 0x{sw["bctr"]:08X}: RELATIVE table ({sw["load"]}: {8 * size}-bit '
                       f'offsets{" << " + str(sw["shift"]) if sw.get("shift") else ""} at 0x{table:08X} + base '
                       f'0x{base:08X}){bound}: ReXGlue {n} labels -> {status}; image: {true_n} consecutive '
                       f'in-function targets (weak for offset tables: small offsets always land inside)')
            out.append(f'    read it live: {cmd}   (the offsets table, then base + offset per entry)')
            continue
        table = sw['table'] if sw['table'] is not None else sw['bctr'] + 4
        true_n, stop = 0, None
        while src:
            w = word(table + 4 * true_n)
            if w is None or not (f[0] <= w < fend and w % 4 == 0) or true_n > 4096:
                stop = w
                break
            true_n += 1
        inline = table == sw['bctr'] + 4
        status = ('TOO SHORT' if true_n > n else 'ok' if true_n == n else 'shorter than labels?') if src else '?'
        if sw.get('bound') is not None and sw['bound'] + 1 != n:
            status += (f'; BOUND CHECK SAYS {sw["bound"] + 1} (the cmplwi may guard another path, like the '
                       f'decoders\' escape code in PLAYBOOK Session 2; for absolute tables the image is the witness)')
        count = max(n, true_n) + 2
        cmd = f'--wwe2k14_dump_words=0x{sw["bctr"]:08X}:{count}'
        cmds.append(cmd)
        stop_s = f'0x{stop:08X}' if stop is not None else 'end of image'
        out.append(f'switch r{sw["reg"]} bctr 0x{sw["bctr"]:08X} table 0x{table:08X}'
                   f'{" (inline)" if inline else ""}{bound}: ReXGlue {n} labels, '
                   + (f'image says {true_n} in-function entries (next word {stop_s}) -> {status}' if src
                      else 'no image dump to measure the table'))
        out.append(f'    read it live: {cmd}   (bctr word, the entries, one past the end)')
        if src and true_n > n:
            labels = [image.word(table + 4 * k) for k in range(true_n)]
            out.append(f'    proposed [[switch_tables]] address = 0x{sw["bctr"]:08X}, register = {sw["reg"]}, '
                       f'labels = [{", ".join(f"0x{x:08X}" for x in labels)}]  (verify: index range reasoning + '
                       f'live dump, then config\\switch_tables.toml)')
    return cmds


def recipe_dumps(run, idx, image, out, show_ok=5):
    """Any run with [dump] lines (--wwe2k14_dump_words), whatever its class: match each dumped range to the switch
    it belongs to (inline table: the range starts at the bctr; relative table: the code that builds the table
    address) and re-measure that table with the live words (recipe_switch prefers them over the image dump)."""
    if not run.dump_ranges:
        return
    out.append(f'{sum(n for _, n in run.dump_ranges)} [dump] lines, {len(run.dumps)} distinct words, in '
               f'{len(run.dump_ranges)} range(s) (--wwe2k14_dump_words): live memory; the tables below are measured '
               f'with these words where they cover them')
    if not idx.ok:
        out.append('  code index missing: dumped ranges not matched to switches')
        return
    if image.data is not None:
        differ = sorted(a for a, w in run.dumps.items() if image.word(a) is not None and image.word(a) != w)
        out.append(f'  live words vs the image dump: {len(run.dumps) - len(differ)} equal, {len(differ)} differ'
                   + (f' (first at 0x{differ[0]:08X})' if differ else ''))
    walked, counts, ok_shown, hidden = {}, collections.Counter(), 0, 0
    for start, n in run.dump_ranges:
        fn = None
        f = idx.containing(start)
        if f and idx.code_lo <= start < idx.code_hi and start < f[0] + max(f[2], 4):
            fn = f  # inline table: the dump starts at the bctr inside this function
        else:  # relative offsets table in data: find the function whose code builds this address (index lis pairs)
            for a in range(start, start + 4):
                _, imms = idx.data_refs(a)
                if imms:
                    fn = idx.containing(imms[0][0])
                    break
        if fn is None:
            counts['no switch found'] += 1
            out.append(f'  0x{start:08X}:{n}: no function in the index contains or builds this address')
            continue
        if fn[0] not in walked:
            lines = []
            recipe_switch(fn[1], idx, image, run, lines)
            walked[fn[0]] = [l for l in lines if l.startswith('switch r') or l.startswith('    ')]
        lines = walked[fn[0]]
        # a range may start at the bctr (the recipe's own command), at the inline table (bctr + 4: the Session 2
        # dump run 012), at an offsets table in data, or at a relative table's base (bctr + 4, code: the bound check
        # is the witness there, not the dumped words)
        k = next((k + 1 for k, l in enumerate(lines[:-1]) if l.startswith('switch r') and (
            f'bctr 0x{start:08X}' in l or f'table 0x{start:08X}' in l or f'base 0x{start:08X}' in l
            or f'--wwe2k14_dump_words=0x{start:08X}:' in lines[k + 1])), None)
        if k is None:
            counts['range is not one of the function\'s switch tables'] += 1
            out.append(f'  0x{start:08X}:{n} -> {fn[1]}: not the start of one of its switch tables (bctr or offsets '
                       f'table); see py tools\\triage\\triage.py {run.num if run.num is not None else "LOG"} '
                       f'--switch {fn[1]}')
            continue
        sw = lines[k - 1]
        status = sw.split('->', 1)[1].strip() if '->' in sw else '?'
        bucket = 'ok' if status.startswith('ok') else status.split(';')[0].split(' (')[0]
        counts[bucket] += 1
        if bucket == 'ok' and ok_shown >= show_ok:
            hidden += 1
            continue
        ok_shown += bucket == 'ok'
        out.append(f'  0x{start:08X}:{n} -> {fn[1]} {sw[:230]}')
        out += [f'  {l}' for l in lines[k + 1:k + 2] if l.startswith('    proposed')]
    if hidden:
        out.append(f'  ... {hidden} more ranges measure ok')
    out.append(f'  dumped tables by result: {dict(counts)}')


CRASH_MARK = b'[wwe2k14][crash] unhandled exception 0x'


def guest_signature(code, av, g):
    """Map-independent crash signature: exception code, the guest AV tied to the crash (kind + guest address), the
    guest lr / ctr / last indirect target and the first 3 guest return addresses. These are guest values, so they
    survive relinks (the host offset does not). None when there is nothing guest-side to compare."""
    if g is None:
        return None
    ret = tuple(a for a in (g.get('ret') or [])[:3] if a)
    parts = (av, g.get('lr') or 0, g.get('ctr') or 0, g.get('lit') or 0, ret)
    if not any(parts):
        return None
    return (code,) + parts


def scan_crash_signatures(logs):
    """{run number: [guest signature, ...]} for every log in the folder, by a raw byte search for crash blocks
    (cached per file size + mtime in out\\harness\\triage\\cache)."""
    import mmap
    os.makedirs(CACHE, exist_ok=True)
    cpath = os.path.join(CACHE, 'crashsig_' + re.sub(r'\W+', '_', os.path.abspath(logs))[-80:] + '.json')
    try:
        cache = json.load(open(cpath, encoding='utf-8'))
    except (OSError, ValueError):
        cache = {}
    out, changed = {}, False
    for p in glob.glob(os.path.join(logs, 'wwe2k14_*.log')):
        m = re.search(r'wwe2k14_(\d+)(?:\.\d+)?\.log$', p)
        if not m:
            continue
        st = os.stat(p)
        key = os.path.basename(p)
        ent = cache.get(key)
        if not ent or ent[0] != st.st_size or ent[1] != int(st.st_mtime):
            sigs = []
            if st.st_size:
                with open(p, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mm:
                    pos = mm.find(CRASH_MARK)
                    while pos != -1:
                        before = mm[max(0, pos - 600):pos].decode('utf-8', 'replace')
                        after = mm[pos:pos + 8000].decode('utf-8', 'replace')
                        code = int(re.match(r'\[wwe2k14\]\[crash\] unhandled exception 0x([0-9A-Fa-f]+)', after)
                                   .group(1), 16)
                        avs = re.findall(r'Unhandled guest access violation: (read|write) of guest 0x([0-9A-Fa-f]+)',
                                         before)
                        av = f'{avs[-1][0]} 0x{int(avs[-1][1], 16):08X}' if avs else None
                        g = None
                        gi = after.find('[wwe2k14][crash] guest context')
                        if gi != -1:
                            gtext = after[gi:]
                            mm2 = re.search(r'lr=([0-9A-F]{8})\s+ctr=([0-9A-F]{8})\s+last_indirect_target=([0-9A-F]{8})',
                                            gtext)
                            mr = re.search(r'return addresses:((?: [0-9A-F]{8})*)', gtext)
                            g = {'lr': int(mm2.group(1), 16) if mm2 else 0, 'ctr': int(mm2.group(2), 16) if mm2 else 0,
                                 'lit': int(mm2.group(3), 16) if mm2 else 0,
                                 'ret': [int(x, 16) for x in mr.group(1).split()] if mr else []}
                        sig = guest_signature(code, av, g)
                        if sig:
                            sigs.append(list(sig[:5]) + [list(sig[5])])
                        pos = mm.find(CRASH_MARK, pos + 1)
            ent = [st.st_size, int(st.st_mtime), sigs]
            cache[key] = ent
            changed = True
        if ent[2]:
            out.setdefault(int(m.group(1)), []).extend(tuple(s[:5]) + (tuple(s[5]),) for s in ent[2])
    if changed:
        try:
            tmp = f'{cpath}.{os.getpid()}.tmp'
            with open(tmp, 'w', encoding='utf-8') as f:
                json.dump(cache, f)
            os.replace(tmp, cpath)
        except OSError:
            pass
    return out


def resolve_host(frames, hmap, dll_syms=None):
    out = []
    for mod, off in frames:
        if mod.lower() == 'wwe2k14.exe' and hmap:
            name, delta = hmap.lookup(off)
            out.append(f'{mod}+0x{off:X}  {name}+0x{delta:X}' if name else f'{mod}+0x{off:X}  ?')
        elif dll_syms and off in dll_syms.get(mod.lower(), {}):
            out.append(f'{mod}+0x{off:X}  {dll_syms[mod.lower()][off]}')
        else:
            out.append(f'{mod}+0x{off:X}')
    return out


def first_guest_frame(frames, hmap, skip_helpers=True):
    if not hmap:
        return None
    for mod, off in frames:
        if mod.lower() != 'wwe2k14.exe':
            continue
        name, _ = hmap.lookup(off)
        g = guest_of_symbol(name)
        if g:
            return g
    return None


# ----------------------------------------------------------------------------------------------- triage

def triage(arg, exe_dir=EXE_DIR, use_events=True, map_override=None, idx=None, image=None, dbe=None,
           switch_fn=None):
    num, parts, logs = find_parts(arg, exe_dir)
    if os.path.basename(os.path.dirname(logs)) and os.path.exists(os.path.join(os.path.dirname(logs), 'wwe2k14.exe')):
        exe_dir = os.path.dirname(logs)
    exe = os.path.join(exe_dir, 'wwe2k14.exe')
    run = parse_log(num, parts)
    idx = idx or CodeIndex()
    image = image or Image()
    dbe = dbe if dbe is not None else load_db_entries()
    t_first = parse_ts(run.t_first) if run.t_first else None
    t_last = parse_ts(run.t_last) if run.t_last else None
    frames = read_frames_csv(logs, num) if num is not None else None
    app_events, other_events, gpu_resets = [], [], []
    if use_events and t_first:
        t_prev, t_next = prev_run_end(logs, num), next_run_start(logs, num)
        ev = events_for(t_first, t_last, t_prev, t_next)
        app_events, other_events = match_app_events(ev['app'], exe, t_first, t_last, t_prev, t_next)
        gpu_resets = ev['gpu']
    else:
        ev = {'app': [], 'gpu': [], 'error': 'skipped'}
    gpu_last_min = [g for g in gpu_resets if t_last and g >= t_last - dt.timedelta(seconds=60)]
    mp, map_fresh, map_reason = map_status(exe_dir, t_first, map_override)
    exe_stamp, exe_size = exe_pe_info(exe)
    # An offset past the exe's image is in another module. With a stale map the exe on disk is a later build:
    # allow it 64 MB of difference before calling an offset 'outside' (DLL offsets are gigabytes away or wrapped).
    exe_limit = (exe_size or 0x20000000) + (0 if map_fresh and exe_size else 0x4000000)
    hmap = None

    def get_map():
        nonlocal hmap
        if hmap is None and mp and map_fresh:
            hmap = HostMap(mp)
            hmap.limit = exe_limit
        return hmap

    details, secondary, nexts = [], [], []
    cls, conf, culprit = None, 'low', None
    crash = run.crashes[-1] if run.crashes else None
    exc_code = crash['code'] if crash and crash['kind'] == 'exception' else None
    exc_off = crash['offset'] if crash and crash['kind'] == 'exception' else None
    ev_abort = [a for a in app_events if a.get('module', '').lower() == 'ucrtbase.dll'
                and a.get('code', '').lower() == '0xc0000409']
    ev_crash = [a for a in app_events if a not in ev_abort]  # this process's own exception event (exe or a DLL)
    ev_own = ev_crash[-1] if ev_crash else None
    fault_mod = fault_mod_off = None  # the faulting module when the fault is outside wwe2k14.exe
    from_event = False
    if exc_code is None and ev_own is not None:
        what = (f'{ev_own["time"]} {ev_own.get("module")} {ev_own.get("code")} offset {ev_own.get("offset")}, '
                f'pid {ev_own.get("pid")}')
        if run.shutdown:  # the log shows a normal end: the event never decides the class
            secondary.append(f'event log: this process crashed AFTER its shutdown lines ({what}): a crash on exit, '
                             f'the run itself ended normally')
        else:  # no crash block (log not flushed / no handler yet): use this process's event
            from_event = True
            exc_code = int(ev_own['code'], 16)
            off = int(ev_own.get('offset') or '0', 16)
            if (ev_own.get('module') or '').lower() == 'wwe2k14.exe':
                exc_off = off
            else:
                fault_mod, fault_mod_off = ev_own.get('module'), off
            details.append(f'the log has no crash block: the exception comes from this process\'s event log entry '
                           f'({what})')
    outside = fault_mod is not None or (exc_off is not None and exc_off >= exe_limit)
    if exc_off is not None and exc_off >= exe_limit:
        ff = fault_frame(crash['frames']) if crash else None
        if ff and ff[0].lower() != 'wwe2k14.exe':
            fault_mod, fault_mod_off = ff
            how = 'the first backtrace frame after the exception dispatch'
        elif ev_own is not None and (ev_own.get('module') or '').lower() != 'wwe2k14.exe':
            fault_mod, fault_mod_off = ev_own.get('module'), int(ev_own.get('offset') or '0', 16)
            how = 'the event log entry\'s faulting module'
        else:
            fault_mod, how = 'unknown module', 'neither the backtrace nor the event log names it'
        details.append(f'the fault is outside wwe2k14.exe: offset 0x{exc_off:X} from the exe base is past its '
                       f'SizeOfImage 0x{exe_size or 0:X} (src\\crash_handler.h prints every address relative to the '
                       f'exe). Faulting module: {fault_mod}' + (f'+0x{fault_mod_off:X}' if fault_mod_off is not None
                                                               else '') + f' ({how}); the map is not used for it')
    exe_off = exc_off if (exc_off is not None and not outside) else None

    def module_culprit():
        if fault_mod_off is None:
            return fault_mod, ''
        syms, why = symbolize_dll(exe_dir, fault_mod, [fault_mod_off], t_first, map_fresh)
        return f'{fault_mod}+0x{fault_mod_off:X}' + (f' {syms[fault_mod_off]}' if fault_mod_off in syms else ''), why

    # A logged guest AV is the cause only when it is where the run ended: right before the crash block (the
    # Lettering crash 026-051: same host thread, 0-1 ms), or, with no crash record, where log and frames stopped.
    # Fault-safe probes (--wwe2k14_det_hash, --wwe2k14_peek) log guest AVs and carry on (run 267: 27 of them).
    anchor = (parse_ts(run.frames_summary[0]) - dt.timedelta(seconds=run.frames_summary[1])
              if run.frames_summary else None)
    last_frame_time = (anchor + dt.timedelta(seconds=frames['last_t'])
                       if anchor and frames and frames['last_t'] is not None else None)

    def correlated_av():
        if not run.guest_av:
            return None
        if crash is not None:
            ct = parse_ts(crash['ts'])
            for av in reversed(run.guest_av):
                d = (ct - parse_ts(av[0])).total_seconds()
                if d < 0:
                    continue
                if d > 1.0:
                    return None
                if (av[4] and av[4] == crash.get('thread')) or d <= 0.1:
                    return av
            return None
        av = run.guest_av[-1]
        at = parse_ts(av[0])
        if from_event:  # the event is written ~0.5 s after the fault
            return av if 0 <= (ev_own['t'] - at).total_seconds() <= 10 else None
        if run.shutdown or (t_last - at).total_seconds() > 5:
            return None
        if last_frame_time is not None:
            return av if last_frame_time <= at + dt.timedelta(seconds=1) else None
        return av if (t_last - at).total_seconds() <= 1 else None
    gav = correlated_av()
    abort_gpu = bool(crash and crash['kind'] == 'abort' and any('rexgpu' in m.lower() for m, _ in crash['frames']))
    devlost_log = run.devlost is not None and ('await_failed' in run.devlost or 'current' in run.devlost)

    # ---- classify (strongest evidence first)
    if run.t_first is None:
        cls, conf = 'no_log', 'high'
        details.append(f'the file has no timestamped log lines ({run.lines} lines): empty, cut off before its first '
                       f'line, or not a run log')
    elif run.fatal:
        ts, addr, text = run.fatal[-1]
        cls, conf = ('missing_function', 'high') if addr is not None else ('fatal_other', 'medium')
        details.append(f'[FATAL] at {ts}: {text}')
        if addr is not None:
            culprit, sub = recipe_missing_function(addr, run, idx, details)
            details.append(f'classifier: {sub}')
            if sub == 'REGISTERED':
                nexts.append('nothing to fix: the current build registers it; rerun the route to confirm')
            nexts.append(f'py tools\\q\\q.py addr 0x{addr:08X}')
            if sub in ('GAP_START', 'IN_GAP', 'INSIDE_FUNC'):
                nexts.append(f'py tools\\analysis\\inspect_hint.py 0x{addr:08X} <source>  (predecessor tail + '
                             f'who branches there)')
    elif exc_code == 0xC000001D and not outside:
        cls, conf = 'switch_out_of_range', 'high'
        details.append(f'0xC000001D illegal instruction at wwe2k14.exe+0x{exc_off:X}: the generated '
                       f'"default: __builtin_trap(); // Switch case out of range" is a ud2 (PLAYBOOK Session 2)')
        hm = get_map()
        if hm:
            name, delta = hm.lookup(exc_off)
            g = guest_of_symbol(name)
            b = exe_bytes_at(exe, exc_off)
            details.append(f'map: wwe2k14.exe+0x{exc_off:X} = {name}+0x{delta or 0:X}; bytes there '
                           f'{b.hex(" ") if b else "?"}{" = ud2" if b == bytes([0x0F, 0x0B]) else " (NOT ud2)"}')
            if b != bytes([0x0F, 0x0B]):
                conf = 'medium'
            culprit = g or name
            if g:
                cmds = recipe_switch(g, idx, image, run, details)
                if cmds:
                    flag = '--wwe2k14_dump_words=' + ','.join(c.split('=', 1)[1] for c in cmds)
                    nexts.append(f'pwsh -NoProfile -Command "& {REPO}\\tools\\bench\\run_list.ps1 -Scripts '
                                 f"@('..\\..\\..\\tools\\input_scripts\\selftest_title.txt') -Extra "
                                 f"'{flag}'\"   (any short route: the words are dumped at launch; then "
                                 f"py tools\\triage\\triage.py NNN on that dump run re-measures every dumped table "
                                 f"from its [dump] lines, or py tools\\triage\\triage.py NNN --switch {g} for this "
                                 f"function only)")
                nexts.append(f'py tools\\analysis\\list_function.py {g[4:]}')
        else:
            details.append('host offset not resolved: map stale (see the map line)')
            nexts.append('reproduce on the current build (the crash handler logs the host offset), then rerun triage '
                         'before the next relink; by hand: that build\'s wwe2k14.map -> sub_ -> its switch -> '
                         '--wwe2k14_dump_words=<bctr>:<n> -> [[switch_tables]] (PLAYBOOK Session 2 recipe)')
    elif exc_code == 0xC0000005 or (gav is not None and crash is None and exc_code is None):
        # Only a guest AV tied to the end of the run counts (correlated_av); others are probe noise (secondary).
        cls = 'guest_access_violation' if gav else 'host_access_violation'
        conf = 'high' if exc_code == 0xC0000005 and not outside else 'medium'
        if exc_code != 0xC0000005:
            details.append('no crash block and no event-log entry: the log and the frames stopped at this guest '
                           'access violation')
        if gav:
            ts, kind, gaddr, thr, hthr = gav
            near_null = gaddr < 0x10000
            tie = ('logged right before the crash block' + (' on the same host thread' if crash and hthr and
                                                             hthr == crash.get('thread') else '')
                   if crash else 'the last line before the run stopped')
            details.append(f'guest {kind} of 0x{gaddr:08X} on thread {thr} at {ts} ({tie})'
                           + (f': near-null, i.e. an object pointer that is 0 plus field offset 0x{gaddr:X}'
                              if near_null else ''))
        if outside:
            culprit, why = module_culprit()
            details.append(f'faulting module: {culprit}'
                           + (f' ({why})' if why else '') + '; not resolved against the exe map')
            if fault_mod and fault_mod.lower() in SDK_DLLS and fault_mod_off is not None:
                nexts.append(f'"{SYMBOLIZER}" --obj={os.path.join(exe_dir, fault_mod)} --relative-address '
                             f'0x{fault_mod_off:X}   (the SDK function that faulted; valid only while that DLL is '
                             f'the one that ran)')
            nexts.append('the SDK DLL frames in the host backtrace below show the call path; a fault in the GPU '
                         'backend or runtime is an SDK bug or bad guest data handed to it (compare with Xenia)')
        elif exe_off is not None:
            hm = get_map()
            if hm:
                name, delta = hm.lookup(exe_off)
                culprit = guest_of_symbol(name) or name
                details.append(f'map: faulting instruction wwe2k14.exe+0x{exe_off:X} = {name}+0x{delta or 0:X}')
                ours = not guest_of_symbol(name) and 'wwe2k14@@' in (name or '')
                if ours:
                    details.append('the faulting code is our own app code (namespace wwe2k14, src\\*.h), not a '
                                   'recompiled guest function')
                    gf = first_guest_frame(crash['frames'], hm) if crash else None
                    if gf:
                        details.append(f'first recompiled guest function on the host stack: {gf} (the guest code '
                                       f'that called into our hook)')
                nexts.append(f'"C:\\Program Files\\LLVM\\bin\\llvm-objdump.exe" -d '
                             f'--start-address=0x{hm.base + exe_off - 0x40:X} '
                             f'--stop-address=0x{hm.base + exe_off + 0x10:X} '
                             f'{exe}  ('
                             + ('the faulting instruction in our code' if ours else
                                'which guest register/offset faulted: PPCContext offsets, PLAYBOOK Session 3') + ')')
            else:
                details.append(f'host offset wwe2k14.exe+0x{exc_off:X} not resolved: map stale (see the map line)')
                nexts.append('reproduce on the current build and rerun triage before the next relink (the host offset '
                             'then names the faulting function); meanwhile the guest context below is map-independent')
    elif devlost_log or abort_gpu or (ev_abort and gpu_last_min and not run.shutdown):
        cls = 'device_lost'
        conf = 'high' if devlost_log else 'medium' if (abort_gpu or len(gpu_last_min) >= 3) else 'low'
        backend = run.settings.get('wwe2k14_gpu_backend', (run.backend or '?',))[0] if run.backend is None \
            else run.backend
        details.append(f'backend: {backend}; evidence: '
                       + ', '.join(x for x in (
                           'log "Failed to await submission completion" / GPU crash diagnostics' if devlost_log else '',
                           'abort() from rexgpu-xenos.dll' if abort_gpu else '',
                           f'event 1000 ucrtbase 0xc0000409 (abort without a flushed log)' if ev_abort and not crash else '',
                           f'{len(gpu_resets)} nvlddmkm 153 GPU resets in the run ({len(gpu_last_min)} in its last '
                           f'minute)' if gpu_resets else '') if x))
        d = run.devlost or {}
        if 'current' in d:
            details.append(f'device lost at frame {d["frame"]}: submission {d["current"]} current, {d["completed"]} '
                           f'completed ({d["current"] - d["completed"]} in flight){"; " + d["extra"] if d.get("extra") else ""}')
        for kind, a in d.get('faults', []):
            details.append(f'  device fault: {kind} at {a}')
        if any(k == 'write-invalid' for k, _ in d.get('faults', [])):
            details.append('  write-invalid = the GPU wrote to memory that is not bound any more (Session 3: a '
                           'freed resolve readback buffer)')
        cps = d.get('checkpoints', [])
        for stage, cid, kind in cps:
            details.append(f'  checkpoint stage {stage}: id {cid} ({kind})')
        if cps and run.dev_records:
            cid = cps[-1][1]
            before = [rec for rec in run.dev_records if rec[0] <= cid][-8:]
            after = [rec for rec in run.dev_records if rec[0] > cid]
            kinds_b = collections.Counter(k for _, k, _ in before)
            kinds_a = collections.Counter(k for _, k, _ in after)
            details.append(f'  records up to the checkpoint (last {len(before)}): {dict(kinds_b)}; after it '
                           f'(issued, not confirmed done): {dict(kinds_a)}')
            if before:
                last = before[-1]
                details.append(f'  checkpoint record: #{last[0]} {last[1]} {last[2][:150]}')
            culprit = f'GPU work after checkpoint {cid} ({cps[-1][2]})'
        rb = run.readback
        if rb['lines']:
            over = rb['over_cap']
            details.append(f'  readback buffers: map sizes {[s for s, _ in rb["reached"]]}; over the cap '
                           f'{len(over)}x' + (f' (first at frame {over[0][0]})' if over else '')
                           + (f'; original policy would have destroyed {rb["would_destroy"]} in-flight buffers'
                              if rb['would_destroy'] else ''))
            if over and over[-1][3] is None and 'frame' in d and d['frame'] - over[-1][0] <= 2:
                details.append(f'  -> eviction over the cap at frame {over[-1][0]} with the ORIGINAL policy, device '
                               f'lost at frame {d["frame"]}: the Session 3 readback-eviction pattern')
                culprit = 'resolve readback buffer evicted while in flight (Session 3 pattern)'
        if run.settings.get('vulkan_readback_evict_in_flight', ('',))[0] == 'true':
            details.append('  run used --vulkan_readback_evict_in_flight=true (the original, unsafe policy)')
        if backend == 'd3d12':
            details.append('  D3D12 run: no checkpoint dump on this backend; Xenia notes and Session 2 say use '
                           'Vulkan (default since Session 2)')
        if backend == 'd3d12':
            nexts.append('rerun on Vulkan (--wwe2k14_gpu_backend=vulkan, the default since Session 2): D3D12 has no '
                         'checkpoint dump, and Xenia\'s known-good 2K14 settings use Vulkan')
        if 'Session 3 pattern' in (culprit or ''):
            nexts.append('confirm with an A/B: the same route with --vulkan_readback_evict_in_flight=false (fixed '
                         'policy) vs =true; --vulkan_readback_max_buffers=N lowers the cap to force evictions')
        nexts.append('rerun the same route (vulkan_gpu_crash_diagnostics is an app default): compare the checkpoint '
                     'kind, fault type and readback lines with this run')
    elif crash and crash['kind'] == 'abort':
        cls, conf = 'abort_other', 'medium'
        details.append('abort() with no FATAL line and no GPU frames on the stack')
    elif exc_code is not None:
        cls, conf = 'host_exception', 'medium'
        if outside:
            culprit, why = module_culprit()
            details.append(f'exception 0x{exc_code:08X} in {culprit} (outside wwe2k14.exe{"; " + why if why else ""})'
                           + ('; 0xC000001D outside the exe is not a generated switch ud2' if exc_code == 0xC000001D
                              else ''))
        else:
            details.append(f'exception 0x{exc_code:08X} at wwe2k14.exe+0x{exc_off:X}' if exc_off is not None
                           else f'exception 0x{exc_code:08X}')
    elif run.script_failed:
        cls, conf = 'script_failure', 'high'
        ts, text = run.script_failed[-1]
        details.append(f'{ts} {text}')
        m = re.search(r'line (\d+)', text)
        shot = [s for s in run.screenshots if 'fail' in s.lower()]
        if shot:
            details.append(f'failure screenshot: {shot[-1]}')
        script = run.settings.get('wwe2k14_input_script', ('?',))[0]
        culprit = f'{os.path.basename(script)} line {m.group(1) if m else "?"}'
        nexts.append(f'look at the failure screenshot / last screenshot; rerun with '
                     f'--wwe2k14_input_script={script} once the route is fixed')
    elif not run.shutdown and os.path.exists(os.path.join(os.path.dirname(logs), 'wwe2k14.exe')) \
            and still_running(parts[-1]):
        cls, conf = 'in_progress', 'high'
        details.append('a wwe2k14.exe process is running and this log was written in the last 60 s: the run has not '
                       'ended yet; triage it again when it has')
    elif run.t_first and not run.shutdown and run.lines < 30 and run.counts['file_opens'] == 0:
        cls, conf = 'startup_error', 'high' if run.errors_first else 'medium'
        details += [f'stopped before guest code ran: {l[26:200]}' for l in run.errors_first[:3]]
    elif not run.shutdown:
        # no crash record, no shutdown: frames stopped (hang) or the process was killed / vanished
        anchor = None
        if run.frames_summary:
            anchor = parse_ts(run.frames_summary[0]) - dt.timedelta(seconds=run.frames_summary[1])
        stall = None
        if frames and frames['last_t'] is not None and anchor:
            last_frame_time = anchor + dt.timedelta(seconds=frames['last_t'])
            stall = (t_last - last_frame_time).total_seconds()
        if frames is None:
            cls, conf = 'ended_abruptly', 'low'
            details.append('no shutdown, no crash record' + ((', no event-log crash entry for this process' if not
                                                               ev_crash else '') if use_events else
                                                              ' (event log not read: --no-events)')
                           + ', and no frames CSV for this run (older than the frame log, or not written): cannot '
                             'tell a hang from a kill or an unrecorded crash')
        elif frames['frames'] == 0:
            cls, conf = 'hang', 'medium'
            details.append('no guest frames at all (frames CSV empty) and no shutdown: hang at boot or killed during '
                           'boot')
        elif stall is not None and stall > 10:
            cls, conf = 'hang', 'high' if stall > 30 else 'medium'
            details.append(f'frames stopped at frame {frames["last_frame"]} ({stall:.0f} s before the log\'s last '
                           f'line): the game stopped presenting while other threads kept logging')
        else:
            cls, conf = 'ended_abruptly', 'medium'
            details.append(f'no crash record, no shutdown lines'
                           + (', no event-log crash entry for this process' if use_events and not ev_crash else '')
                           + f'; frames kept coming until the log\'s last line (last frame '
                           f'{frames["last_frame"] if frames else "?"}'
                           f'{", CSV tail cut mid-line" if frames and frames["truncated_tail"] else ""}): killed from '
                           f'outside (run_list -WaitSeconds / run_and_triage -KillAfterWait / closed by hand) or a '
                           f'silent hang where every thread stopped logging together with the frames; run_list\'s '
                           f'KILLED line for this run tells which')
            duration = (t_last - t_first).total_seconds()
            if 700 <= duration <= 745:
                details.append(f'the run lasted {duration:.0f} s: run_list.ps1 kills a run still going after '
                               f'-WaitSeconds 720')
            step = run.script_steps[-1] if run.script_steps else None
            m = re.search(r'(waiting for .*?|wait_state .*?)\(frame (\d+)\)', step[1]) if step else None
            if m and frames and frames['last_frame'] and frames['last_frame'] - int(m.group(2)) > 600:
                cls, conf = 'script_stall', 'high' if 700 <= duration <= 745 else 'medium'
                waited = frames['last_frame'] - int(m.group(2))
                script = run.settings.get('wwe2k14_input_script', ('?',))[0]
                culprit = f'{os.path.basename(script)} {step[1].split(":", 1)[0]}'
                fps = re.search(r'([\d.]+) fps', run.frames_summary[2]) if run.frames_summary else None
                slow = fps and float(fps.group(1)) < 20
                details.append(f'the script was stuck at "{step[1][:120]}" for {waited} frames while the game kept '
                               f'presenting: ' + (f'but only at {fps.group(1)} fps at the end, so the wait may have '
                                                  f'been starved by a slowdown rather than a route desync'
                                                  if slow else 'a route desync (wrong menu / missed press), not a '
                                                               'game hang'))
                nexts.insert(0, 'look at the last screenshot of this run; give the wait a timeout so the script fails '
                                'fast (wait_file TEXT TIMEOUT / press_until_file) instead of waiting for the kill')
        if run.script_steps:
            details.append(f'last script step: {run.script_steps[-1][1][:160]}')
        elif run.script_tail:
            details.append(f'last input line: {run.script_tail[-1][1][:160]}')
        if cls != 'script_stall':
            nexts.append('rerun with the flight recorder / --wwe2k14_profile_main=true to see where the main thread '
                         'is (Session 8 task 11), or check run_list\'s KILLED line for this run')
    else:
        cls, conf = 'clean_exit', 'high'
        outcome = []
        if run.script_pass:
            outcome.append(f'PASS: {run.script_pass[-1][1][:100]}')
        if run.script_quit:
            outcome.append(f'script quit at {run.script_quit[1][:60]}')
        elif run.counts['input_lines'] == 0:
            outcome.append('no input script (window closed by hand / Alt+F4)')
        details.append('shutdown: ' + '; '.join(s for _, s in run.shutdown[:3]) + ('; ' + '; '.join(outcome) if outcome else ''))

    # ---- shared detail: host backtrace and guest context (any crash block)
    if crash:
        hm = get_map()
        details.append(f'crash block at {crash["ts"]}: {crash["kind"]}'
                       + (f' 0x{crash["code"]:08X} at wwe2k14.exe+0x{crash["offset"]:X}'
                          + (' (outside the exe, see above)' if outside else '') if crash['kind'] == 'exception'
                          else ''))
        if not hm:
            details.append('  host frames unresolved: map stale (see the map line)')
        dll_syms, dll_why = {}, []
        for mod in SDK_DLLS:
            offs = list(dict.fromkeys(o for m, o in crash['frames'] if m.lower() == mod))[:16]
            if offs:
                syms, why = symbolize_dll(exe_dir, mod, offs, t_first, map_fresh)
                dll_syms[mod] = syms
                dll_why.append(why)
        frames_s = resolve_host(crash['frames'], hm, dll_syms)
        exe_frames = [s for s in frames_s if s.lower().startswith('wwe2k14.exe')]
        details.append('  host backtrace (wwe2k14.exe frames, first 10): ' + (' | '.join(exe_frames[:10]) or 'none'))
        sdk_frames = [s for s in frames_s if s.split('+')[0].lower() in SDK_DLLS]
        if sdk_frames:
            details.append('  SDK DLL frames (first 8): ' + ' | '.join(sdk_frames[:8]) + f'  [{"; ".join(dll_why)}]')
        other = collections.Counter(s.split('+')[0] for s in frames_s if not s.lower().startswith('wwe2k14.exe')
                                    and s.split('+')[0].lower() not in SDK_DLLS)
        if other:
            details.append(f'  other modules on the stack: {dict(other)}')
        if hm and not culprit:
            culprit = first_guest_frame(crash['frames'], hm)
        g = crash.get('guest')
        if g:
            regs = g['regs']
            details.append(f'  guest thread {g["thread"]}: lr={g["lr"] or 0:08X} {idx.name(g["lr"])}, '
                           f'ctr={g["ctr"] or 0:08X} {idx.name(g["ctr"])}, '
                           f'last_indirect_target={g["lit"] or 0:08X} {idx.name(g["lit"])}')
            if g['ret']:
                details.append('  guest return addresses: ' + ' <- '.join(idx.name(a) for a in g['ret'][:12]))
                if not culprit:  # the first saved LR is in the faulting function's caller
                    culprit = idx.name(g['ret'][0]).split('+')[0] + ' (caller of the faulting function, guest stack)'
            else:
                details.append('  guest stack walk found no frames' + (' (raw words at r1 dumped)' if g['raw'] else ''))
            zero_nv = [f'r{i}' for i in range(14, 32) if regs.get(f'r{i}') == 0]
            if regs and regs.get('r31') == 0 and cls == 'guest_access_violation':
                details.append(f'  r31 = 0 (non-volatile): if r31 was valid at the function\'s entry, a callee '
                               f'returned with r31/r1 changed -> check for setjmp/longjmp or a bad function '
                               f'boundary first (PLAYBOOK Session 3 lesson); zero non-volatiles: {", ".join(zero_nv)}')
            if culprit and culprit.startswith('sub_'):
                fn_name = culprit.split()[0]  # culprit may carry a note: 'sub_X (caller of the faulting function...)'
                nexts.append(f'py tools\\analysis\\list_function.py {fn_name[4:]}')
                nexts.append(f'py tools\\q\\q.py fn {fn_name}')
            if cls == 'guest_access_violation':
                nexts.append('[[midasm_hook]] around the culprit\'s calls to log r1/r31 before/after (PLAYBOOK '
                             'Session 3 pattern), or script watch_zero on the zeroed word')

    for a in app_events:
        details.append(f'event log: {a["time"]} event 1000 {a.get("module")} {a.get("code")} offset {a.get("offset")} '
                       f'(this process: pid {a.get("pid") or "?"}, started {(a.get("pstart") or "?")[11:]}, exe time '
                       f'stamp {a.get("stamp") or "?"})')
    if run.flight_tests:
        details.append('DELIBERATE failure test (--wwe2k14_flight_test): '
                       + '; '.join(f'{t[:90]}' for _, t in run.flight_tests[-4:]))
    for ts, number, reason, path, detail in run.flight_dumps:
        details.append(f'flight recorder dump #{number} ({reason}) at {ts}: {path}  [{detail[:100]}]')
    if use_events and exc_code is not None and exe_off is not None:
        # Same exception at the same exe offset in other processes. The build of this run: its own event's PE time
        # stamp, else the exe on disk when nothing was relinked since the run started.
        run_stamp = None
        if ev_own is not None and ev_own.get('stamp'):
            run_stamp = int(ev_own['stamp'], 16)
        elif map_fresh and not map_override and exe_stamp is not None:
            run_stamp = exe_stamp
        own = {(a.get('pid'), a.get('pstart')) for a in app_events}
        same = [a for a in all_app_events()['app'] if (a.get('module') or '').lower() == 'wwe2k14.exe'
                and int(a.get('code') or '0', 16) == exc_code and int(a.get('offset') or '0', 16) == exe_off
                and (a.get('pid'), a.get('pstart')) not in own]

        def build(a):
            if not a.get('stamp') or run_stamp is None:
                return 'build ?'
            return 'same build' if int(a['stamp'], 16) == run_stamp else 'other build'
        if same:
            n_same = sum(build(a) == 'same build' for a in same)
            details.append(f'same signature (0x{exc_code:08X} at +0x{exe_off:X}) in {len(same)} other crash(es) in '
                           f'the event log, {n_same} of them the same build (exe time stamp '
                           f'{f"0x{run_stamp:08x}" if run_stamp is not None else "unknown"}): '
                           + ', '.join(f'{a["time"][:19]} ({build(a)})' for a in same[:8])
                           + (' ...' if len(same) > 8 else ''))
            if n_same < len(same):
                details.append('  an equal offset in another build is a strong hint, not proof: app-only relinks have '
                               'kept generated code in place (Session 3), but a relink can also move the same crash '
                               'to another offset, so this list can miss runs (the guest-side signature below does '
                               'not depend on the build)')
    if crash and crash['kind'] == 'exception' and num is not None:
        own_sig = guest_signature(crash['code'], f'{gav[1]} 0x{gav[2]:08X}' if gav else None, crash.get('guest'))
        if own_sig:
            sigs = scan_crash_signatures(logs)
            twins = sorted(n for n, ss in sigs.items() if n != num and own_sig in ss)
            desc = (f'0x{own_sig[0]:08X}' + (f', guest {own_sig[1]}' if own_sig[1] else '')
                    + f', lr={own_sig[2]:08X} ctr={own_sig[3]:08X} lit={own_sig[4]:08X}'
                    + (f', return chain {" ".join(f"{a:08X}" for a in own_sig[5])}' if own_sig[5] else ''))
            details.append(f'same guest-side crash signature ({desc}; map-independent, survives relinks) in '
                           + (f'{len(twins)} other run(s) of this log folder: '
                              + ', '.join(f'{n:03d}' for n in twins[:20]) + (' ...' if len(twins) > 20 else '')
                              if twins else 'no other run of this log folder'))

    # ---- secondary observations
    if gpu_resets and cls != 'device_lost':
        secondary.append(f'{len(gpu_resets)} nvlddmkm 153 GPU resets during the run ({len(gpu_last_min)} in its last '
                         f'minute){" (D3D12 era: the GPU faults of Session 2)" if run.backend == "d3d12" else ""}')
    if run.readback['over_cap'] and cls != 'device_lost':
        rb = run.readback
        secondary.append(f'readback buffers went over the cap {len(rb["over_cap"])}x without a device loss'
                         + (f' (the fixed policy avoided {rb["would_destroy"]} in-flight destructions)'
                            if rb['would_destroy'] else ''))
    loose_av = [av for av in run.guest_av if av is not gav]
    if loose_av:
        secondary.append(f'{len(loose_av)} logged guest access violation(s) not tied to the end of the run (fault-safe '
                         f'probe reads such as --wwe2k14_det_hash / --wwe2k14_peek log them and carry on; first: '
                         f'{loose_av[0][1]} of 0x{loose_av[0][2]:08X} at {loose_av[0][0]}, last at {loose_av[-1][0]})')
    near_other = [a for a in other_events if t_first and t_first - dt.timedelta(seconds=5) <= a['t']
                  <= t_last + dt.timedelta(seconds=90)]
    if near_other:
        one = len(near_other) == 1
        secondary.append(f'{len(near_other)} event-1000 entr{"y" if one else "ies"} near this run '
                         f'belong{"s" if one else ""} to other processes and {"was" if one else "were"} not used: '
                         + '; '.join(f'{a["time"][:19]} {a.get("module")} {a.get("code")} (process started '
                                     f'{(a.get("pstart") or "?")[11:19]}, {os.path.basename(os.path.dirname(a.get("path") or "?"))}'
                                     f'\\{os.path.basename(a.get("path") or "?")})' for a in near_other[:4]))
    if run.script_failed and cls != 'script_failure':
        secondary.append(f'script also failed: {run.script_failed[-1][1][:120]}')
    if run.counts['ppc_trap']:
        secondary.append(f'{run.counts["ppc_trap"]} PPC tw/td trap lines (logged and continued; not fatal)')
    if run.counts['draw_failed_in_backend']:
        secondary.append(f'{run.counts["draw_failed_in_backend"]} draws "Failed in backend", '
                         f'{run.counts["resolve_region_empty"]} "Resolve region is empty"')
    if app_events and not run.shutdown and cls in ('clean_exit', 'script_failure', 'hang', 'ended_abruptly',
                                                   'script_stall'):
        secondary.append(f'event log has {len(app_events)} error event(s) for this process: '
                         + '; '.join(f'{a["time"]} {a["module"]} {a["code"]}' for a in app_events))
    if run.dump_ranges:
        recipe_dumps(run, idx, image, details)
    if switch_fn:
        details.append(f'--switch {switch_fn} (live [dump] words of this run where they cover the tables, else the '
                       f'image dump):')
        recipe_switch(switch_fn, idx, image, run, details)
    flight = os.path.join(logs, f'flight_{num:03d}.txt') if num is not None else None
    if flight and os.path.exists(flight) and not run.flight_dumps:
        secondary.append(f'flight recorder dump: {flight}')
    if ev.get('error') and use_events:
        secondary.append(f'event log not read: {ev["error"]}')
    if idx.ok and idx.stale:
        secondary += idx.stale
    if culprit and culprit.startswith('sub_'):
        a = int(culprit[4:12], 16)
        if a in dbe:
            name, c, file = dbe[a]
            details.append(f'db: {culprit} = {name} ({c}, db\\{file})')

    last_file = run.files[-1] if run.files else None
    return {
        'run': num, 'log': parts[-1], 'parts': len(parts), 'lines': run.lines,
        'start': run.t_first, 'end': run.t_last,
        'class': cls, 'confidence': conf, 'culprit': culprit,
        'next': list(dict.fromkeys(nexts)), 'details': details, 'secondary': secondary,
        'map': {'path': mp, 'fresh': map_fresh, 'reason': map_reason},
        'frames': frames, 'frames_summary': run.frames_summary[2] if run.frames_summary else None,
        'backend': run.backend,
        'last_file': f'{last_file[0]} {last_file[2]}' if last_file else None,
        'events': {'app': [{k: v for k, v in a.items() if k != 't'} for a in app_events],
                   'gpu_resets': len(gpu_resets), 'gpu_resets_last_min': len(gpu_last_min)},
        'script': run.settings.get('wwe2k14_input_script', (None,))[0],
    }


def print_report(v, brief=False):
    head = (f'VERDICT run {v["run"]:03d}: {v["class"]} (confidence {v["confidence"]})' if v['run'] is not None
            else f'VERDICT {v["log"]}: {v["class"]} (confidence {v["confidence"]})')
    print(head)
    print(f'  culprit: {v["culprit"] or "unknown"}')
    if v['next']:
        print(f'  next:    {v["next"][0]}')
        for n in v['next'][1:]:
            print(f'           {n}')
    if brief:
        return
    print(f'-- details ({v["parts"]} log part(s), {v["lines"]} lines, {v["start"]} .. {v["end"]}, '
          f'backend {v["backend"] or "?"})')
    for d in v['details']:
        print(f'  {d}')
    fr = v['frames']
    if fr:
        print(f'  frames CSV: {fr["frames"]} frames, last frame {fr["last_frame"]} at t={fr["last_t"]} s'
              f'{" (tail cut mid-line)" if fr["truncated_tail"] else ""}; longest frame '
              f'{fr.get("worst_ms", 0) / 1000:.2f} s (frame {fr.get("worst_frame")}), {fr.get("stalls_5s", 0)} '
              f'frame(s) >= 5 s')
    if v.get('frames_summary'):
        print(f'  last [frames] summary: {v["frames_summary"][:150]}')
    if v['last_file']:
        print(f'  last file opened: {v["last_file"]}')
    print(f'  map: {v["map"]["reason"]}')
    if v['secondary']:
        print('-- also seen')
        for s in v['secondary']:
            print(f'  {s}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('target', help='run number (30, 030, wwe2k14_030) or a log path')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--brief', action='store_true', help='verdict lines only')
    ap.add_argument('--no-events', action='store_true', help='do not read the Windows event log')
    ap.add_argument('--exe-dir', default=EXE_DIR)
    ap.add_argument('--map', help='map of the build that ran (trusted as-is)')
    ap.add_argument('--save', action='store_true', help='also write out\\harness\\triage\\run_NNN.json')
    ap.add_argument('--switch', metavar='sub_XXXXXXXX',
                    help='also run the switch recipe on this function, using this run\'s [dump] words '
                         '(e.g. on the --wwe2k14_dump_words run that a switch_out_of_range verdict suggested)')
    a = ap.parse_args()
    v = triage(a.target, a.exe_dir, not a.no_events, a.map, switch_fn=a.switch)
    if a.save:
        d = os.path.join(REPO, 'out', 'harness', 'triage')
        os.makedirs(d, exist_ok=True)
        name = f'run_{v["run"]:03d}.json' if v['run'] is not None else \
            'log_' + re.sub(r'\W+', '_', os.path.basename(v['log'])) + '.json'
        with open(os.path.join(d, name), 'w', encoding='utf-8') as f:
            json.dump(v, f, indent=1, default=str)
    if a.json:
        print(json.dumps(v, indent=1, default=str))
    else:
        print_report(v, a.brief)


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(errors='replace')
    except AttributeError:
        pass
    main()
