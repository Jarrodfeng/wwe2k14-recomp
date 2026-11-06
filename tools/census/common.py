"""Session 92 census: shared code - one game per item (live channel), waits on the event bus, the owned-frame log
parser, the phase of every guest frame, and the item verdict. Not run by itself; used by run.py / sheet.py.

What it answers: "how do I start one game for one item, wait for things with a timeout, close it for sure, and turn
its log + events + pictures into a result?" Every wait here has a deadline; a dead game raises Crashed, a wait that
runs out raises Stuck; run.py turns both into a result and the sweep goes on.
"""
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import traceback
from bisect import bisect_right

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
for sub in ('live', 'atlas', 'census'):
    sys.path.insert(0, os.path.join(REPO, 'tools', sub))
import events as evlib  # noqa: E402
import live  # noqa: E402
import match  # noqa: E402
import nav  # noqa: E402
import detect  # noqa: E402

CEN = os.path.join(REPO, 'out', 'census')
FLAG_OVERRIDES = []  # run.py --flags "--a=1 --b=2": replace / add these on every game start (Session 92)
KEEP_SHADERS = ''  # run.py --keep-shaders: every game's shader storage copied into <pass>\shaders\ when it ends (Session 92)
MAIN_EXE = os.path.join(REPO, 'out', 'build', 'win-amd64-release', 'wwe2k14.exe')
SETTINGS_SRC = os.path.join(REPO, 'out', 'owned_check', 'settings_native1440.json')
DATA = 'userdata_unlocked_s91'
MIN_FREE_GB = 6.0


class Crashed(Exception):
    pass


class Stuck(Exception):
    pass


class Unbuildable(Exception):
    pass


def now():
    return time.strftime('%Y-%m-%d %H:%M:%S')


def write_json(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(obj, f, indent=1)
    os.replace(tmp, path)


# ---------------------------------------------------------------- processes

def game_procs(name):
    import psutil
    out = []
    token = '--wwe2k14_live=%s' % name
    for p in psutil.process_iter(['name', 'cmdline']):
        try:
            if (p.info['name'] or '').lower() == 'wwe2k14.exe' and token in (p.info['cmdline'] or []):
                out.append(p)
        except Exception:
            pass
    return out


def kill_game(name):
    n = 0
    for p in game_procs(name):
        try:
            p.kill()
            n += 1
        except Exception:
            pass
    # Session 92: also the run_list wrapper (pwsh) still queued for the run lock with this live name - a start that
    # timed out while queued left it waiting, it launched a stray game later, and retries piled up 13 of them (04:1x).
    import psutil
    token = '--wwe2k14_live=%s' % name
    for p in psutil.process_iter(['name', 'cmdline']):
        try:
            if (p.info['name'] or '').lower() in ('pwsh.exe', 'powershell.exe') and \
                    any(token == a or a.endswith(token) or (token + ' ') in a or a.endswith(token + "'")
                        for a in (p.info['cmdline'] or [])):
                p.kill()
        except Exception:
            pass
    return n


def free_ram_gb():
    import psutil
    return psutil.virtual_memory().available / 2 ** 30


def wait_for_ram(say, max_wait=900):
    t0 = time.time()
    while free_ram_gb() < MIN_FREE_GB:
        if time.time() - t0 > max_wait:
            return False
        say('free RAM %.1f GB < %.0f: waiting' % (free_ram_gb(), MIN_FREE_GB))
        time.sleep(30)
    return True


# ---------------------------------------------------------------- phases

class PhaseTracker:
    """The phase of every guest frame from the event bus: boot / menus / other / load / entrance / match."""

    def __init__(self):
        self.seq = None
        self.menu_seen = False
        self.entrance = False
        self.clock = False
        self.marks = [(-1, 'boot')]
        self.entrance_slots = {}
        self.first = {}

    @property
    def phase(self):
        return self.marks[-1][1]

    def _compute(self):
        if self.seq == 9:
            return 'match' if self.clock else ('entrance' if self.entrance else 'load')
        if self.seq == 75:
            return 'menus'
        if not self.menu_seen:
            return 'boot'
        return 'other'

    def feed(self, e):
        k, f = e.get('kind'), e.get('f', 0)
        if k == 'seq':
            self.seq = e.get('seq')
            if self.seq == 75:
                self.menu_seen = True
            if self.seq == 9:
                self.entrance = self.clock = False
        elif k == 'entrance':
            if self.clock or not (e.get('step') or 0) > 0:
                return  # step 0 is noise (loads, the skip); steps after the bell are the ring
            self.entrance = True
            self.entrance_slots.setdefault(e.get('slot'), []).append(e.get('step'))
            self.first.setdefault('entrance', f)
        elif k == 'match' and e.get('what') == 'clock_started':
            self.clock = True
            self.first.setdefault('clock_started', f)
        else:
            return
        ph = self._compute()
        if ph != self.marks[-1][1]:
            self.marks.append((f, ph))

    def at(self, frame):
        i = bisect_right([m[0] for m in self.marks], frame) - 1
        return self.marks[max(i, 0)][1]

    def window_phase(self, a, b):
        votes = {}
        for k in range(5):
            ph = self.at(a + (b - a) * k // 4)
            votes[ph] = votes.get(ph, 0) + 1
        return max(votes.items(), key=lambda kv: (kv[1], kv[0] != 'load'))[0]


# ---------------------------------------------------------------- the log

WIN = re.compile(r'\[native\]\[owned-frame\] frames (\d+)-(\d+): (.*)$')
SUMMARY = re.compile(r'^passes (\d+) owned (\d+) fallback (\d+)(?: \(by reason: (.*?)\))?, copies served')
NOPIPE = re.compile(r'no pipeline (\d+)')
TRUTH = re.compile(r'TRUTH WORK per frame: passes recorded ([\d.]+)')
SIG = re.compile(r'^\s+(\d+) x (.+?) (?:- )?surf (\d+)/(\d+)/(\d+) box (\S+) draws (\d+) vs (\S+) ps (\S+)')
LOST = re.compile(r'frame (\d+): a record clear was LOST')
COPYLOSS = re.compile(r'copy losses by source line: (.*?);')
BAD = re.compile(r'\] \[(error|critical)\] ')
CRASH = ('[wwe2k14][crash]', 'VK_ERROR_DEVICE_LOST', 'DEVICE_LOST', 'device lost')
# Session 92 M0 (SDK free_counters.h): what of the 360 machinery still runs under TRUE NATIVE, one line per window.
FREE = re.compile(r'\[d3d\]\[free\] frames (\d+)-(\d+): (.*)$')
FREE_FIELDS = (('truth_entries', r'truth entries (\d+)'), ('translator_runs', r'translator runs (\d+)'),
               ('packets', r'packets executed (\d+)'), ('register_writes', r'register writes (\d+)'),
               ('wait_reg_mem', r'wait_reg_mem (\d+)'), ('answers_walks', r'answers-side walks (\d+)'),
               ('clears_library_way', r"record clears drawn the library's way (\d+)"),
               ('direct_waits', r'direct guest-word waits (\d+)'),  # Session 92 M5a (gpu_d3d_sync_direct)
               ('resolves_regs', r'resolves via IssueCopy registers (\d+)'), ('cache_resolves', r'cache resolves (\d+)'),
               ('presents_packet', r'presents as packets (\d+)'), ('swap_fetch_regs', r'swap fetch from registers (\d+)'),
               ('fetch0_stale', r'unwritten since the present (\d+)'))  # Session 92 M3
FREE_SHADER = re.compile(r'\b(VS|PS) 0x([0-9A-Fa-f]{16})')
# Session 92 M4f: the typed front end's share of the token draws (SDK d3d_direct.cpp window line), and why the rest went to the old path.
TYPED = re.compile(r'\[d3d\]\[front\] M4f-A frames \d+-\d+: token draw items offered (\d+), drawn by the typed front end (\d+)'
                   r'.*?handed to the old path \d+ by reason: (.*?);')
TYPED_REASON = re.compile(r'\s*(.+?) (\d+)$')
# Session 92 M4f-B: the other draw kinds typed (appended to the M4f-A line) and the pending clears (B2's own line).
TYPED_B = re.compile(r'M4f-B typed: crowd members (\d+) \(declined to the old way (\d+)\), inline draws (\d+) \(handed to the old path (\d+)\), '
                     r'cover rectangles (\d+), bounds rectangles (\d+), memexport draws (\d+)')
TYPED_B_KEYS = ('crowd_members', 'crowd_declined', 'inline_typed', 'inline_old', 'cover_rects', 'bounds_rects', 'memexport_typed')
PEND = re.compile(r'\[native\]\[pend\] frames \d+-\d+: (.*)$')
PEND_FIELDS = (('pend_made', r'pending clears made (\d+)'), ('pend_consumed_start', r'pass start \(held again before a draw\) (\d+)'),
               ('pend_depth_given', r'depth given to a colour-only pass (\d+)'), ('pend_copy_filled', r'copy filled (\d+)'),
               ('pend_superseded', r'by a newer clear (\d+)'), ('pend_unconsumed', r'unconsumed at frame end (\d+)'),
               ('pend_unservable', r'unservable \(held again and refused\) (\d+)'), ('pend_retried', r'held again and dropped again (\d+)'),
               ('pend_refusals', r'D3dClear refusals pended (\d+)'), ('pend_unpendable', r'\(unpendable (\d+)'))
M6_VARIANT = '[m6][variant] '  # Session 92 M6 groundwork: one line per translation variant a run made (tools\shaders\m6_variants.py)
# Session 92 M4f-C (the cut): the proof line per window. The FIRST cut window holds the boot (the latch frame, the drops before the owned
# frame) - its numbers are kept apart ('cut_boot_*'); every later window must read 0 drops / refusals / readers / poison rewrites / segments.
CUT = re.compile(r'\[d3d\]\[cut\] frames \d+-\d+: (.*)$')
CUT_NUM = (('cut_items_packet', r'executor items (\d+) packet'), ('cut_records_as_packets', r'records executed as packets after the latch (\d+)'),
           ('cut_segments_after', r'segment pieces after the latch (\d+)'), ('cut_poison_rewritten', r'poisoned registers rewritten (\d+)'),
           ('cut_entry_failed', r'entry calls failed (\d+)'), ('cut_flips_ignored', r'owned-frame switch-offs ignored (\d+)'),
           ('cut_dropped_before_owned', r'dropped before the owned frame (\d+)'))
CUT_LIST = (('cut_drops', r'draws / records dropped by reason: (.*?)(?:; draws skipped|; detector|$)'),
            ('cut_skips', r'draws skipped by a setting or drawn in a counted special way \(not drops\): (.*?)(?:; detector|$)'),
            ('cut_refused', r'packet items refused and DROPPED[^:]*: (.*?);'),
            ('cut_readers', r'register readers called after the latch: (.*?)$'),
            ('cut_type0_bases', r'type-0 base(.*?)(?:;|$)'))
CUT_LIST_ITEM = re.compile(r'([^,;]+?) x?(\d+)(?: \(ret [^)]*\))?$')
CUT_READER = re.compile(r'([A-Za-z_][\w:]*(?:\(\))?) (\d+)')
CUT_US = re.compile(r'executor ([\d.]+) us a frame')
CUT_ARMED = re.compile(r'detector 2 armed (yes|no)')
TYPED_US = re.compile(r'typed prepare ([\d.]+) us a frame, whole typed draw ([\d.]+) us a frame')
MMIO = re.compile(r'guest MMIO reads by register[^:]*: (.*?)(?:;|$)')
NOPIPE_OWNED = re.compile(r'\[native\]\[owned-only\] frames \d+-\d+: latch on;.*?skipped for no pipeline (\d+)')
CROWD_REFUSED = re.compile(r'\[scene\]\[crowd\] batch refused at frame \d+: reason (\d+)')


def _cut_list(text):
    """'a x3, b 5, none' -> {'a': 3, 'b': 5}"""
    out = {}
    text = text.strip()
    if not text or text.startswith('none'):
        return out
    for part in text.split(', '):
        m = CUT_LIST_ITEM.match(part.strip())
        if m:
            out[m.group(1).strip()] = out.get(m.group(1).strip(), 0) + int(m.group(2))
    return out
GPUTAG = ('[gpu]', '[d3d]', '[native]', '[wwe2k14]')


def parse_log(path):
    """-> {'windows': [...], 'signatures': {key: {...}}, 'lost': [frames], 'errors': [...], 'crash': [...],
    'build': [...], 'copy_losses': [...]}; signatures per window are kept under windows[i]['sigs']."""
    out = {'windows': [], 'lost': [], 'errors': [], 'crash': [], 'build': [], 'copy_losses': [], 'lines': 0,
           'free': None}
    if not path or not os.path.exists(path):
        return out
    free = {k: 0 for k, _ in FREE_FIELDS}
    free.update({'windows': 0, 'new_shaders': [], 'typed_offered': 0, 'typed_drawn': 0, 'typed_reasons': {}, 'm6_variants': 0})
    free.update({k: 0 for k in TYPED_B_KEYS})
    free.update({k: 0 for k, _ in PEND_FIELDS})
    free['pend_windows'] = 0
    free.update({'cut_windows': 0, 'cut_armed': None, 'cut_us': [], 'typed_us': [], 'mmio': {}, 'nopipe_owned': 0, 'crowd_refused': {}})
    free.update({k: 0 for k, _ in CUT_NUM})
    free.update({k: {} for k, _ in CUT_LIST})
    free['cut_boot'] = {}
    cur_sigs = None
    err_seen = set()
    wins = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            out['lines'] += 1
            if cur_sigs is not None:
                m = SIG.match(line)
                if m:
                    reason = re.split(r' \(', m.group(2), 1)[0]
                    reason = re.sub(r'\s+start( cleared)?\s*$', '', reason)
                    cur_sigs.append({'count': int(m.group(1)), 'reason': reason, 'detail': m.group(2),
                                     'surface': '%s/%s' % (m.group(4), m.group(5)),
                                     'box': m.group(6), 'draws': int(m.group(7)), 'vs': m.group(8), 'ps': m.group(9)})
                    continue
                cur_sigs = None
            m = FREE.search(line)
            if m:
                rest = m.group(3)
                free['windows'] += 1
                for k, rx in FREE_FIELDS:
                    v = re.search(rx, rest)
                    if v:
                        free[k] += int(v.group(1))
                for kind, h in FREE_SHADER.findall(rest):
                    tag = '%s %s' % (kind, h.upper())
                    if tag not in free['new_shaders']:
                        free['new_shaders'].append(tag)
                continue
            m = TYPED.search(line)
            if m:
                free['typed_offered'] += int(m.group(1))
                free['typed_drawn'] += int(m.group(2))
                if m.group(3).strip() != 'none':
                    for part in m.group(3).split(', '):
                        r = TYPED_REASON.match(part)
                        if r:
                            free['typed_reasons'][r.group(1)] = free['typed_reasons'].get(r.group(1), 0) + int(r.group(2))
                b = TYPED_B.search(line)
                if b:
                    for k, v in zip(TYPED_B_KEYS, b.groups()):
                        free[k] += int(v)
                u = TYPED_US.search(line)
                if u:
                    free['typed_us'].append(float(u.group(2)))
                continue
            m = CUT.search(line)
            if m:
                rest = m.group(1)
                free['cut_windows'] += 1
                boot = free['cut_windows'] == 1  # (the latch frame's window: boot drops, the first poison fill)
                tgt = free['cut_boot'] if boot else free
                for k, rx in CUT_NUM:
                    v = re.search(rx, rest)
                    if v:
                        tgt[k] = tgt.get(k, 0) + int(v.group(1))
                for k, rx in CUT_LIST:
                    v = re.search(rx, rest)
                    if v:
                        d = tgt.setdefault(k, {})
                        # (the readers print as "DrawRegs() 0 TextureCache::register_file() 0 ...", space-separated)
                        items = ({n: int(c) for n, c in CUT_READER.findall(v.group(1))} if k == 'cut_readers'
                                 else _cut_list(v.group(1)))
                        for name, n in items.items():
                            d[name] = d.get(name, 0) + n
                v = CUT_ARMED.search(rest)
                if v:
                    free['cut_armed'] = v.group(1) if free['cut_armed'] in (None, 'yes') else free['cut_armed']
                v = CUT_US.search(rest)
                if v and not boot:
                    free['cut_us'].append(float(v.group(1)))
                continue
            m = MMIO.search(line)
            if m:
                for name, n in _cut_list(m.group(1)).items():
                    free['mmio'][name] = free['mmio'].get(name, 0) + n
            m = NOPIPE_OWNED.search(line)
            if m:
                free['nopipe_owned'] += int(m.group(1))
            m = CROWD_REFUSED.search(line)
            if m:
                free['crowd_refused'][m.group(1)] = free['crowd_refused'].get(m.group(1), 0) + 1
                continue
            m = PEND.search(line)
            if m:
                free['pend_windows'] += 1
                for k, rx in PEND_FIELDS:
                    v = re.search(rx, m.group(1))
                    if v:
                        free[k] += int(v.group(1))
                continue
            if M6_VARIANT in line:
                free['m6_variants'] += 1
                continue
            if '[wwe2k14][build]' in line and len(out['build']) < 30:
                out['build'].append(line.rstrip().split('[wwe2k14][build] ', 1)[-1][:300])
            m = WIN.search(line)
            if m:
                a, b, rest = int(m.group(1)), int(m.group(2)), m.group(3)
                w = wins.setdefault((a, b), {'a': a, 'b': b, 'sigs': [], 'passes': 0, 'owned': 0, 'fallback': 0,
                                             'reasons': '', 'no_pipeline': 0, 'truth_passes': None})
                s = SUMMARY.match(rest)
                if s:
                    w['passes'], w['owned'], w['fallback'] = int(s.group(1)), int(s.group(2)), int(s.group(3))
                    w['reasons'] = s.group(4) or ''
                    p = NOPIPE.search(rest)
                    w['no_pipeline'] = int(p.group(1)) if p else 0
                    t = TRUTH.search(rest)
                    w['truth_passes'] = float(t.group(1)) if t else None
                elif rest.startswith('fallback passes by signature'):
                    cur_sigs = w['sigs']
                elif 'copy losses by source line:' in rest:
                    c = COPYLOSS.search(rest)
                    if c and c.group(1).strip() != 'none':
                        out['copy_losses'].append({'a': a, 'b': b, 'losses': c.group(1)[:300]})
                continue
            m = LOST.search(line)
            if m:
                out['lost'].append(int(m.group(1)))
                continue
            if any(c in line for c in CRASH):
                if len(out['crash']) < 10:
                    out['crash'].append(line.rstrip()[:300])
            if BAD.search(line) and any(t in line for t in GPUTAG):
                key = re.sub(r'^\[[^\]]*\] ', '', line.rstrip())[:200]
                key = re.sub(r'\d+', 'N', key)
                if key not in err_seen:
                    err_seen.add(key)
                    if len(out['errors']) < 20:
                        out['errors'].append(line.rstrip()[:300])
    out['windows'] = [wins[k] for k in sorted(wins)]
    out['free'] = free if (free['windows'] or free['typed_offered'] or free['pend_windows'] or free['m6_variants'] or
                           free['cut_windows']) else None
    return out


COUNT = {  # which phases count for the item verdict
    'roster': ('entrance', 'match'), 'arena': ('entrance', 'match'), 'type': ('entrance', 'match'),
    'wm30': ('entrance', 'match'), 'mode': ('menus', 'other', 'entrance', 'match'),
}


def analyse(item, log, tracker):
    """Windows with phases, the counted totals and the signatures -> dict for the result."""
    count_phases = COUNT.get(item['family'], ('entrance', 'match'))
    sigs = {}
    tot = {'windows': 0, 'passes': 0, 'owned': 0, 'fallback': 0, 'no_pipeline': 0, 'lost_clears': 0}
    by_phase = {}
    for w in log['windows']:
        w['phase'] = tracker.window_phase(w['a'], w['b'])
        bp = by_phase.setdefault(w['phase'], {'windows': 0, 'passes': 0, 'fallback': 0})
        bp['windows'] += 1
        bp['passes'] += w['passes']
        bp['fallback'] += w['fallback']
        if w['phase'] in count_phases:
            tot['windows'] += 1
            for k in ('passes', 'owned', 'fallback', 'no_pipeline'):
                tot[k] += w[k]
            for s in w['sigs']:
                key = '%s | surf %s | box %s | vs %s ps %s' % (s['reason'], s['surface'], s['box'], s['vs'], s['ps'])
                d = sigs.setdefault(key, {'key': key, 'count': 0, 'phases': {}, 'detail': s['detail'][:200]})
                d['count'] += s['count']
                d['phases'][w['phase']] = d['phases'].get(w['phase'], 0) + s['count']
    lost_counted = [f for f in log['lost'] if tracker.at(f) in count_phases]
    tot['lost_clears'] = len(lost_counted)
    for w in log['windows']:
        w['sigs'] = w['sigs'][:8]
    return {'totals': tot, 'by_phase': by_phase, 'signatures': sorted(sigs.values(), key=lambda d: -d['count']),
            'lost_clear_frames': lost_counted[:20], 'lost_clears_all': len(log['lost'])}


def verdict_of(tot, status):
    if status != 'ok':
        return status
    if tot['windows'] == 0:
        return 'nodata'
    return 'owned' if tot['fallback'] == 0 and tot['lost_clears'] == 0 else 'handback'


# ---------------------------------------------------------------- one game

class Game:
    """One game for one item: start, drive through the live channel, close for sure."""

    def __init__(self, name, item, exe, say, atlas=None, pass_dir=None):
        self.name, self.item, self.say = name, item, say
        self.exe = exe or MAIN_EXE
        self.exe_dir = os.path.dirname(self.exe)
        self.logs = os.path.join(self.exe_dir, 'logs')
        self.caps = os.path.join(self.exe_dir, 'captures')
        self.deadline = time.time() + item.get('timeout_s', 1800)
        self.run = None
        self.ev = None
        self.events = []
        self.tr = PhaseTracker()
        self.shots = []
        self.pairs = []
        self.nav = nav.Nav(name, atlas=atlas)
        self.watchdog = None
        self.watchdog_fired = False

    # -- time
    def left(self):
        return self.deadline - time.time()

    def _fire(self):
        self.watchdog_fired = True
        self.say('watchdog: item deadline + grace passed, killing the game')
        kill_game(self.name)

    # -- start / close
    def start(self, extra_flags=(), script='menu', fast=True, wait_menu=True, boot_wait=300):
        kill_game(self.name)
        if not wait_for_ram(self.say):
            raise Crashed('not enough free RAM to start')
        settings = os.path.join(CEN, 'settings_%s.json' % self.name)
        os.makedirs(CEN, exist_ok=True)
        shutil.copyfile(SETTINGS_SRC, settings)
        flags = ['--wwe2k14_d3d_owned=true', '--wwe2k14_native_frame=true', '--wwe2k14_use_launcher_settings=true',
                 '--wwe2k14_settings_file=%s' % settings, '--wwe2k14_dlc=true',
                 '--wwe2k14_replay_dir=%s' % os.path.join(REPO, 'out', 'replays'),
                 '--wwe2k14_scenario_dir=%s' % os.path.join(REPO, 'out', 'scenarios')]
        main = os.path.normcase(os.path.abspath(self.exe)) == os.path.normcase(os.path.abspath(MAIN_EXE))
        if not main:
            flags.append('--game_data_root=%s' % os.path.join(REPO, 'game_data', 'extracted'))
        flags += list(self.item.get('flags', [])) + list(extra_flags)
        # Session 92: run.py --flags (a switch A/B over census items): each override replaces any flag of the same name
        # above, so no flag is ever passed twice (CLI11 drops every later startup flag then).
        for ov in FLAG_OVERRIDES:
            key = ov.split('=', 1)[0] + '='
            flags = [f for f in flags if not f.startswith(key)] + [ov]
        self.start_flags = list(flags)  # (Session 92: feature_proof.check expects every one of these switches to have engaged)
        self.watchdog = threading.Timer(self.item.get('timeout_s', 1800) + 120, self._fire)
        self.watchdog.daemon = True
        self.watchdog.start()
        txt = ''
        for attempt in range(3):
            txt = live.start(self.name, script=script, fast=fast, extra=' '.join(flags), data=DATA,
                             exe='' if main else self.exe, boot_wait=boot_wait, keep_shaders=KEEP_SHADERS)
            if txt.startswith('started:'):
                break
            self.say('start attempt %d: %s' % (attempt + 1, txt[:200]))
            kill_game(self.name)
            time.sleep(20)
        else:
            raise Crashed('the game did not start: %s' % txt[:200])
        fields, _ = live.parse(live.request(self.name, '!status'))
        self.run = int(fields.get('run'))
        self.ev = evlib.Events(self.run)
        self.ev.path = os.path.join(self.logs, 'events_%03d.jsonl' % self.run)
        self.say('started run %s' % self.run)
        if wait_menu:
            st, _, failed = self.batch(['wait_state menu 1 3600 30'], timeout=600)
            if failed:
                raise Stuck('the main menu never came up: %s' % failed)

    def close(self):
        """Quit the game and make sure it is gone. -> True if it left by itself."""
        clean = True
        try:
            live.request(self.name, 'quit', connect_wait=2.0)
        except Exception:
            pass
        t0 = time.time()
        while time.time() - t0 < 60 and game_procs(self.name):
            time.sleep(1)
        if game_procs(self.name):
            clean = False
            kill_game(self.name)
            time.sleep(2)
        if self.watchdog:
            self.watchdog.cancel()
        return clean

    # -- driving
    def batch(self, lines, timeout=120):
        if self.left() <= 0:
            raise Stuck('item deadline passed')
        try:
            return self.nav.batch(lines, timeout=int(max(10, min(timeout, self.left() + 30))))
        except live.NoGame as x:
            raise Crashed('live channel gone: %s' % x)
        except nav.Lost as x:
            if not game_procs(self.name):
                raise Crashed('game gone: %s' % x)
            raise Stuck('batch not ok: %s' % x)
        except OSError as x:
            raise Crashed('pipe error: %s' % x)

    def pump(self):
        new = self.ev.read() if self.ev else []
        for e in new:
            self.events.append(e)
            self.tr.feed(e)
        return new

    def wait_for(self, pred, timeout_s, poll=0.25):
        """First event matching pred (dict / callable) within timeout_s; None on timeout. Raises Crashed if the game dies."""
        test = pred if callable(pred) else (lambda e: all(e.get(k) == v for k, v in pred.items()))
        end = min(time.time() + timeout_s, self.deadline)
        last_alive = time.time()
        while time.time() < end:
            for e in self.pump():
                if test(e):
                    return e
            if time.time() - last_alive > 5:
                last_alive = time.time()
                if not game_procs(self.name):
                    self.pump()
                    raise Crashed('the game process is gone')
            time.sleep(poll)
        return None

    def shot(self, tag=''):
        """Queue-free screenshot now; returns its record."""
        self.pump()
        k = len(self.shots) + 1
        label = 'cen_%s_%03d' % (self.item['id'], k)
        self.batch(['screenshot %s' % label], timeout=60)
        rec = {'label': '%s%s' % (tag, '') if tag else label, 'file': label, 'phase': self.tr.phase,
               'path': os.path.join(self.caps, '%03d_%s.png' % (self.run, label))}
        self.shots.append(rec)
        return rec

    def pair(self, tag='flicker'):
        """Two shots 2 guest frames apart (flicker)."""
        a = self.shot(tag + '_a')
        self.batch(['wait_frames 2'], timeout=30)
        b = self.shot(tag + '_b')
        self.pairs.append((a['file'], b['file']))

    # -- matches
    def build_match(self, spec):
        try:
            self.nav.go('list:1')
        except nav.Lost as x:
            raise Stuck('could not reach the main menu: %s' % str(x)[:200])
        go = int(time.time() * 10) % 1000000 + 1
        self.batch(['cvar wwe2k14_match %s' % spec, 'cvar wwe2k14_match_dry false', 'cvar wwe2k14_match_compare ""',
                    'cvar wwe2k14_match_go %d' % go, 'wait_frames 150'], timeout=300)
        if not self.wait_for({'kind': 'seq', 'seq': 9}, 240):
            lines = parse_log(self.log_path())['build']
            raise Unbuildable('no match started: %s' % ' | '.join(lines[-4:])[:400])

    def log_path(self):
        g = sorted(glob.glob(os.path.join(self.logs, 'wwe2k14_%03d*.log' % (self.run or 0))))
        return g[0] if g else None

    def entrances_to_bell(self, timeout_s=900, every=300, max_shots=70):
        """Full entrances: shots through them until the bell (match clock_started). -> True if the bell rang."""
        end = min(time.time() + timeout_s, self.deadline)
        taken = 0
        idle = 0
        while time.time() < end:
            self.pump()
            if self.tr.clock:
                return True
            if self.tr.phase in ('entrance', 'match') and taken < max_shots:
                self.shot()
                taken += 1
            else:  # loading / menus / videos before the entrances: a picture now and then
                idle += 1
                if idle % 20 == 1:
                    self.shot()
            self.batch(['wait_frames %d' % every], timeout=300)
            self.pump()
        return False

    def paused(self):
        """True when the pause menu is open NOW. The screen text keeps the last burst after a menu closes (stale
        "PAUSED" stays in log_text), but the open pause menu re-fetches its labels every guest frame: so the newest
        fetch frame of a "PAUSED" / "RETURN TO GAM" string (`log_text 40` -> "recent text ... @frame") must be within
        4 frames of now (S92 switch-hang report: a stray START toggle left the CPU match paused for 25 minutes)."""
        if self.left() <= 0:
            raise Stuck('item deadline passed')
        try:
            _, log, _ = live.run_batch(self.name, ['log_text 40'], 60)
        except live.NoGame as x:
            raise Crashed('live channel gone: %s' % x)
        except OSError as x:
            raise Crashed('pipe error: %s' % x)
        now = None
        newest = -1
        for ln in log:
            m = re.search(r'screen text: .* \(frame (\d+)\)\s*$', ln)
            if m:
                now = int(m.group(1))
            m = re.search(r'recent text \(newest first\): (.*)$', ln)
            if m:
                for part in m.group(1).split(' | '):
                    mm = re.match(r'(.*) @(\d+)\s*$', part)
                    if mm and ('PAUSED' in mm.group(1).upper() or 'RETURN TO GAM' in mm.group(1).upper()):
                        newest = max(newest, int(mm.group(2)))
        res = now is not None and newest >= 0 and now - newest <= 4
        self.say('paused() = %s (now %s, newest pause text @%s)' % (res, now, newest))
        return res

    def set_pause(self, want, tries=3):
        """Press START (at most `tries` times) until paused() == want; raise Stuck when the toggle is not taken."""
        for i in range(tries):
            if self.paused() == want:
                return
            self.batch(['press START 5 10', 'wait_frames 40'], timeout=120)
        if self.paused() != want:
            raise Stuck('pause toggle not taken (wanted paused=%s after %d START presses)' % (want, tries))

    def skip_to_bell(self, tries=40):
        for _ in range(tries):
            self.batch(['press START 5 10', 'wait_frames 60'], timeout=120)
            if self.wait_for({'kind': 'match', 'what': 'clock_started'}, 1.5):
                # a press held over / just after the bell pauses the match (S92 race): unpause if so
                self.batch(['wait_frames 30'], timeout=60)
                self.set_pause(False, tries=3)
                return True
        return False

    def hold_match(self, frames, shot_at=(180, 600, 1200), pair_at=600):
        """Shots at the given guest-frame offsets after the bell, a flicker pair at pair_at, then done."""
        pos = 0
        marks = sorted(set(list(shot_at) + ([pair_at] if pair_at else [])))
        for m in marks:
            if m > frames:
                break
            if m > pos:
                self.batch(['wait_frames %d' % (m - pos)], timeout=300)
                pos = m
            if m == pair_at:
                self.pair()
            else:
                self.shot()
        if frames > pos:
            self.batch(['wait_frames %d' % (frames - pos)], timeout=300)
        self.pump()


# ---------------------------------------------------------------- result

def feature_check(g):
    """Session 92: tools/harness/feature_proof.py on this game's log, expecting every switch the game was started with.
    -> {'ok', 'proven', 'not_engaged': [names], 'details': {name: why}, 'no_entry': [...]} or {'error': ...}."""
    try:
        sys.path.insert(0, os.path.join(REPO, 'tools', 'harness'))
        import feature_proof
        expect = {}
        for f in getattr(g, 'start_flags', []):
            if f.startswith('--') and '=' in f:
                k, v = f[2:].split('=', 1)
                expect[k] = v
        r = feature_proof.check(g.log_path(), expect)
        return {'ok': r['ok'], 'proven': sum(1 for x in r['features'] if x['state'] == 'PROVEN'),
                'not_engaged': r['not_engaged'],
                'details': {x['name']: x['detail'] for x in r['features'] if x['state'] != 'PROVEN'},
                'no_entry': r['no_entry'][:12]}
    except Exception as x:  # a broken checker must not lose the result
        return {'error': str(x)[:200]}


def finish_result(item, g, status, note, started, elapsed_s, shots_dir, keep_png=False):
    """Everything after the game closed: log, windows, detectors, thumbnails -> the result dict."""
    g.pump()
    log = parse_log(g.log_path())
    an = analyse(item, log, g.tr)
    crash = bool(log['crash']) and status == 'ok'
    features = feature_check(g)
    if crash:
        status, note = 'crash', (note + ' ' + log['crash'][0])[:300]
    res = {
        'id': item['id'], 'family': item['family'], 'spec': item.get('spec'), 'names': item.get('names'),
        'arena': item.get('arena'), 'status': status, 'note': note, 'run': g.run, 'exe_dir': g.exe_dir,
        'started': started, 'finished': now(), 'elapsed_s': round(elapsed_s), 'lane': g.name,
        'verdict': verdict_of(an['totals'], status),
        'totals': an['totals'], 'by_phase': an['by_phase'], 'signatures': an['signatures'][:30],
        'lost_clear_frames': an['lost_clear_frames'], 'lost_clears_all': an['lost_clears_all'],
        'copy_losses': log['copy_losses'][:10], 'gpu_errors': log['errors'], 'crash_lines': log['crash'],
        'windows': [{k: w[k] for k in ('a', 'b', 'phase', 'passes', 'owned', 'fallback', 'reasons', 'no_pipeline',
                                       'truth_passes')} for w in log['windows']],
        'entrance_slots': {str(k): v for k, v in g.tr.entrance_slots.items()},
        'first_frames': g.tr.first, 'build_lines': log['build'][-6:],
        'free': log['free'],  # Session 92 M0: the 360 machinery still running (None on SDKs without the counters)
        'features': features,  # Session 92: feature_proof.py - did every switch this game was given engage (log lines)?
    }
    if status in ('stuck', 'crash', 'error', 'unbuildable') or note:
        res['last_events'] = g.events[-20:]
    fin = [e for e in g.events if e.get('kind') == 'match' and e.get('what') == 'finish']
    if fin:
        res['finish'] = fin[-1]
    shots = []
    os.makedirs(shots_dir, exist_ok=True)
    byfile = {}
    for s in g.shots:
        if not os.path.exists(s['path']):
            time.sleep(0.5)
        if not os.path.exists(s['path']):
            shots.append(dict(s, missing=True))
            continue
        thumb = os.path.join(shots_dir, s['file'] + '.jpg')
        try:
            m = detect.metrics(s['path'], thumb)
            rec = dict(s, thumb=os.path.relpath(thumb, CEN), metrics=m)
        except Exception as x:
            rec = dict(s, error=str(x)[:100])
        shots.append(rec)
        byfile[s['file']] = rec
    res['shots'] = shots
    res['flicker'] = []
    for a, b in g.pairs:
        if a in byfile and b in byfile:
            try:
                res['flicker'].append({'a': a, 'b': b, 'diff': detect.pair_diff(byfile[a]['path'], byfile[b]['path']),
                                       'phase': byfile[a]['phase']})
            except Exception as x:
                res['flicker'].append({'a': a, 'b': b, 'error': str(x)[:100]})
    if not keep_png:
        for s in shots:
            m = s.get('metrics') or {}
            odd = m.get('black_frac', 0) > 0.9 or m.get('white_frac', 0) > 0.8 or m.get('distinct', 999) < 64 \
                or m.get('magenta_frac', 0) > 0.001
            if s.get('thumb') and not s.get('error') and not odd:  # the full PNG stays for odd-looking shots
                try:
                    os.remove(s['path'])
                except OSError:
                    pass
    return res


def tb():
    return traceback.format_exc(limit=6)[-600:]
