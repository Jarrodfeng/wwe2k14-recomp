"""The project's automatic disk tidy (Jarrod, 2026-11-09: storage should stay ~75 GB by itself, nobody runs or checks it).

Replaces tools\\harness\\disk_cleanup.ps1. Rules and where they came from: docs\\DECISIONS.md; how sessions use it: CLAUDE.md (Safety).

S102 TIDY2 (Jarrod, 2026-11-15) SUPERSEDES every rule below about ages, KEEP files, KEEP_TREES and state-doc names for out\\:
  out\\ is SCRATCH. It is cleared at the end of a session (--auto end) and by the hourly pass when no session is live; while a session is live it is
  left alone (only --now / the under-50-GB pass, which clear what has been idle 2 h). Held (rules in tidy_out.py): the allowlist (out\\build is SACRED:
  Jarrod plays from it, never deleted by any pass), tools\\harness\\keep_list.txt (exact paths, reason, optional until-date), exact paths a check script
  names (a container name protects nothing), --hold carry-overs. A KEEP file is reported as ignored.
  Unpack copies (fview_unpack_*) are never held. Agent worktrees: finished + merged go at the next pass; unmerged are reported, never taken
  automatically. --plan shows what session end would delete now, the held paths with sizes, and problems. Tests: tidy_out_selftest.py.

WHAT IT MAY TOUCH: out\\, the SDK worktrees in thirdparty\\sdk-wt, and the Claude Code agent worktrees .claude\\worktrees\\agent-* (a
whole worktree, or only the contents of its own out\\ scratch: LANE 294 below; never any other file there). Deletes permanently,
never follows a junction or link.
  - out\\ is temporary by default. Anything not on the keep list expires, except small notes (.md .txt .json .toml .py .ps1 up to
    2 MB: source comments and docs cite them). Expiry is ROLLING (lane 154, Jarrod 2026-11-12): output made DURING a live session
    expires at LIVE_KEEP_H (6 h); output made before it (last night's) and everything when no session is live expires at
    EXPIRE_H (24 h). Sheets made for Jarrod overnight need a hold.
  - RUN FOLDERS out\\sNN\\runs\\YYYYMMDD_HHMMSS_<route> (lane 294): the newest of each route, and any within RUN_PAIR_MIN (30 min) of it
    (a run's case and record folders, A/B pairs), keep 24 h (S102 K1: a live session's start no longer widens it); every other run folder goes after
    RUN_H (2 h). A run named in a state doc / --hold, or holding a valid KEEP, stays. An unpacked capture copy
    (images.bin / manifest.json / case_*.zip) outside a run goes at CAPTURE_COPY_H (6 h). A file that cannot be removed stops its
    folder (capture markers go first); never half a capture.
  - KEEP FILES EXPIRE (lane 294, Jarrod 2026-11-13): a KEEP holds only while it has a reason AND (1) a future "until YYYY-MM-DD"
    in it, or (2) a check, selftest, oracle or gate script in tools\\ names the folder's own out path (the folder ITSELF: a
    longer path under it does not count; comments, docstrings and `# tidy: output` lines are not read), or its reason cites an existing such
    script that names the folder. A bare KEEP is a 7-day hold from its mtime, then no hold. --plan
    lists every KEEP folder with its size and state.
  - KEEP_TREES (current builds, tools, indexes) stay, minus the throwaway run files inside them (THROW).
  - NO_TOUCH (bench data, replays, scenarios, recordings, ...) is never touched.
  - Finished git worktrees are removed whole. A worktree with real edits is reported, never removed. Commits that no branch holds are
    saved first under refs/tidy/. An agent worktree (.claude\\worktrees\\agent-<id>) is live, and the worktree itself never touched,
    while the agent's transcript (~\\.claude\\projects\\...\\subagents\\agent-<id>.jsonl) or any file in the worktree changed within
    AGENT_LIVE_H (2 h), and while its agent's meta (subagents\\agent-<id>.meta.json, matched by worktreePath) has no "finished": true
    and the parent session is live. A worktree whose HEAD is NOT yet merged into the branch checked out in the main checkout (also
    sdk-wt and out\\wt) waits AGENT_UNMERGED_H (24 h) after its last activity. An uncommitted edit of a BUILD_SETUP file in an agent
    worktree is a real edit. Finished ones go on every trigger, during a live session too; the agent is re-read right before
    deleting; Claude Code's worktree lock is removed and the entry pruned. Branches stay.
  - AGENT WORKTREE out\\ SCRATCH (lane 294): never tracked files, never a junction. Once the agent is finished (meta flag, no later
    transcript) its out\\ goes when nothing in it changed for 30 min (branch merged and no uncommitted edits) or AGENT_UNMERGED_H
    (24 h: unmerged, or edits not committed) - a reviewer may still be using it. Over SCRATCH_REPORT_GB (10) it is reported as LOUD
    on every pass; while C: has under 100 GB free, such an out\\ is cleared per top-level folder, WHOLE, only when that folder's
    newest file is older than 15 min, even if the agent is live (a live agent's build trees - CMakeCache.txt / build.ninja /
    .ninja_log, or any folder holding .exe / .obj / .pdb / .dll / .lib - and anything holding a junction stay). Same refusals as any
    other delete: a --hold, a KEEP, an owner process. S102 K1: a FINISHED agent over its quota (4 GB; a file .tidy_quota in the worktree
    raises it) also loses its biggest top-level folders idle 15 min, whatever C: shows; a running agent is never touched by the quota.
  - CLAUDE SCRATCHPADS (S102 K1): Temp\\claude\\<hash>\\<session>\\scratchpad is measured and reported on every pass (LOUD over 15 GB).
    The delete pass (children of a session that is not live, idle 12 h, no junction, no KEEP) exists but is OFF (SCRATCHPAD_PASS /
    --scratchpads) until Jarrod says yes.

NEVER REMOVED, on every trigger: anything younger than the rolling limit above, anything under a folder with an ACTIVE KEEP (above),
anything on the carry-over list (--hold), any path named in STATUS.md, PLAYBOOK.md, CLAUDE.md or the newest session plan / summary
(a session root or an out\\sNN\\runs namespace named there protects nothing), any out path (depth 2 and deeper) a check script names
(a glob cut at * is a prefix): every capture / run folder it names stays
(whatever its age); other folders a check / selftest / oracle / gate script names while they were touched within TOOL_STALE_DAYS
(7). A game or build holding the run lock = the tidy skips this time (--scratch and the watch only skip for an EXCLUSIVE hold or a
game process: S102 K1). Free space is logged every watch run (out\\tidy\\free.csv, the [disk] line; --reserve-ok exits 1 under 40 GB free). Every pass except --now prints the project's size against the
~75 GB budget and the top 5 folders, and MISSING INPUT for paths a check names that do not exist.

  py tools\\harness\\tidy.py --plan [--ahead H]       dry run: writes out\\tidy\\plan_<time>.json/.txt, prints the summary
                                                     (--ahead: a forecast as if it were H hours later, not applicable;
                                                     TIDY_REPO=<checkout> runs this copy against another checkout, --plan only)
  py tools\\harness\\tidy.py --apply PLAN.json       deletes exactly that list (the first clean-up; also turns --auto on)
  py tools\\harness\\tidy.py --auto start|end|hourly  one pass that decides and deletes (no dry run). Add --dry to only print
  py tools\\harness\\tidy.py --now [--dry]        EMERGENCY, one command, no prompts, no measuring first (lane 187, Jarrod 2026-11-12):
                                                     (a) finished agent worktrees idle >= 30 min, biggest first (running agents found
                                                     from their meta / transcript, whatever the parent session; MERGED only: unmerged ones wait 2 h, and 30 min under 15 GB free;
                                                     commits go under refs/tidy/, branches stay), then (b) the agent out\\ scratch pass, then
                                                     (c) exactly the under-50-GB pass (its ages), until 100 GB free (--target GB). One short
                                                     summary (no size budget walk). Same protections as always.
  py tools\\harness\\tidy.py --auto watch           10 min task wwe2k14-tidy-watch. Reads free space only; below 50 GB runs --now; else, when an
                                                     agent worktree has an out\\, and no timed run holds the run lock, a dry look at that scratch
                                                     (no delete, no lock) and only if something would go the scratch pass; else exits at once
  py tools\\harness\\tidy.py --scratch [--dry]      the agent worktree out\\ scratch pass alone
  py tools\\harness\\tidy.py --guard PATH...         for hand-written clean-ups: exit 1 and name every PATH to refuse. Judged against the MAIN
                                                     checkout; relative paths are made absolute against the current directory first. Refused:
                                                     outside out\\ / sdk-wt / an agent out\\; at, under or ABOVE a keep-list or never-touch
                                                     folder; at, under or above a registered worktree; inside the out\\ of a running or recent
                                                     agent; a newest (possibly running) run folder or the runs folder; behind a link; under or
                                                     holding an active KEEP; an input a check names, inside one or above one
  py tools\\harness\\tidy.py --missing              inputs a check names that do not exist (MISSING INPUT)
  py tools\\harness\\tidy.py --hold PATH "why"       carry a folder over to the next session (released when that session
                                                     ends, or after 7 days)
  py tools\\harness\\tidy.py --hold PATH "why" --until YYYY-MM-DD   (or --days N) a hold with its own expiry: released
                                                     only then, never by --auto end
  py tools\\harness\\tidy.py --holds                 list the carry-over   --release [PATH]  drop one, or all

Triggers: --auto start from the SessionStart hook; --auto end is a step of ending a session (CLAUDE.md "End", after
asking Jarrod when it is not clearly the end or the next session will use the files); --auto hourly from the scheduled
task "wwe2k14-tidy" (below 50 GB free on C: it goes past the rolling rule, oldest and biggest throwaway types first,
finished agent worktrees biggest first, never into the last LOW_LIVE_KEEP_H (2 h) of a live session, and stops at 75 GB
free; if that is not enough it tells Jarrod, at most once a day); --auto watch from "wwe2k14-tidy-watch" (every 10 min).
"""
import argparse
import calendar
import glob
import ctypes
import collections
import json
import msvcrt
import os
import re
import shutil
import stat
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tidy_disk      # S102 K1: strip_py, free log, duplicate captures
import tidy_out       # S102 TIDY2: the allowlist, the keep list, containers, the sweep

# TIDY_REPO: run this copy of the script against another checkout (lane 154: planning the real tree from a worktree).
REPO = (os.environ.get('TIDY_TEST_ROOT') or os.environ.get('TIDY_REPO')
        or os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
TEST = bool(os.environ.get('TIDY_TEST_ROOT'))
OUT = os.path.join(REPO, 'out')
SDK = os.path.join(REPO, 'thirdparty', 'rexglue-sdk-src')
TIDY = os.path.join(OUT, 'tidy')
WT_ROOTS = [os.path.join(REPO, 'thirdparty', 'sdk-wt'), os.path.join(SDK, 'thirdparty', 'sdk-wt')]
ALLOWED_ROOTS = [OUT] + WT_ROOTS
AGENT_WT = os.path.join(REPO, '.claude', 'worktrees')     # lane 154: only agent-* worktrees directly in it, never its files
PROJECTS = os.environ.get('TIDY_TEST_PROJECTS') or os.path.expanduser(r'~\.claude\projects')
PROJECT_PREFIX = 'C--Users-Jarrod-wwe2k14-recomp'

HOUR = 3600.0
GB = 1e9
EXPIRE_H = 24       # no live session: temporary output older than this expires
LIVE_KEEP_H = 6     # lane 154: while a session is live, temporary output older than this expires (rolling)
LOW_LIVE_KEEP_H = 2     # the under-50-GB pass, while a session is live
AGENT_LIVE_H = 2        # an agent worktree (or its transcript) active this recently is live
AGENT_UNMERGED_H = 24 * 3650   # S102 TIDY2: an unmerged agent worktree is never taken by an automatic pass (reported; the main session decides); --now passes its own hours
AGENT_MERGED_H = 0.25   # S102 TIDY2: a finished agent whose branch is merged goes at the next pass (15 min idle guard)
WT_IDLE_H = 6           # plain worktrees (sdk-wt, out\\wt, bisect): idle this long (and no running agent / .owner pid) before they go
HOURLY_IDLE_H = 6       # the hourly out\\ sweep (no session live) takes only items untouched this long
ACTIVE_IDLE_H = 0.5     # session end while an agent / Surface job / build of this session is still running: only items idle this long
UNKNOWN_SESSION = 'unknown-sessions'
ORPHAN_META_H = 24      # a meta without "finished" counts as running only while it (or its transcript) was written this recently
LOW_GB = 50
TARGET_GB = 75
NOW_IDLE_H = 0.5        # --now / --auto watch / the under-50-GB pass: a finished agent worktree idle this long goes (S101 lane 187)
NOW_TARGET_GB = 100     # --now stops once this much is free
HARD_FLOOR_GB = 15      # --now only: below this much free, finished UNMERGED agent worktrees go at NOW_IDLE_H too
ANY_PARENT = False      # --now: a meta without 'finished' written within ORPHAN_META_H counts as running whatever its parent session
LOCKED = False         # this process runs inside the run-lock hold (--locked)
LIVE_IDLE_H = 12    # a session whose transcript was written this recently is live (S102 TIDY2: was 3; a session waiting on a long Surface job is not dead)
LIVE_MAX_H = 72     # a session left open for days protects at most this far back
LIVE_AGENT_MAIN_H = 12     # its agents' transcripts count only if the session itself wrote within this
CARRY_DAYS = 7
NOTE_MAX = 2 * 1024 * 1024
ALERT_EVERY_H = 24
PLAN_MAX_H = 24     # an older plan is refused: the disk has moved on
REPARSE = stat.FILE_ATTRIBUTE_REPARSE_POINT
WORKERS = 16
RUN_H = 2               # lane 294: a run folder that is not the newest of its route goes after this
CAPTURE_COPY_H = 6      # lane 294: an unpacked capture copy outside a kept run goes after this
KEEP_BARE_DAYS = 7      # lane 294: a KEEP with neither an expiry nor a tool that names it holds this long from its mtime
SCRATCH_REPORT_GB = 10      # lane 294: an agent worktree's out\ over this is reported loudly every pass
SCRATCH_PRESSURE_GB = 100   # and cleared (even if its agent is live) while C: has less than this free
SCRATCH_RECENT_MIN = 15     # under pressure only files modified in the last 15 min stay
BUDGET_GB = 75              # CLAUDE.md: the project stays ~75 GB
RESERVE_GB = 40             # S102 K1: below this much free on C: `tidy.py --reserve-ok` returns 1 (lanes that write captures are not started)
SCRATCH_QUOTA_GB = 4        # S102 K1: an agent worktree's out\ quota (a worktree file .tidy_quota holding a number raises it); LOUD at half
SCRATCHPAD_PASS = True       # S102 K1: the scratchpad DELETE pass; ON since Jarrod's yes (2026-11-14 01:0x, CLAUDE.md Safety / DECISIONS)
SCRATCHPAD_CHILD_H = 12     # S102 K1: a scratchpad child of a session that is not live goes when its newest file is older than this
SCRATCHPAD_LOUD_GB = 15     # a live session's scratchpad over this is reported loudly

# S102 TIDY2: out\\ is scratch (tidy_out.py: the allowlist, the keep list, exact paths a check names). NO_TOUCH is the Deleter's last backstop: nothing
# below is ever deleted by any pass, --now included. out\\build is SACRED (Jarrod, 2026-11-15: "out/build which is where I play my game from").
NO_TOUCH = {'build', 'replays', 'scenarios', 'recordings', 'harness/screenshots', 'backup', 'tidy', 'bench/userdata_frozen', 'bench/userdata_frozen.sha256',
            'harness/runlock'}
KEEP_TREES = {p[4:] for p, _k, _w in tidy_out.allow_patterns('') if '*' not in p and '{' not in p}
CONTAINERS = {'harness', 'android', 'release', 'wt', 'harness/bisect', 'harness/bisect/wt', 'harness/bisect/exe', 'release/stage'}
NOTE_EXT = {'.md', '.txt', '.json', '.toml', '.py', '.ps1'}
THROW = [
    ('timeline', re.compile(r'^timeline_.*\.csv$', re.I)),
    ('run logs', re.compile(r'^wwe2k14_\d+(\.\d+)?\.log$', re.I)),
    ('frame logs', re.compile(r'^frames_.*\.csv$', re.I)),
    ('bench zips', re.compile(r'^surface_bench_.*\.zip$', re.I)),
    ('captures', re.compile(r'^(snap_.*|det_.*)\.bin$', re.I)),
]
KEEP_EXTRA = {'logo': re.compile(r'\.(mov|mp4)$', re.I)}     # old videos go; the logo sources stay
RUNLOG_NUM = re.compile(r'^wwe2k14_(\d+)\.log$', re.I)
COMPILED = {'.obj', '.o', '.pch', '.pdb', '.lib', '.a', '.exe', '.dll', '.so', '.ilk', '.map', '.exp', '.apk', '.gch', '.idb'}
CAPTURE = {'.rps', '.rdc', '.bin', '.xtc', '.xsh', '.dat', '.spvopt'}
IMAGE = {'.png', '.jpg', '.jpeg', '.bmp', '.dds', '.tga'}
VIDEO = {'.mp4', '.mov', '.mkv', '.avi', '.bik2'}
LOW_ORDER = ['timeline', 'bench zips', 'compiled', 'worktree', 'captures', 'run logs', 'frame logs', 'videos', 'images', 'other']
BUILD_SETUP = {'generated/rexglue.cmake', 'wwe2k14_manifest.toml', 'tools/owned/fast_build.ps1'}
REF_FILES_FIXED = ['STATUS.md', 'PLAYBOOK.md', 'CLAUDE.md']
REF_RE = re.compile(r'((?:out|thirdparty[\\/]+sdk-wt)[\\/]+[^\s`\'"<>|*?(),;:\[\]{}]+)')
NO_WINDOW = 0x08000000
DETACHED = 0x00000008
NEW_GROUP = 0x00000200


def lp(p):
    """Long-path form, so a worktree's deep files can be listed and deleted."""
    return p if p.startswith('\\\\?\\') else '\\\\?\\' + os.path.abspath(p)


def born(st):
    """When a file was last created or changed. Copies keep an old mtime, so the creation time counts too."""
    return max(st.st_mtime, getattr(st, 'st_birthtime', st.st_ctime))


class Age:
    """Which timestamps have expired: older than cutoff, or (end of a session) made since that session began."""

    def __init__(self, cutoff, own_since=None, rolling=None):
        self.cutoff = cutoff
        self.own_since = own_since
        self.rolling = rolling      # (live session start, now - LIVE_KEEP_H): output made inside this window expires too

    def expired(self, t):
        return (t < self.cutoff or (self.own_since is not None and t >= self.own_since)
                or (self.rolling is not None and self.rolling[0] <= t < self.rolling[1]))


def make_ages(now, live, own_since=None, low=False):
    """(age, strict) for one pass (lane 154).
    age: temporary output expires when it is older than EXPIRE_H, or when it was made during the live session and is older
      than LIVE_KEEP_H (two intervals: output made BEFORE the oldest live session is not hit by the 6 h rule, so last night's
      output survives the morning session's first pass and expires at 24 h).
    strict: the old rule (whole live session + 24 h) for capture folders and out\\s*\\runs, which running tools still read.
    low (under 50 GB): everything older than LOW_LIVE_KEEP_H, or 1 h when no session is live; strict = the old low-pass rule."""
    start = min([s[1] for s in live]) if live else None
    if low:
        cut = now - (LOW_LIVE_KEEP_H if live else 1) * HOUR
        return Age(cut), Age(now - HOUR)      # S102 K1: the START of a live session no longer widens the strict cutoff
    c24 = now - EXPIRE_H * HOUR
    if live:
        return (Age(c24, own_since, (start, now - LIVE_KEEP_H * HOUR)), Age(c24, own_since))      # S102 K1: strict = 24 h like everything else (was min(24 h, session START) = up to 72 h)
    a = Age(c24, own_since)
    return a, a


def type_of(name):
    for label, rx in THROW:
        if rx.match(name):
            return label
    x = os.path.splitext(name)[1].lower()
    if x in COMPILED:
        return 'compiled'
    if x in CAPTURE:
        return 'captures'
    if x in VIDEO:
        return 'videos'
    if x in IMAGE:
        return 'images'
    return 'other'


def is_throw(name):
    return any(rx.match(name) for _, rx in THROW)


def is_allowed(path):
    p = os.path.normcase(os.path.abspath(path))
    return any(p.startswith(os.path.normcase(os.path.abspath(r)) + os.sep) for r in ALLOWED_ROOTS)


def wt_allowed(path):
    """An agent worktree: a folder named agent-* directly in .claude\\worktrees (lane 154). Nothing deeper, no other file."""
    p = os.path.normcase(os.path.abspath(path))
    return (os.path.dirname(p) == os.path.normcase(os.path.abspath(AGENT_WT))
            and os.path.basename(p).startswith('agent-'))


def repo_key(path):
    return os.path.relpath(path, REPO).replace('\\', '/').lower()


def out_rel(path):
    return os.path.relpath(path, OUT).replace('\\', '/').lower()


def git(repo, *a, timeout=180):
    return subprocess.run(['git', '--no-optional-locks', '-C', repo, *a], capture_output=True, text=True, errors='replace',
                          timeout=timeout, creationflags=NO_WINDOW)


def pid_alive(pid):
    h = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)     # never os.kill(pid, 0): on Windows that is CTRL_C_EVENT
    if not h:
        return False
    code = ctypes.c_ulong()
    ok = ctypes.windll.kernel32.GetExitCodeProcess(h, ctypes.byref(code))
    ctypes.windll.kernel32.CloseHandle(h)
    return bool(ok) and code.value == 259


def free_gb():
    if TEST and os.environ.get('TIDY_TEST_FREE'):      # S102 K1 selftest
        return float(os.environ['TIDY_TEST_FREE'])
    return shutil.disk_usage(os.path.splitdrive(REPO)[0] + '\\').free / GB


def stamp(t=None):
    return time.strftime('%Y-%m-%d %H:%M', time.localtime(t if t is not None else time.time()))


# ---------------------------------------------------------------- state files
def load_json(name, default):
    try:
        with open(os.path.join(TIDY, name), encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def save_json(name, data):
    os.makedirs(TIDY, exist_ok=True)
    tmp = os.path.join(TIDY, name + '.tmp')
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(data, f, indent=1)
    os.replace(tmp, os.path.join(TIDY, name))


def history(line):
    os.makedirs(TIDY, exist_ok=True)
    with open(os.path.join(TIDY, 'history.txt'), 'a', encoding='utf-8') as f:
        f.write('%s  %s\n' % (stamp(), line))


def prune_reports(prefix, keep=30):
    try:
        old = sorted((n for n in os.listdir(TIDY) if n.startswith(prefix)), reverse=True)[keep:]
        for n in old:
            os.unlink(os.path.join(TIDY, n))
    except OSError:
        pass


# ---------------------------------------------------------------- what is protected
def sessions(now):
    """FAIL CLOSED (S102 TIDY2, Opus review): [(session id, start, last write)] of live sessions. A projects folder that is missing / unreadable / holds
    no project folder, or any error, returns one UNKNOWN session = "something is live" (the sweep then leaves out\\ alone), never "nothing is live"."""
    if TEST:
        return _sessions_raw(now)
    try:
        if not os.path.isdir(PROJECTS) or not any(d.name.startswith(PROJECT_PREFIX) and d.is_dir() for d in os.scandir(PROJECTS)):
            return [(UNKNOWN_SESSION, now, now)]
        return _sessions_raw(now)
    except Exception:
        return [(UNKNOWN_SESSION, now, now)]


def _sessions_raw(now):
    """[(session id, start time, last write)] of live Claude sessions on this project."""
    out = []
    if TEST:
        return [tuple(x) for x in json.loads(os.environ.get('TIDY_TEST_LIVE', '[]')) if now - x[2] <= LIVE_IDLE_H * HOUR]
    if not os.path.isdir(PROJECTS):
        return out
    for d in os.scandir(PROJECTS):
        if not d.name.startswith(PROJECT_PREFIX) or not d.is_dir():
            continue
        for e in os.scandir(d.path):
            if not e.name.endswith('.jsonl'):
                continue
            st = e.stat()
            last = st.st_mtime
            sub = os.path.join(d.path, e.name[:-6], 'subagents')     # a session's agents write their own transcripts
            # Only for a session that was itself active lately: an old session's agent files can get touched long after
            # it ended (seen 2026-11-09: a 2 Nov session counted as live and shielded 115 GB).
            if now - st.st_mtime <= LIVE_AGENT_MAIN_H * HOUR and os.path.isdir(sub):
                for a in os.scandir(sub):
                    if a.name.endswith('.jsonl'):
                        last = max(last, a.stat().st_mtime)
            if now - last > LIVE_IDLE_H * HOUR:
                continue
            start = None
            try:
                with open(e.path, encoding='utf-8', errors='replace') as f:
                    for line in f:
                        m = re.search(r'"timestamp"\s*:\s*"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)', line)
                        if m:
                            start = calendar.timegm(time.strptime(m.group(1), '%Y-%m-%dT%H:%M:%S'))     # transcripts are UTC
                            break
            except OSError:
                pass
            if start is None:
                start = getattr(st, 'st_birthtime', st.st_ctime)
            out.append((e.name[:-6], max(start, now - LIVE_MAX_H * HOUR), last))
    return sorted(out, key=lambda s: s[2], reverse=True)


FIXED_WHY = re.compile(r'jarrod|until\s+\d{4}-\d\d-\d\d', re.I)


OUT_RE = re.compile(r'(?<![A-Za-z0-9_])(out[\\/]+[^\s`\'"<>|*?(),;:\[\]{}]+)')
JOIN_RE = re.compile(r"""['"]out['"]((?:\s*[,/]\s*['"][^'"\\/\n]+['"](?!\s*:))+)""")      # S102 K1: a literal followed by ':' is a dict key, not a path part
LIT_RE = re.compile(r"""['"]([^'"\\/\n]+)['"]""")
TOOLS_PY_MAX = 2 * 1024 * 1024
TOOL_STALE_DAYS = 7


TOOL_READERS = re.compile(r'check|selftest|oracle|gate', re.I)     # tools that read captures; a generator's output folder is not an input


def _doc_namespace(rel):
    """Only a session root, its runs folder or a container is too broad for a state doc to name (a path under KEEP_TREES stays named)."""
    return re.fullmatch(r's\d+(/runs)?', rel) is not None or rel in CONTAINERS


def _namespace(rel):
    """A folder that is only a namespace or already handled by its own policy (a session root, a container, the keep list,
    never-touch): protecting it as an "input" would shield everything under it."""
    return (re.fullmatch(r's\d+(/runs)?', rel) is not None or rel in CONTAINERS or rel in NO_TOUCH
            or any(rel == k or rel.startswith(k + '/') for k in KEEP_TREES))


def _add_input(found, k, src):
    """S102 TIDY2: a name protects EXACTLY that path (a file, or a leaf folder), never a container, never a scratch copy; a capture marker file
    (images.bin / manifest.json) names its capture folder. A glob cut at '*' / '?' is a PREFIX key (ends with '*')."""
    star = k.endswith('*')
    k = re.sub(r'[\\/]+', '/', k.rstrip('*')).rstrip('./').lower()
    parts = k.split('/')
    if len(parts) < 2 or parts[0] != 'out' or tidy_out.is_scratch_copy(k):
        return
    cands = [k]
    if not star and len(parts) >= 3 and parts[-1] in tidy_out.CAPTURE_MARKERS:
        cands = ['/'.join(parts[:-1])]
    for c in cands:
        if len(c.split('/')) < 2 or _namespace(c[4:]) or tidy_out.container_reason(c, OUT):
            continue
        found.setdefault(c + ('*' if star else ''), src)


def _scan_text(text, found, src):
    for m in OUT_RE.finditer(text):
        _add_input(found, m.group(1) + ('*' if text[m.end():m.end() + 1] in ('*', '?') else ''), src)
    for m in JOIN_RE.finditer(text):      # os.path.join(..., 'out', 'a', 'b') and 'out' / 'a' / 'b'
        _add_input(found, 'out/' + '/'.join(LIT_RE.findall(m.group(1))), src)


def required_input(k):
    """Lane 294 (2026-11-13: the clean-up deleted ring check captures J1 / 7149 that d3_ring_check.py needs): a capture or run folder a check
    names is a REQUIRED input and is protected whatever its age (no TOOL_STALE_DAYS rule): its last part is capture, or a part is case_*
    or a runs/<stamp> folder. Other tool-named folders keep the 7-day rule."""
    parts = k.split('/')
    return parts[-1] == 'capture' or any(x.startswith('case_') for x in parts) or 'runs' in parts[1:-1]


def tool_inputs():
    """Repo-relative paths (depth >= 2, so out/post_plan_cache counts) that check tools read (lane 154, S3):
      tools\\**\\*check*.py / *selftest*.py / *oracle*.py / *gate*.py: out path literals ('out/a/b', r'out\\a\\b', os.path.join(..., 'out', 'a', 'b'), 'out' / 'a').
    The check tools take no run lock, so the tidy cannot know when they run."""
    troot = os.path.join(REPO, 'tools')
    py = {}
    for dp, dn, fn in os.walk(troot):
        dn[:] = [x for x in dn if x != '__pycache__' and x != 'node_modules']
        for n in fn:
            if not n.endswith('.py') or not TOOL_READERS.search(n) or n.startswith('tidy'):
                continue
            fp = os.path.join(dp, n)
            try:
                if os.path.getsize(fp) > TOOLS_PY_MAX:
                    continue
                text = open(fp, encoding='utf-8', errors='replace').read()
            except OSError:
                continue
            _scan_text(tidy_disk.strip_py(text), py, os.path.relpath(fp, REPO).replace('\\', '/'))      # S102 K1: no comments, docstrings or `# tidy: output` lines
    return py


def named_inputs():
    """Protected inputs (S102 TIDY2): the capture / case / run folders a check script names. No age rule. A check script's working folder
    (out/l123, ...) is scratch: the check makes it again."""
    found = {}
    for k, v in tool_inputs().items():
        if required_input(k) or (not k.endswith('*') and tidy_out.is_capture_dir(os.path.join(REPO, k.replace('/', os.sep)))):
            found[k] = v
    return found


MISSING_IGNORE_SOURCES = {'tools/renderer/scene_start_check.py'}      # names out/s101/runs/run/case_* as a placeholder, not an input


def missing_inputs():
    """Lane 294: [(path, source)] of required_input() paths a check script names that do not exist now (a glob prefix counts: some scripts name
    case_<stamp>*)."""
    py = tool_inputs()
    out = []
    for k, v in sorted({k: v for k, v in py.items() if required_input(k)}.items()):
        if re.search(r'[%{}$<>]', k) or '*' in k[:-1] or 'selftest' in v.lower() or v in MISSING_IGNORE_SOURCES:      # selftest paths are fixtures, not inputs
            continue
        star = k.endswith('*')
        base = os.path.join(REPO, k.rstrip('*').replace('/', os.sep))
        if star:
            present = bool(glob.glob(glob.escape(base) + '*'))
        else:
            present = os.path.exists(base) or bool(glob.glob(glob.escape(base) + '*'))
        if not present:
            if not k.endswith(('.exe', '.dll')) and not any(k.startswith(m[0] + '/') for m in out):     # one line per missing root
                out.append((k, v))
    return out


def missing_lines(limit=12):
    try:
        miss = missing_inputs()
    except Exception as e:
        return ['MISSING INPUT check failed: %r' % (e,)]
    L = ['MISSING INPUT: %s  (named by %s)' % m for m in miss[:limit]]
    if len(miss) > limit:
        L.append('MISSING INPUT: ... and %d more (tidy.py --missing)' % (len(miss) - limit))
    return L


def main_checkout():
    """The main checkout's root (git common dir's parent), so --guard works the same from an agent worktree."""
    try:
        r = git(REPO, 'rev-parse', '--path-format=absolute', '--git-common-dir')
        c = r.stdout.strip()
        if r.returncode == 0 and c:
            return os.path.dirname(os.path.abspath(c))
    except (OSError, subprocess.TimeoutExpired):
        pass
    return REPO


def _refuse_tree(ap):
    """At, under or ABOVE anything held (the allowlist, the keep list, paths a check names, carry-overs, never-touch): the tidy never deletes those,
    so a hand-written delete must not."""
    if not os.path.normcase(ap).startswith(os.path.normcase(OUT) + os.sep):
        return ''
    rel = 'out/' + out_rel(ap)
    for k in sorted(NO_TOUCH):
        if rel == 'out/' + k or rel.startswith('out/' + k + '/'):
            return 'at or under the never-touch folder out\\' + k.replace('/', '\\')
    h = held_index()[0]
    w = h.why(rel)
    if w:
        return 'held (%s)' % w[:80]
    if h.ancestor(rel):
        return 'above a held path (the tidy enters it and removes only what is not held)'
    return ''


def _refuse_worktree(ap):
    """At, inside or above a registered worktree (sdk-wt, out\\wt), or a folder with a .git inside sdk-wt: those go only through the tidy."""
    n = os.path.normcase(ap)
    for w in list_worktrees(REPO) + list_worktrees(SDK):
        wp = os.path.normcase(os.path.abspath(w['path']))
        if is_allowed(w['path']) and (n == wp or wp.startswith(n + os.sep) or n.startswith(wp + os.sep)):
            return 'at, inside or above the registered worktree ' + os.path.relpath(w['path'], REPO)
    for r in WT_ROOTS:
        rn = os.path.normcase(os.path.abspath(r))
        if n.startswith(rn + os.sep) and os.path.exists(os.path.join(ap, '.git')):
            return 'an unregistered worktree folder'
    return ''


def _refuse_agent_out(ap, now):
    """Inside the out\\ of an agent that is running or was active within AGENT_LIVE_H (or has no record: unknown counts as running)."""
    if not in_agent_out(ap):
        return ''
    root = os.path.normcase(os.path.abspath(AGENT_WT)) + os.sep
    name = os.path.normcase(os.path.abspath(ap))[len(root):].split(os.sep)[0]
    wt = os.path.join(AGENT_WT, name)
    infos = agent_infos()
    last, running, mine = agent_state(infos, {s_[0] for s_ in sessions(now)}, wt, now, AGENT_LIVE_H)
    if not mine:
        return 'inside the out\\ of an agent worktree with no agent record (treated as running)'
    if running or last >= now - AGENT_LIVE_H * HOUR:
        return 'inside the out\\ of an agent that is running or was active in the last %d h' % AGENT_LIVE_H
    return ''


def _refuse_run(ap):
    """The runs folder (it holds the newest runs) or a run folder in the newest set of its route (possibly a run in progress)."""
    if not os.path.normcase(ap).startswith(os.path.normcase(OUT) + os.sep):
        return ''
    r = out_rel(ap)
    if RUNS_DIR_RE.fullmatch(r):
        return 'the runs folder holds the newest (possibly running) run folders'
    m = RUN_CHILD_RE.match(r)
    if m:
        d = os.path.join(OUT, *m.group(1).split('/'))
        try:
            names = [e.name for e in os.scandir(lp(d)) if e.is_dir(follow_symlinks=False)]
        except OSError:
            return ''
        if m.group(2) in newest_run_names(names):
            return 'the newest run folder of its route (a run in progress keeps writing there)'
    return ''


def guard_paths(paths):
    """Lane 294: for each path (absolute; a relative one is made absolute against the current directory) -> (path, reason or ''): refused
    when it is outside what the tidy may touch (out\\, sdk-wt, an agent worktree's out\\), at / under / above a keep-list or never-touch
    folder, at / inside / above a registered worktree, inside the out\\ of a running or recent agent, a newest run folder (or the runs
    folder), behind a junction, holds or lies under an active KEEP, IS a protected input, lies inside one, or CONTAINS one (glob prefixes
    too). For hand-written clean-up scripts."""
    global ANY_PARENT
    ANY_PARENT = True       # an agent whose meta lacks "finished" counts as running whatever its parent session
    protect, named = protect_keys()
    exact = {q for q in protect if not q.endswith('*')}
    prefixes = [q[:-1] for q in protect if q.endswith('*')]
    d = Deleter(time.time())
    now = time.time()
    res = []
    try:
        for p in paths:
            ap = os.path.abspath(p)
            why = ''
            if not (is_allowed(ap) or in_agent_out(ap)):
                why = 'outside the folders the tidy may touch (out\\, thirdparty\\sdk-wt, an agent worktree out\\)'
            elif d.protected_zone(ap):
                why = 'never-touch folder (bench data, replays, scenarios, ...)'
            else:
                why = _refuse_tree(ap) or _refuse_worktree(ap) or _refuse_agent_out(ap, now) or _refuse_run(ap)
                if not why and os.path.lexists(ap) and d.link_on_path(ap):
                    why = 'a junction or link at or above it'
                elif not why and (keep_above(ap) or keep_active(ap)):
                    why = 'an active KEEP file holds it'
                elif not why and os.path.isdir(ap) and walk_tree(ap)[4]:
                    why = 'an active KEEP file inside it'
                elif not why:
                    try:
                        k = repo_key(ap)
                    except ValueError:
                        k = ''
                    hit = next((q for q in exact if k == q or k.startswith(q + '/') or q.startswith(k + '/')), None)
                    if hit is None:
                        pf = next((q for q in prefixes if k.startswith(q) or q.startswith(k + '/')), None)
                        hit = pf + '*' if pf is not None else None
                    if hit:
                        why = 'protected: %s (%s)' % (hit, named.get(hit, 'hold'))
            res.append((p, why))
    finally:
        d.pool.shutdown()
    return res


def carry_entries():
    """The holds. Lane 154: an entry made with --until / --days has fixed=True and keeps its own expiry. Older entries whose
    reason says Jarrod or names an until date are migrated to that kind when read (saved with the next change)."""
    rows = load_json('carry.json', [])
    for c in rows:
        if 'fixed' not in c:
            c['fixed'] = bool(FIXED_WHY.search(c.get('why', '')))
    return rows


def carried_keys():
    return {c['path'] for c in carry_entries()}


def carry_expire(now, released_before=None):
    """Drops holds past their expiry; at the end of a session also the plain ones added before it. A fixed hold
    (--until / --days) is released only at its own expiry."""
    keep, gone = [], []
    for c in carry_entries():
        if now > c['expires'] or (released_before is not None and c['added'] < released_before and not c['fixed']):
            gone.append(c)
        else:
            keep.append(c)
    if gone:
        save_json('carry.json', keep)
    return gone


def lock_busy(shared_ok=False):
    """Why the tidy should skip now, or ''. S102 K1 (R12 change 7): shared_ok = the pass runs under its own SHARED slot, so other shared / build
    holders do not stop it; only an EXCLUSIVE hold (a timed run) or a game process does."""
    fake = os.environ.get('TIDY_TEST_LOCKSTATUS')      # S102 K1 selftest: a run-lock status text to parse
    if TEST and fake is None:
        return ''
    try:
        o = fake if fake is not None else subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(REPO, 'tools', 'runlock.ps1'), 'status'],
                                                         capture_output=True, text=True, errors='replace', timeout=60, creationflags=NO_WINDOW).stdout
    except (OSError, subprocess.TimeoutExpired) as e:
        return 'run lock unreadable (%s)' % e
    for line in o.splitlines():
        s = line.strip()
        if s.startswith('mode:') and s != 'mode: free' and (not shared_ok or 'EXCLUSIVE' in s.upper()):
            return s
        if not shared_ok and re.match(r'(slot|build)\d: ', s) and not s.endswith(': free') and 'STALE' not in s.upper():
            return s
        if s.startswith('game processes:') and s != 'game processes: none':
            return s
    return ''


# ---------------------------------------------------------------- KEEP files (lane 294: they expire)
KEEP_UNTIL_RE = re.compile(r'(?:until|expires?|expiry)\D{0,12}(\d{4}-\d\d-\d\d)', re.I)
TOOL_REF_RE = re.compile(r'tools[\\/][\w.\\/-]+', re.I)
_keep_cache = {}
_corpus_cache = []


def reader_corpus():
    """tools\\**\\*.py (the check readers), lower case, slashes normalised. Read once, only when a KEEP needs it."""
    if _corpus_cache:
        return _corpus_cache[0]
    parts = []
    for dp, dn, fn in os.walk(os.path.join(REPO, 'tools')):
        dn[:] = [x for x in dn if x not in ('__pycache__', 'node_modules')]
        for n in fn:
            if n.endswith('.py') and not n.startswith('tidy') and TOOL_READERS.search(n):     # S2: check / selftest / oracle / gate readers only, never generators
                fp = os.path.join(dp, n)
                try:
                    if os.path.getsize(fp) <= TOOLS_PY_MAX:
                        parts.append(tidy_disk.strip_py(open(fp, encoding='utf-8', errors='replace').read()))      # S102 K1
                except OSError:
                    pass
    text = re.sub(r'[\\/]+', '/', '\n'.join(parts)).lower()
    _corpus_cache.append(text)
    return _corpus_cache[0]


def keep_name_re(key):
    return re.compile(r'(?<![\w./-])' + re.escape(key) + r'(?![\w/-]|\.\w)')


def _tool_names(t, key):
    fp = os.path.join(REPO, t.rstrip('./').replace('/', os.sep))
    if not key or not os.path.isfile(fp) or not TOOL_READERS.search(os.path.basename(t)):
        return False
    try:
        text = open(fp, encoding='utf-8', errors='replace').read()
    except OSError:
        return False
    if fp.endswith('.py'):
        text = tidy_disk.strip_py(text)
    return bool(keep_name_re(key).search(re.sub(r'[\\/]+', '/', text).lower()))


def keep_info(d):
    """None when d has no KEEP file, else {'valid': bool (holds now), 'kind': str, 'why': str (first words of its reason)}.
    Valid: a reason + an expiry date still ahead; or a reason that a tool names; or a bare KEEP younger than
    KEEP_BARE_DAYS (a hold from its mtime). Everything else is no hold (reported in --plan)."""
    f = os.path.join(d, 'KEEP')
    try:
        st = os.stat(lp(f))
    except OSError:
        return None
    if not stat.S_ISREG(st.st_mode):
        return None
    ck = (os.path.normcase(os.path.abspath(d)), st.st_mtime, st.st_size)
    if ck in _keep_cache:
        return _keep_cache[ck]
    try:
        with open(lp(f), encoding='utf-8', errors='replace') as fh:
            text = fh.read(4096)
    except OSError:
        text = ''
    reason = text.strip()
    now = time.time()
    info = {'valid': False, 'kind': '', 'why': ' '.join(reason.split())[:70]}
    m = KEEP_UNTIL_RE.search(text)
    if reason and m:
        try:
            until = time.mktime(time.strptime(m.group(1) + ' 23:59:59', '%Y-%m-%d %H:%M:%S'))
        except ValueError:
            until = 0
        info['valid'] = until > now
        info['kind'] = ('expiry %s' if until > now else 'expired %s (no hold)') % m.group(1)
    elif reason:
        corpus = reader_corpus()
        try:
            key = repo_key(d)
        except ValueError:
            key = ''
        low = re.sub(r'[\\/]+', '/', reason).lower()
        # S102 K1 (R12 change 1): the reader names the folder ITSELF (a longer path under it does not count: out/s100 was held for 39.8 GB because a
        # check named out/s100/attires; that child is protected by its own name), and a tool the reason points at must name the folder too.
        named = bool(key) and bool(keep_name_re(key).search(corpus))
        named = named or any(_tool_names(t, key) for t in TOOL_REF_RE.findall(low))
        if named:
            info.update(valid=True, kind='named by a tool')
    if not info['valid'] and not info['kind'].startswith('expired'):
        left = st.st_mtime + KEEP_BARE_DAYS * 86400
        if now < left:
            info.update(valid=True, kind='bare: %d-day hold until %s' % (KEEP_BARE_DAYS, stamp(left)))
        else:
            info['kind'] = 'bare, older than %d days (no hold)' % KEEP_BARE_DAYS
    _keep_cache[ck] = info
    return info


def keep_active(d):
    """S102 TIDY2 (Jarrod, 2026-11-15): a KEEP file no longer protects anything. It is reported ("ignored: move it to tools\\harness\\keep_list.txt
    with a reason"); the keep list is the one place."""
    return False


# ---------------------------------------------------------------- the one pass over out\
def walk_tree(path):
    """(bytes, files, newest, links, KEEP files) without following anything."""
    b = f = n = links = keeps = 0
    stack = [path]
    while stack:
        cur = stack.pop()
        try:
            it = list(os.scandir(lp(cur)))
        except OSError:
            links += 1      # unreadable counts as unsafe
            continue
        for e in it:
            try:
                st = e.stat(follow_symlinks=False)
            except OSError:
                links += 1
                continue
            if st.st_file_attributes & REPARSE:
                links += 1
            elif e.is_dir(follow_symlinks=False):
                stack.append(os.path.join(cur, e.name))
            else:
                b += st.st_size
                f += 1
                n = max(n, born(st))
                keeps += e.name.lower() == 'keep' and keep_active(cur)
    return b, f, n, links, keeps


def in_agent_out(p):
    """p is an agent worktree's out\\ folder or below it (lane 294: the only part of an agent worktree the scratch pass may delete from)."""
    ap = os.path.normcase(os.path.abspath(p))
    root = os.path.normcase(os.path.abspath(AGENT_WT)) + os.sep
    if not ap.startswith(root):
        return False
    parts = ap[len(root):].split(os.sep)
    return len(parts) >= 2 and parts[0].startswith('agent-') and parts[1] == 'out'


def keep_above(path):
    """A KEEP file in any folder above path, up to out\\ or the worktree root."""
    d = os.path.dirname(os.path.abspath(path))
    while is_allowed(d) or in_agent_out(d):
        if keep_active(d):
            return True
        d = os.path.dirname(d)
    return False


CAPTURE_MARKERS = {'images.bin', 'manifest.json'}
RUNS_RE = re.compile(r's\d+/runs(/|$)')
RUNS_DIR_RE = re.compile(r's\d+/runs')
RUN_CHILD_RE = re.compile(r'(s\d+/runs)/([^/]+)')
RUN_NAME_RE = re.compile(r'^(\d{8})_(\d{6})_(.+)$')
RUN_PAIR_MIN = 30       # lane 294 B1: run folders of one route this close to the newest are all "the newest"


def newest_run_names(names):
    """Lower-case names of the newest run folder of each route and every one within RUN_PAIR_MIN of it (case + record dirs, A/B pairs)."""
    routes = {}
    for n in names:
        m = RUN_NAME_RE.match(n.lower())
        if m:
            routes.setdefault(m.group(3), []).append((run_time(m), n.lower()))
    newest = set()
    for lst in routes.values():
        top = max(t for t, _ in lst)
        newest.update(n for t, n in lst if t >= top - RUN_PAIR_MIN * 60)
    return newest


def run_time(m):
    # the stamp is the LOCAL time run.py wrote; read as UTC here, which is fine: only differences between stamps are used
    try:
        return calendar.timegm(time.strptime(m.group(1) + m.group(2), '%Y%m%d%H%M%S'))
    except ValueError:
        return 0


def is_capture_dir(names):
    """A folder holding an unpacked capture (images.bin / manifest.json / case_*.zip): tools re-extract only when images.bin is
    missing, so it must never be half-deleted or deleted while a tool may still read it (lane 154, S3)."""
    return any(n in CAPTURE_MARKERS or (n.startswith('case_') and n.endswith('.zip')) for n in (x.lower() for x in names))


# ---------------------------------------------------------------- worktrees
def list_worktrees(repo):
    if not os.path.isdir(repo):
        return []
    rows, cur = [], {}
    for line in git(repo, 'worktree', 'list', '--porcelain').stdout.splitlines() + ['']:
        if line.startswith('worktree '):
            cur = {'path': os.path.abspath(line[9:]), 'repo': repo}
        elif line.startswith('HEAD '):
            cur['head'] = line[5:]
        elif line.startswith('branch '):
            cur['branch'] = line[7:].replace('refs/heads/', '')
        elif line == '' and cur:
            rows.append(cur)
            cur = {}
    return rows[1:]     # the first is the main tree


def submodule_paths():
    out = set()
    for line in git(SDK, 'config', '-f', '.gitmodules', '--get-regexp', r'submodule\..*\.path').stdout.splitlines():
        p = line.split(None, 1)
        if len(p) == 2:
            out.add(p[1].strip())
    return out


def check_worktree(w, age, protect, carried, subs):
    w.update(state='report', why='', b=0, n=0, t=0, save_ref=False)
    try:
        return check_worktree_inner(w, age, protect, carried, subs)
    except (OSError, subprocess.TimeoutExpired) as e:
        w['why'] = 'check failed: %s' % str(e)[:120]
        return w


def check_worktree_inner(w, age, protect, carried, subs):
    p = w['path']
    if not (is_allowed(p) or wt_allowed(p)):
        w['why'] = 'outside the folders the tidy may touch'
        return w
    if not os.path.isdir(lp(p)):
        w.update(state='gone', why='folder already missing')
        return w
    w['b'], w['n'], w['t'], links, keeps = walk_tree(p)
    key = repo_key(p)
    if key.startswith('out/wt/exe-slot'):      # S106 G2LF F4: lane C4's exe slots (out\wt\exe-slot1..3): an idle detached slot is a cold build to remake, kept like the other worktree keeps
        w.update(state='kept', why="an exe-build slot (out/wt/exe-slot*): the exe lane's build tree")
        return w
    # A state doc naming a parent (out\\harness\\bisect\\wt) protects nothing below it; a deliberate --hold on one does.
    if any(k == key or k.startswith(key + '/') for k in protect) or any(key.startswith(k + '/') for k in carried):
        w.update(state='kept', why='named in a state doc or carried')
        return w
    if keeps or keep_above(p):
        w.update(state='kept', why='KEEP file')
        return w
    if not age.expired(w['t']):
        w.update(state='kept', why='recent activity')
        return w
    if links:
        w['why'] = 'holds a junction or an unreadable item (a build may be running)'
        return w
    owner = p + '.owner'
    if os.path.exists(owner):
        m = re.search(r'pid=(\d+)', open(owner, encoding='utf-8', errors='replace').read())
        if m and pid_alive(int(m.group(1))):
            w.update(state='kept', why='owner process still alive')
            return w
    r = git(p, 'status', '--porcelain')
    if r.returncode:
        w['why'] = 'git status failed: ' + r.stderr.strip()[:120]
        return w
    real = []
    benign = (set() if w.get('agent') else BUILD_SETUP if os.path.normcase(w['repo']) == os.path.normcase(REPO) else subs)
    for line in r.stdout.splitlines():
        path = line[3:].strip().strip('"').rstrip('/')
        if line[:2] != '??' and (path in benign or path.split(' -> ')[-1] in benign):
            continue
        real.append(line.strip())
    if real:
        w['why'] = '%d real edit(s): %s' % (len(real), '; '.join(real[:4]))
        return w
    head = w.get('head', '')
    if not head or not git(w['repo'], 'for-each-ref', '--count=1', '--contains', head).stdout.strip():
        w['save_ref'] = True
    w['state'] = 'remove'
    return w


def agent_infos():
    """Every subagent of this project (lane 154, S4): [{'id', 'tr' (transcript mtime), 'meta_t', 'finished' (None = no flag),
    'has_meta', 'parent' (session id), 'wt' (worktreePath, normcased), 'jsonl', 'meta'}]. Claude Code writes
    subagents\\agent-<id>.meta.json with worktreePath, and "finished": true once the agent ends."""
    out = []
    if not os.path.isdir(PROJECTS):
        return out
    for d in os.scandir(PROJECTS):
        if not d.is_dir() or not (TEST or d.name.startswith(PROJECT_PREFIX)):
            continue
        try:
            sess = list(os.scandir(d.path))
        except OSError:
            continue
        for s_ in sess:
            sub = os.path.join(s_.path, 'subagents')
            if not s_.is_dir() or not os.path.isdir(sub):
                continue
            try:
                names = os.listdir(sub)
            except OSError:
                continue
            ids = set()
            for n in names:
                if n.startswith('agent-') and n.endswith('.jsonl'):
                    ids.add(n[:-6])
                elif n.startswith('agent-') and n.endswith('.meta.json'):
                    ids.add(n[:-10])
            for i in ids:
                jsonl, meta = os.path.join(sub, i + '.jsonl'), os.path.join(sub, i + '.meta.json')
                info = {'id': i, 'tr': 0.0, 'meta_t': 0.0, 'finished': None, 'has_meta': False, 'parent': s_.name, 'wt': '',
                        'jsonl': jsonl, 'meta': meta}
                try:
                    info['tr'] = os.stat(jsonl).st_mtime
                except OSError:
                    pass
                try:
                    info['meta_t'] = os.stat(meta).st_mtime
                    with open(meta, encoding='utf-8', errors='replace') as f:
                        m = json.load(f)
                    info['has_meta'] = True
                    info['finished'] = True if m.get('finished') is True else None
                    if m.get('worktreePath'):
                        info['wt'] = os.path.normcase(os.path.abspath(m['worktreePath']))
                except (OSError, ValueError):
                    pass
                out.append(info)
    return out


def agent_state(infos, live_ids, path, now, hours):
    """-> (last activity of its agent(s), running?, the matching infos). Matched by worktreePath, or by folder name.
    running = a meta without "finished": true while its parent session is live, whatever the mtimes."""
    key = os.path.normcase(os.path.abspath(path))
    name = os.path.basename(path)
    mine = [i for i in infos if i['wt'] == key or i['id'] == name]
    last = max([max(i['tr'], i['meta_t']) for i in mine] or [0.0])
    running = any(i['has_meta'] and i['finished'] is not True and (i['parent'] in live_ids or ANY_PARENT)
                  and max(i['tr'], i['meta_t']) >= now - ORPHAN_META_H * HOUR for i in mine)     # a killed agent does not keep it for ever
    # S2 (lane 187): a resumed agent may keep finished:true in its meta while it works again; its transcript then outruns the meta.
    # Only for a live parent: old transcripts get bulk-touched, and a resume always comes from a live session.
    running = running or any(i['has_meta'] and i['finished'] is True and i['parent'] in live_ids and i['tr'] > i['meta_t'] + 60
                             and i['tr'] >= now - ORPHAN_META_H * HOUR for i in mine)
    return last, running, mine


def session_branch(repo=None):
    r = git(repo or REPO, 'symbolic-ref', '--short', '-q', 'HEAD')
    return r.stdout.strip() if r.returncode == 0 else ''


def is_merged(repo, head, branch):
    """Is head already in the branch checked out in that repo's main checkout? Unknown counts as not merged (kept longer)."""
    return bool(branch and head) and git(repo, 'merge-base', '--is-ancestor', head, branch).returncode == 0


def agent_rows():
    return [w for w in list_worktrees(REPO) if wt_allowed(w['path'])]


def check_agent_worktree(w, now, infos, live_ids, branch, protect, carried, merged_h=AGENT_MERGED_H, unmerged_h=AGENT_UNMERGED_H):
    """Agent worktree rules (lane 154): live while its agent is running (no "finished" in its meta, parent session live) or its
    transcript / meta or any file changed within AGENT_LIVE_H; one whose HEAD is not merged into the session branch waits
    AGENT_UNMERGED_H after its last activity; the rest is check_worktree. No BUILD_SETUP leniency: an uncommitted manifest or
    fast_build edit in an agent worktree is a builder's work (S2)."""
    w['agent'] = True
    merged = is_merged(REPO, w.get('head'), branch)
    hours = merged_h if merged else unmerged_h     # lane 187: the emergency passes cut these to NOW_IDLE_H (a running agent is still kept)
    cutoff = now - hours * HOUR
    check_worktree(w, Age(cutoff), protect, carried, set())
    w['agent'] = True
    w['merged'] = merged
    w['live_hours'] = hours
    last, running, mine = agent_state(infos, live_ids, w['path'], now, hours)
    w['agent_ids'] = [i['id'] for i in mine]
    if w['state'] == 'kept' and w['why'] == 'recent activity':
        w['why'] = ('active within %d h' % hours) if merged else 'active within 0 h: UNMERGED branch (%s), never taken automatically, the main session decides' % (branch or '?')
    if w['state'] in ('remove', 'report') and running:
        w['state'] = 'kept'
        w['save_ref'] = False
        w['why'] = 'its agent is still running (its meta has no finished flag)'
    elif w['state'] in ('remove', 'report') and last >= cutoff:
        w['state'] = 'kept'
        w['save_ref'] = False
        w['why'] = 'its agent wrote a transcript %.0f min ago%s' % ((now - last) / 60, '' if merged else ' (branch not merged: kept %d h)' % hours)
    return w


def check_plain_worktree(w, age, now, protect, carried, subs, branches, infos=(), live_ids=()):
    """sdk-wt and out\\wt worktrees (N3): one whose HEAD is not merged into its repo's checked-out branch waits 24 h."""
    repo = w['repo']
    if repo not in branches:
        branches[repo] = session_branch(repo)
    merged = is_merged(repo, w.get('head'), branches[repo])
    check_worktree(w, age if merged else Age(now - AGENT_UNMERGED_H * HOUR), protect, carried, subs)
    w['merged'] = merged
    if w['state'] in ('remove', 'report') and infos:      # fix 5: a running agent's meta names this worktree: never taken
        _last, running, _m = agent_state(infos, live_ids, w['path'], now, WT_IDLE_H)
        if running:
            w.update(state='kept', save_ref=False, why='a running agent works in it (its meta has no finished flag)')
    return w


def check_worktrees(age, protect, now=None, live_ids=None, merged_h=AGENT_MERGED_H, unmerged_h=AGENT_UNMERGED_H, agents_only=False):
    """live_ids: ids of the sessions live right now (--ahead passes the real ones: a forecast assumes the session goes on)."""
    now = now or time.time()
    rows = list_worktrees(REPO) + list_worktrees(SDK)
    rows = [w for w in rows if is_allowed(w['path'])]
    if agents_only:
        rows = []
    subs = submodule_paths() if os.path.isdir(SDK) else set()
    carried = carried_keys()
    arows = agent_rows()
    infos, branch = [], ''
    if arows or rows:
        infos = agent_infos()
        live_ids = live_ids if live_ids is not None else {s_[0] for s_ in sessions(now)}
    if arows:
        branch = session_branch()
    branches = {}
    with ThreadPoolExecutor(8) as pool:
        res = list(pool.map(lambda w: check_plain_worktree(w, age, now, protect, carried, subs, branches, infos, live_ids), rows))
        res += list(pool.map(lambda w: check_agent_worktree(w, now, infos, live_ids, branch, protect, carried, merged_h, unmerged_h), arows))
    return res


# ---------------------------------------------------------------- deleting
class Deleter:
    def __init__(self, guard_t):
        self.guard = guard_t       # a file created or changed after this moment is left alone
        self.pool = ThreadPoolExecutor(WORKERS)
        self.lock = threading.Lock()
        self.bytes = self.files = self.skipped = 0
        self.errors = []
        self.clean_dirs = set()
        self.agent_cache = None
        self.carried = carried_keys()
        held_index()      # S102 TIDY2: sets HELD, the last backstop (protected_zone)

    def err(self, p, e):
        with self.lock:
            if len(self.errors) < 200:
                self.errors.append('%s: %s' % (p, e))

    def unlink(self, p, size):
        try:
            os.unlink(lp(p))
        except PermissionError:
            try:
                os.chmod(lp(p), stat.S_IWRITE)
                os.unlink(lp(p))
            except OSError as e:
                self.err(p, e)
                return False
        except OSError as e:
            self.err(p, e)
            return False
        with self.lock:
            self.bytes += size
            self.files += 1
        return True

    def clear_dir(self, d):
        subs = []
        try:
            if os.lstat(lp(d)).st_file_attributes & REPARSE:        # N1: a junction made after the check (fast_build's New-Junction)
                self.err(d, 'junction or link appeared: left alone')
                return subs
            entries = list(os.scandir(lp(d)))
        except OSError as e:
            self.err(d, e)
            return subs
        if any(e.name.lower() == 'keep' for e in entries) and keep_active(d):
            with self.lock:
                self.skipped += 1
            return subs
        if os.path.normcase(os.path.dirname(os.path.abspath(d))) == os.path.normcase(os.path.join(os.path.abspath(OUT), 'exe')):
            # S106 G2LF F1: an out/exe/<name> folder. A game may have started after the scan: re-read the busy set now (skip the whole folder), and probe every
            # top-level .exe / .dll with a rename round trip BEFORE anything is deleted (a loaded image cannot be renamed: the folder is then left untouched).
            if os.path.normcase(os.path.abspath(d)) in {os.path.normcase(x) for x in busy_exe_dirs()}:
                with self.lock:
                    self.skipped += 1
                self.err(d, 'an exe in it is running: folder left whole')
                return subs
            for e in entries:
                if e.name.lower().endswith(('.exe', '.dll')) and not e.is_dir(follow_symlinks=False):
                    src = os.path.join(d, e.name)
                    try:
                        os.rename(lp(src), lp(src + '.tidychk'))
                        os.rename(lp(src + '.tidychk'), lp(src))
                    except OSError as ex:
                        self.err(d, 'an image in it is in use (%s: %s): folder left whole' % (e.name, ex))
                        return []
        # (d) never delete part of a folder: capture markers go first (tools re-extract only when images.bin is missing), and the
        # first file that cannot be removed (open, locked) stops this folder: the rest stays, and so do its subfolders.
        entries.sort(key=lambda e: (e.name.lower() not in CAPTURE_MARKERS and not (e.name.lower().startswith('case_') and e.name.lower().endswith('.zip')),
                                    e.name == '.git', e.name.lower()))      # .git LAST: a partial worktree delete stays registered and is retried
        for e in entries:
            child = os.path.join(d, e.name)
            try:
                st = e.stat(follow_symlinks=False)
            except OSError as ex:
                self.err(child, ex)
                continue
            if st.st_file_attributes & REPARSE:
                self.err(child, 'junction or link: left alone')
                continue
            if e.is_dir(follow_symlinks=False):
                subs.append(child)
            elif born(st) >= self.guard:
                with self.lock:
                    self.skipped += 1
            elif not self.unlink(child, st.st_size):
                self.err(d, 'a file could not be removed: the rest of this folder and its subfolders stay')
                return []
        return subs

    def folders(self, roots):
        roots = [r for r in roots if self.safe(r)]
        order, level = [], roots
        while level:
            order.append(level)
            level = [s for subs in self.pool.map(self.clear_dir, level) for s in subs]
        for lv in reversed(order):
            list(self.pool.map(self.rmdir, lv))

    def rmdir(self, d):
        try:
            os.rmdir(lp(d))
        except OSError:
            pass        # not empty: a guarded or locked file stayed

    def safe(self, p):
        if not (is_allowed(p) or wt_allowed(p) or in_agent_out(p) or in_scratchpad(p)) or self.protected_zone(p):      # S102 K1: in_scratchpad
            self.err(p, 'refused: outside the allowed folders or on the never-touch list')
            return False
        if self.link_on_path(p):
            self.err(p, 'refused: a junction or link at or above it')
            return False
        key = repo_key(p)
        if any(key == k or key.startswith(k + '/') for k in self.carried) or keep_above(p):
            with self.lock:
                self.skipped += 1
            return False
        return True

    def link_on_path(self, p):
        """True when p, or any folder from it up to the allowed root, is a junction or link."""
        roots = {os.path.normcase(os.path.abspath(r)) for r in ALLOWED_ROOTS + [AGENT_WT]}
        d = os.path.abspath(p)
        while True:
            if d not in self.clean_dirs:
                try:
                    if os.lstat(lp(d)).st_file_attributes & REPARSE:
                        return True
                except OSError:
                    return True     # vanished or unreadable: nothing to delete safely
                if d != os.path.abspath(p):
                    self.clean_dirs.add(d)
            if os.path.normcase(d) in roots or os.path.dirname(d) == d:
                return False
            d = os.path.dirname(d)

    @staticmethod
    def protected_zone(p):
        if not os.path.normcase(os.path.abspath(p)).startswith(os.path.normcase(OUT) + os.sep):
            return False
        r = out_rel(p)
        if any(r == n or r.startswith(n + '/') for n in NO_TOUCH):
            return True
        if r.startswith('wt/'):
            return False       # out\wt git worktrees are the worktree pass's (finished + merged go at once; real edits are reported only)
        if HELD is not None:       # S102 TIDY2: the last backstop, whatever the pass: a held path, or a folder that holds one, is never deleted
            return bool(HELD.why('out/' + r)) or HELD.ancestor('out/' + r)
        return False

    def file(self, row):
        p = row['p']
        if not self.safe(p):
            return
        try:
            st = os.stat(lp(p), follow_symlinks=False)
        except OSError:
            return
        if born(st) >= self.guard:
            with self.lock:
                self.skipped += 1
            return
        self.unlink(p, st.st_size)

    def remove(self, items, worktrees, phantoms=()):
        files = [i for i in items if not i['dir']]
        list(self.pool.map(self.file, files))
        self.folders([i['p'] for i in items if i['dir']])
        gone = [w for w in worktrees if self.worktree(w)]
        self.unlock_prune(gone + [w for w in phantoms if w.get('state') == 'gone'])
        self.pool.shutdown()

    @staticmethod
    def unlock_prune(rows):
        """N2: Claude Code locks agent worktrees, and prune skips a locked entry: unlock a removed path first."""
        for w in rows:
            # git creates the lock before the folder: a live, just-created worktree must keep Claude Code's lock
            if not os.path.isdir(lp(w['path'])):
                git(w['repo'], 'worktree', 'unlock', w['path'])
        for repo in {w['repo'] for w in rows}:
            git(repo, 'worktree', 'prune')

    def agent_active_now(self, w):
        """S4: re-read the agent's transcript / meta right before deleting. A reason string, or ''."""
        now = time.time()
        if not self.agent_cache or now - self.agent_cache[0] > 5:
            self.agent_cache = (now, agent_infos(), {s_[0] for s_ in sessions(now)})
        _, infos, live_ids = self.agent_cache
        last, running, _ = agent_state(infos, live_ids, w['path'], now, w.get('live_hours', AGENT_LIVE_H))
        if running:
            return 'its agent is running again (no finished flag)'
        if last >= now - w.get('live_hours', AGENT_LIVE_H) * HOUR:
            return 'its agent wrote a transcript or meta %.0f min ago' % ((now - last) / 60)
        return 

    def worktree(self, w):
        """Re-checks one worktree right before deleting it. True when it went."""
        p = w['path']
        if not (is_allowed(p) or wt_allowed(p)):
            self.err(p, 'refused: outside the allowed folders')
            return False
        if self.link_on_path(p):
            self.err(p, 'refused: a junction or link at or above it')
            return False
        _, _, t, links, keeps = walk_tree(p)
        if links or keeps or t >= self.guard:
            self.err(p, 'worktree changed since it was checked: left alone')
            return False
        if w.get('agent'):
            why = self.agent_active_now(w)
            if why:
                self.err(p, 'agent worktree left alone: ' + why)
                return False
        if w.get('save_ref') and w.get('head'):
            name = re.sub(r'[^A-Za-z0-9_.-]', '_', os.path.basename(p))
            if git(w['repo'], 'update-ref', 'refs/tidy/%s_%s' % (name, w['head'][:8]), w['head']).returncode:
                self.err(p, 'could not save its commits: left alone')
                return False
        self.folders([p])
        try:
            if os.path.exists(p + '.owner'):
                os.unlink(p + '.owner')
        except OSError:
            pass
        return True


# ---------------------------------------------------------------- agent worktree out\ scratch (lane 294)
# ---------------------------------------------------------------- Claude scratchpads (S102 K1, R12 changes 4 and 5)
def project_session_ids():
    """Ids of every session transcript of this project (live or not)."""
    ids = set()
    if TEST and not os.environ.get('TIDY_TEST_PROJECTS'):
        return {x[0] for x in json.loads(os.environ.get('TIDY_TEST_LIVE', '[]'))} | set(os.environ.get('TIDY_TEST_IDS', '').split(','))
    try:
        for d in os.scandir(PROJECTS):
            if d.name.startswith(PROJECT_PREFIX) and d.is_dir():
                ids.update(e.name[:-6] for e in os.scandir(d.path) if e.name.endswith('.jsonl'))
    except OSError:
        pass
    return ids


def scratchpad_sessions():
    """[(session id, Temp\\claude\\<hash>\\<id> folder)] for this project's sessions, live or not. Never another project's folder."""
    root = os.environ.get('TIDY_TEST_SCRATCHPADS')
    if root:
        return [(e.name, e.path) for e in os.scandir(root) if e.is_dir()] if os.path.isdir(root) else []
    if TEST:
        return []
    if not os.environ.get('LOCALAPPDATA'):      # S102 K1: never glob a relative 'Temp\\claude'
        return []
    base = os.path.join(os.environ['LOCALAPPDATA'], 'Temp', 'claude')
    ids = project_session_ids()
    return [(os.path.basename(g), g) for g in glob.glob(os.path.join(glob.escape(base), '*', '*')) if os.path.basename(g) in ids and os.path.isdir(g)]


def in_scratchpad(p):
    """p is below <session folder>\\scratchpad of one of this project's sessions (a path the tidy may delete under, through scratchpad_pass only)."""
    ap = os.path.normcase(os.path.abspath(p))
    return any(ap.startswith(os.path.normcase(os.path.abspath(os.path.join(g, 'scratchpad'))) + os.sep) for _, g in scratchpad_sessions())


def scratchpad_pass(now, dry=False):
    """R12 change 5: a scratchpad child of a session that is NOT live goes whole when its newest file is older than SCRATCHPAD_CHILD_H
    (never a junction, never a folder with an active KEEP). Live sessions only get a LOUD line when over SCRATCHPAD_LOUD_GB. The session
    folder's own tasks / images are left. -> (bytes freed or that would be, lines)."""
    live_ids = {s_[0] for s_ in sessions(now)}
    d = Deleter(now)
    total, lines = 0, []
    try:
        for sid, g in scratchpad_sessions():
            sp = os.path.join(g, 'scratchpad')
            if not os.path.isdir(sp):
                continue
            if d.link_on_path(sp) or keep_active(sp) or keep_above(sp):      # S102 K1: a junction at or above it, or an active KEEP: left alone
                continue
            if sid in live_ids:
                b = walk_tree(sp)[0]
                if b > SCRATCHPAD_LOUD_GB * GB:
                    lines.append('LOUD: live session %s scratchpad is %s (limit %d GB)' % (sid[:8], human(b), SCRATCHPAD_LOUD_GB))
                continue
            got = 0
            guard = now - SCRATCHPAD_CHILD_H * HOUR
            for e in sorted(os.scandir(lp(sp)), key=lambda e: e.name.lower()):
                child = os.path.join(sp, e.name)
                try:
                    st = e.stat(follow_symlinks=False)
                except OSError:
                    continue
                if st.st_file_attributes & REPARSE:
                    continue
                if e.is_dir(follow_symlinks=False):
                    if keep_active(child):
                        continue
                    b, newest, links, build, keeps = child_info(child)
                    if links or keeps or newest >= guard:
                        continue
                    if dry:
                        got += b
                    else:
                        b0 = d.bytes
                        d.folders([child])
                        got += d.bytes - b0
                elif born(st) < guard and d.safe(child):      # S102 K1: loose files go through the same refusals
                    if dry:
                        got += st.st_size
                    else:
                        b0 = d.bytes
                        d.unlink(child, st.st_size)
                        got += d.bytes - b0
            total += got
            if got > 0.05 * GB:
                lines.append('%s scratchpad of idle session %s: %s (children idle %d h)' % ('would clear' if dry else 'cleared', sid[:8], human(got), SCRATCHPAD_CHILD_H))
    finally:
        d.pool.shutdown()
    if total and not dry:
        history('scratchpad: cleared %s from idle sessions' % human(total))
    return total, lines


def scratch_dirs():
    """[(agent worktree row, its out\\ folder)] for registered agent worktrees whose out is a real folder (never a junction)."""
    rows = []
    for w in agent_rows():
        o = os.path.join(w['path'], 'out')
        try:
            st = os.lstat(lp(o))
        except OSError:
            continue
        if st.st_file_attributes & REPARSE or not stat.S_ISDIR(st.st_mode):
            continue
        rows.append((w, o))
    return rows


def tracked_under_out(wt):
    r = git(wt, 'ls-files', '-z', '--', 'out')
    return {x.replace('\\', '/').lower() for x in r.stdout.split('\0') if x} if r.returncode == 0 else None


BUILD_MARKERS = {'cmakecache.txt', 'build.ninja', '.ninja_log'}
BUILD_EXT = {'.exe', '.obj', '.pdb', '.dll', '.lib'}      # S102 K1: a folder holding compiled output is a builder's tree (CLAUDE.md: build trees excepted)


def child_info(path):
    """(bytes, newest, links, has build files, active KEEP files) under path, nothing followed."""
    b = n = links = keeps = 0
    build = False
    stack = [path]
    while stack:
        cur = stack.pop()
        try:
            it = list(os.scandir(lp(cur)))
        except OSError:
            links += 1
            continue
        for e in it:
            try:
                st = e.stat(follow_symlinks=False)
            except OSError:
                links += 1
                continue
            if st.st_file_attributes & REPARSE:
                links += 1
            elif e.is_dir(follow_symlinks=False):
                stack.append(os.path.join(cur, e.name))
            else:
                b += st.st_size
                n = max(n, born(st))
                nm = e.name.lower()
                build = build or nm in BUILD_MARKERS or os.path.splitext(nm)[1] in BUILD_EXT
                keeps += nm == 'keep' and keep_active(cur)
    return b, n, links, build, keeps


def scratch_skip_reason(w, o, d):
    """Why this worktree's out\\ must not be touched, or ''. The checks Deleter.safe() makes for any other delete (lane 294 B2)."""
    p = w['path']
    if not wt_allowed(p):
        return 'not an allowed agent worktree'
    key = repo_key(p)
    if any(key == k or key.startswith(k + '/') for k in d.carried):
        return 'held (--hold)'
    owner = p + '.owner'
    if os.path.exists(owner):
        m = re.search(r'pid=(\d+)', open(owner, encoding='utf-8', errors='replace').read())
        if m and pid_alive(int(m.group(1))):
            return 'owner process alive'
    if keep_active(p) or walk_tree(p)[4]:
        return 'KEEP file in the worktree'
    if d.link_on_path(o):
        return 'junction or link at or above out\\'
    return ''


def clear_scratch(o, wt, guard_t, d, dry, skip_build, only=None):
    """Lane 294 B3: works per top-level child of the worktree's out\\. A folder goes WHOLE and only when its newest file is older than
    guard_t (through Deleter.folders: capture markers first, stops on the first file that cannot go), never a half capture; skip_build
    leaves any folder holding CMakeCache.txt / build.ninja / .ninja_log (a live agent's build). Tracked files, junctions and active KEEP
    stay. -> (bytes freed or that would be, errors)."""
    tracked = tracked_under_out(wt)
    if tracked is None:
        return 0, ['git ls-files failed in ' + wt]
    freed = 0
    try:
        kids = sorted(os.scandir(lp(o)), key=lambda e: e.name.lower())
    except OSError as e:
        return 0, [str(e)]
    for e in kids:
        child = os.path.join(o, e.name)
        try:
            st = e.stat(follow_symlinks=False)
        except OSError:
            continue
        crel = 'out/' + e.name.lower()
        if only is not None and e.name not in only:      # S102 K1: the quota clears only the biggest children
            continue
        if st.st_file_attributes & REPARSE or any(t == crel or t.startswith(crel + '/') for t in tracked):
            continue
        if e.is_dir(follow_symlinks=False):
            if keep_active(child):
                continue
            b, newest, links, build, keeps = child_info(child)
            if links or keeps or newest >= guard_t or (skip_build and build):
                continue
            if dry:
                freed += b
            else:
                b0 = d.bytes
                d.folders([child])
                freed += d.bytes - b0
        elif born(st) < guard_t:
            if dry:
                freed += st.st_size
            else:
                b0 = d.bytes
                d.unlink(child, st.st_size)
                freed += d.bytes - b0
    return freed, ([] if dry else list(d.errors))


HELD = None      # S102 TIDY2: the HeldIndex of the running pass (Deleter.protected_zone)
PRESSURE_FORCE = False      # S102 K1: the watch sets this when free space falls fast (reserve in under 2 h): scratch_pass treats C: as under pressure


def wt_quota_gb(wt):
    """An agent worktree's out\\ quota in GB: SCRATCH_QUOTA_GB, or the number in <worktree>\\.tidy_quota (a brief may raise it)."""
    try:
        v = float(open(os.path.join(wt, '.tidy_quota'), encoding='utf-8').read().split()[0])
        return v if v > 0 else SCRATCH_QUOTA_GB
    except (OSError, ValueError, IndexError):
        return SCRATCH_QUOTA_GB


def quota_pick(o, quota_b, guard_t):
    """Names of the biggest top-level children of out\\ (idle since guard_t, no links, no build files, no active KEEP) whose removal
    brings it under quota_b. -> (names, total bytes of the folder)."""
    rows, total = [], 0
    try:
        for e in os.scandir(lp(o)):
            if e.is_dir(follow_symlinks=False):
                b, newest, links, build, keeps = child_info(os.path.join(o, e.name))
                total += b
                if not (links or keeps or build or newest >= guard_t or keep_active(os.path.join(o, e.name))):
                    rows.append((b, e.name))
            else:
                try:
                    total += e.stat(follow_symlinks=False).st_size
                except OSError:
                    pass
    except OSError:
        return [], 0
    names = []
    for b, n in sorted(rows, reverse=True):
        if total <= quota_b:
            break
        names.append(n)
        total -= b
    return names, total


def scratch_pass(now, dry=False, free=None, skip=()):
    """Rule 3. -> (bytes freed or that would be, lines). Finished agent (meta flag, no later transcript): its out\\ goes once its newest
    file is older than NOW_IDLE_H (merged branch) or AGENT_UNMERGED_H (unmerged: a review may still need it) - S3. Any agent: out\\ over
    SCRATCH_REPORT_GB is reported loudly, and while C: has under SCRATCH_PRESSURE_GB free every top-level folder whose newest file is older
    than SCRATCH_RECENT_MIN goes (pressure overrides the waits), a live agent's build folders excepted. Same refusals as any delete (B2)."""
    free = free_gb() if free is None else free
    if PRESSURE_FORCE:
        free = 0.0
    skip = {os.path.normcase(x) for x in skip}
    rows = [(w, o) for w, o in scratch_dirs() if os.path.normcase(w['path']) not in skip]
    if not rows:
        return 0, []
    infos = agent_infos()
    live_ids = {s_[0] for s_ in sessions(now)}
    branch = session_branch()
    with ThreadPoolExecutor(8) as pool:
        sizes = list(pool.map(lambda r: walk_tree(r[1])[0], rows))
    d = Deleter(now)
    lines, total, errs = [], 0, []
    try:
        for (w, o), b in zip(rows, sizes):
            name = os.path.basename(w['path'])
            only = None
            over = b > SCRATCH_REPORT_GB * GB
            quota = wt_quota_gb(w['path'])
            if over:
                lines.append('LOUD: %s out\\ scratch is %s (limit %d GB)' % (name, human(b), SCRATCH_REPORT_GB))
            elif b > quota * GB / 2:      # report only: the quota deletes nothing of a running agent
                lines.append('LOUD: %s out\\ scratch is %s (quota %.0f GB%s)' % (name, human(b), quota, ', over' if b > quota * GB else ''))
            last, running, mine = agent_state(infos, live_ids, w['path'], now, AGENT_LIVE_H)
            resumed = any(i['tr'] > i['meta_t'] + 60 and i['tr'] >= now - AGENT_LIVE_H * HOUR for i in mine)
            finished = bool(mine) and not running and not resumed and all(i['finished'] is True for i in mine)
            if b == 0:
                continue
            if free < SCRATCH_PRESSURE_GB and over:
                guard, why = now - SCRATCH_RECENT_MIN * 60, 'over %d GB and C: under %d GB free' % (SCRATCH_REPORT_GB, SCRATCH_PRESSURE_GB)
            elif finished:
                # a builder that never committed has HEAD == the session branch: merged only when it also has no uncommitted tracked edits
                clean = git(w['path'], 'status', '--porcelain', '--untracked-files=no').stdout.strip() == ''
                idle = NOW_IDLE_H if (clean and is_merged(REPO, w.get('head'), branch)) else AGENT_UNMERGED_H
                guard, why = now - idle * HOUR, 'agent finished'
                if walk_tree(o)[2] >= guard:        # S3: a reviewer or the main session may be using it
                    # S102 K1 (R12 change 6): a FINISHED agent over its quota still loses its biggest idle (15 min) folders; a running agent
                    # is only ever touched under pressure (CLAUDE.md), never by the quota
                    if b <= quota * GB:
                        continue
                    guard, why = now - SCRATCH_RECENT_MIN * 60, 'finished agent over its %.0f GB quota' % quota
                    only, _left = quota_pick(o, int(quota * GB), guard)
                    if not only:
                        continue
            else:
                continue
            r = scratch_skip_reason(w, o, d)
            if r:
                lines.append('skipped %s out\\ scratch: %s' % (name, r))
                continue
            got, e = clear_scratch(o, w['path'], guard, d, dry, skip_build=not finished, only=only)
            total += got
            errs += e
            if got > 0.05 * GB:
                lines.append('%s out\\ scratch of %s: %s (%s)' % ('would clear' if dry else 'cleared', name, human(got), why))
    finally:
        d.pool.shutdown()
    if errs and not dry:
        lines.append('scratch: %d refusals / errors (first: %s)' % (len(errs), errs[0][-110:]))
        st = load_json('state.json', {})
        if now - st.get('scratch_err_t', 0) > HOUR:     # a watch that keeps failing on locked files writes this at most hourly
            history('scratch ERRORS (%d): %s' % (len(errs), '; '.join(x[-90:] for x in errs[:3])))
            st['scratch_err_t'] = now
            save_json('state.json', st)
    if total and not dry:
        history('scratch: cleared %s from agent worktree out\\ folders' % human(total))
    return total, lines


WT_INDEX_FILES = ('ours.db', 'ours.db-wal', 'ours.db-shm', 'ours.db-journal')   # ORDER MATTERS: the db goes first, its sidecars after (a journal without its db is harmless; the reverse is not)


def worktree_index_pass(dry=False):
    """QTF (S103 WTA fix 3 / QTF 6): a worktree's private out/harness/index/ours.db is never kept. `q ours` in a worktree always reads the MAIN checkout's
    index now (qours.db_path ignores the private copy), so these ~200 MB files (16 of 115 worktrees had one) are pure waste and the source of the 10-minute
    re-indexes. Each one goes through guard_paths (an agent that is running or was active recently is refused and tried again at the next pass).
    -> (bytes freed or that would be, lines)."""
    cands = []
    for _w, o in scratch_dirs():
        for n in WT_INDEX_FILES:
            f = os.path.join(o, 'harness', 'index', n)
            if os.path.isfile(lp(f)):
                cands.append(f)
    if not cands:
        return 0, []
    ok, lines, total = [], [], 0
    for f, why in guard_paths(cands):
        if why:
            lines.append('worktree index kept (%s): %s' % (why, f))
        else:
            ok.append(f)
    d = Deleter(time.time())
    try:
        for f in ok:
            try:
                sz = os.path.getsize(lp(f))
            except OSError:
                continue
            if dry or d.unlink(f, sz):
                total += sz
                lines.append('worktree index %s: %s (%s)' % ('would go' if dry else 'deleted', f, human(sz)))
    finally:
        d.pool.shutdown()
    if total and not dry:
        history('worktree private ours.db deleted: %s' % human(total))
    return total, lines


def safe_scratch(now, dry=False, free=None, skip=()):
    """scratch_pass that can never stop the pass around it (a git timeout, a vanished folder)."""
    t, L = 0, []
    try:
        t, L = scratch_pass(now, dry, free, skip)
    except Exception as e:
        history('scratch pass FAILED: %r' % (e,))
        L = ['scratch pass failed: %s' % repr(e)[:100]]
    try:
        t3, L3 = worktree_index_pass(dry)
        t, L = t + t3, L + L3
    except Exception as e:
        history('worktree index pass FAILED: %r' % (e,))
    try:
        t2, L2 = scratchpad_pass(now, dry) if SCRATCHPAD_PASS else (0, [])      # S102 K1: OFF by default
        t, L = t + t2, L + L2
    except Exception as e:
        history('scratchpad pass FAILED: %r' % (e,))
        L = L + ['scratchpad pass failed: %s' % repr(e)[:100]]
    return t, L


def has_agent_out():
    """Cheap: does any agent worktree folder have an out\\ at all (no git, no walk)?"""
    try:
        if SCRATCHPAD_PASS and any(os.path.isdir(os.path.join(g, 'scratchpad')) for _, g in scratchpad_sessions()):      # S102 K1
            return True
        return any(e.name.startswith('agent-') and os.path.isdir(os.path.join(e.path, 'out')) for e in os.scandir(AGENT_WT))
    except OSError:
        return False


def cmd_scratch(dry=False):
    now = time.time()
    total, lines = safe_scratch(now, dry)
    print('\n'.join(lines + ['tidy --scratch%s: %s %s' % (' --dry' if dry else '', 'would clear' if dry else 'cleared', human(total))]))
    return 0


def top_level_sizes():
    """[(label, bytes)]: each child of out\\, each agent worktree, each sdk-wt worktree, the Claude scratchpad folder."""
    cands = []

    def kids(root, prefix, only=None):
        try:
            for e in os.scandir(lp(root)):
                if e.is_symlink() or (only and not e.name.startswith(only)):
                    continue
                if e.stat(follow_symlinks=False).st_file_attributes & REPARSE:
                    continue
                cands.append((prefix + e.name, os.path.join(root, e.name), e.is_dir(follow_symlinks=False)))
        except OSError:
            pass
    kids(OUT, 'out\\')
    kids(AGENT_WT, '.claude\\worktrees\\', 'agent-')
    for r in WT_ROOTS:
        kids(r, os.path.relpath(r, REPO) + '\\')
    sp = os.environ.get('TIDY_TEST_SCRATCH')
    if sp:
        kids(sp, 'scratchpad\\')
    else:      # S102 K1: EVERY session of this project, live or not (an idle session's 57 GB used to leave the budget line)
        for sid, g in scratchpad_sessions():
            cands.append(('scratchpad\\' + sid[:8], g, True))

    def size(c):
        if c[2]:
            return c[0], walk_tree(c[1])[0]
        try:
            return c[0], os.stat(lp(c[1])).st_size
        except OSError:
            return c[0], 0
    with ThreadPoolExecutor(WORKERS) as pool:
        return list(pool.map(size, cands))


def budget_lines():
    """Rule 5: the project's total against BUDGET_GB (out\\, worktrees, sdk-wt, scratchpad) and the top 5 folders."""
    try:
        rows = top_level_sizes()
    except Exception as e:      # never let the report break a pass
        return ['size budget: could not measure (%s)' % repr(e)[:80]]
    grp = collections.Counter()
    for k, b in rows:
        grp['out' if k.startswith('out\\') else 'agent worktrees' if k.startswith('.claude') else 'scratchpad' if k.startswith('scratchpad') else 'sdk-wt'] += b
    total = sum(grp.values())
    over = total / GB - BUDGET_GB
    L = ['size budget: %s of ~%d GB (%s)%s' % (human(total), BUDGET_GB, ', '.join('%s %s' % (k, human(v)) for k, v in grp.most_common()),
                                               ' - OVER by %.0f GB' % over if over > 0 else ' - within budget')]
    L.append('  top 5: ' + ', '.join('%s %s' % (k, human(b)) for k, b in sorted(rows, key=lambda r: -r[1])[:5]))
    sp = [(k, b) for k, b in rows if k.startswith('scratchpad')]
    if sp:      # S102 K1: the Claude scratchpads are measured and reported on every pass; deleting in them needs Jarrod's OK (SCRATCHPAD_PASS)
        L.append('  scratchpads: %s over %d sessions; delete pass %s' % (human(sum(b for _, b in sp)), len(sp),
                                                                       'ON' if SCRATCHPAD_PASS else 'OFF (reported only)'))
        for k, b in sp:
            if b > SCRATCHPAD_LOUD_GB * GB:
                L.append('  LOUD: %s is %s (limit %d GB)' % (k, human(b), SCRATCHPAD_LOUD_GB))
    return L


# ---------------------------------------------------------------- reporting
def human(b):
    return '%.1f GB' % (b / GB)


def summarize(items, wts):
    area, types = collections.Counter(), collections.Counter()
    for i in items:
        parts = out_rel(i['p']).split('/')
        area[parts[0]] += i['b']
        for k, v in i['types'].items():
            types[k] += v
    for w in wts:
        if w['state'] == 'remove':
            types['worktree'] += w['b']
            parts = repo_key(w['path']).split('/')
            area['/'.join(parts[:2])] += w['b']
    return area, types


def agent_summary(wts):
    """Lines about the .claude\\worktrees agent worktrees: live, merged / unmerged split (lane 154)."""
    ph = [w for w in wts if w.get('agent') and w['state'] == 'gone']
    a = [w for w in wts if w.get('agent') and w['state'] != 'gone']
    if not a and not ph:
        return []
    mg = [w for w in a if w['merged']]
    um = [w for w in a if not w['merged']]
    rm = [w for w in a if w['state'] == 'remove']
    live = [w for w in a if w['state'] == 'kept' and ('transcript' in w['why'] or 'still running' in w['why'] or w['why'].startswith('active within'))]
    other = [w for w in a if w['state'] in ('report', 'kept') and w not in live]
    gb = lambda ws: human(sum(w['b'] for w in ws))
    L = ['agent worktrees (.claude\\worktrees): %d, %s: merged into the session branch %d (%s), not merged %d (%s)' % (
        len(a), gb(a), len(mg), gb(mg), len(um), gb(um)),
        '  would remove %d (%s): merged %d, not merged %d' % (len(rm), gb(rm), sum(1 for w in rm if w['merged']), sum(1 for w in rm if not w['merged'])),
        '  live / waiting (%d, %s): merged %d, not merged %d' % (len(live), gb(live), sum(1 for w in live if w['merged']), sum(1 for w in live if not w['merged'])),
        '  left alone for other reasons (%d, %s): real edits, KEEP, junction, held' % (len(other), gb(other)),
        '  phantom entries (registered in git, folder already gone; unlocked and pruned on the next pass): %d' % len(ph)]
    return L


BIG_HELD_GB = 5     # a held path over this is flagged "big: confirm with Jarrod"


def held_sizes(sw):
    """[(bytes, rel, why, path)] biggest first: the held roots the sweep met, sized (threaded)."""
    def one(r):
        rel, why, path = r
        try:
            if os.path.isdir(lp(path)):
                return tidy_out.stats(path)[0], rel, why, path
            return os.stat(lp(path)).st_size, rel, why, path
        except OSError:
            return 0, rel, why, path
    with ThreadPoolExecutor(WORKERS) as pool:
        return sorted(pool.map(one, sw.rows), key=lambda r: -r[0])


def report_lines(sw, wts, live, problems, held):
    items = sw.items
    area, types = summarize(items, wts)
    total = sum(i['b'] for i in items)
    L = ['tidy plan %s. out\\ is SCRATCH: at the end of a session (and when no session is live) everything in it goes except the allowlist, '
         'tools\\harness\\keep_list.txt and exact paths a check script names. No ages. While a session is live out\\ is left alone (only --now under the reserve).' % stamp(),
         'live sessions: ' + (', '.join('%s (since %s)' % (s_[0][:8], stamp(s_[1])) for s_ in live) or 'none'),
         'C: free now: %.0f GB' % free_gb(), '',
         'WOULD DELETE AT SESSION END: %s (%d items)' % (human(total), len(items)), '']
    L += ['by folder:'] + ['  %8s  %s' % (human(v), k) for k, v in area.most_common(40)]
    hs = held_sizes(sw)
    L += ['', 'HELD in out\\ (never deleted: %s over %d paths):' % (human(sum(r[0] for r in hs)), len(hs))]
    for b, rel, why, _p in hs:
        L.append('  %8s  %s  [%s]%s' % (human(b), rel, why[:110], '  BIG: confirm with Jarrod' if b > BIG_HELD_GB * GB else ''))
    if sw.notes['keeps']:
        L += ['', 'KEEP files found (%d): ignored, they no longer protect anything. Move the path to tools\\harness\\keep_list.txt with a reason:' % len(sw.notes['keeps'])]
        L += ['  ' + os.path.relpath(k, REPO) for k in sw.notes['keeps'][:30]]
    if problems:
        L += ['', 'keep list / lint problems:'] + ['  ' + x for x in problems]
    if sw.notes['worktrees']:
        L += ['', 'worktree folders in out\\ left to the worktree pass: ' + ', '.join(os.path.relpath(x, OUT) for x in sw.notes['worktrees'][:10])]
    rm = [w for w in wts if w['state'] == 'remove']
    L += ['', 'worktrees removed (%d, %s). Their branches stay; commits on no branch are saved under refs/tidy/:' % (
        len(rm), human(sum(w['b'] for w in rm)))]
    L += ['  %8s  %s  [%s]%s%s' % (human(w['b']), repo_key(w['path']), w.get('branch') or 'detached',
                                   '  (commits saved to refs/tidy)' if w.get('save_ref') else '',
                                   '  (merged)' if w.get('merged') else '') for w in sorted(rm, key=lambda x: -x['b'])]
    L += [''] + agent_summary(wts)
    um = [w for w in wts if w.get('agent') and not w.get('merged') and w['state'] != 'gone']
    if um:
        L += ['  UNMERGED agent worktrees (never taken automatically; the main session decides): ' + ', '.join(
            '%s %s' % (os.path.basename(w['path']), human(w['b'])) for w in sorted(um, key=lambda x: -x['b'])[:12])]
    other = [w for w in wts if w['state'] in ('report', 'kept')]
    L += ['', 'worktrees NOT removed (%d, %s):' % (len(other), human(sum(w['b'] for w in other)))]
    L += ['  %8s  %s  %s: %s' % (human(w['b']), repo_key(w['path']), w['state'], w['why']) for w in sorted(other, key=lambda x: -x['b'])]
    if sw.notes['links']:
        L.append('junctions / links left alone in out: %d' % sw.notes['links'])
    L += ['', 'every item:'] + ['  %8s  %s' % (human(i['b']), os.path.relpath(i['p'], REPO)) for i in sorted(items, key=lambda x: -x['b'])[:3000]]
    return L, total


def alert(state, text, now):
    state['alert'] = {'time': now, 'text': text}
    with open(os.path.join(TIDY, 'alert.txt'), 'w', encoding='utf-8') as f:
        f.write('%s  %s\n' % (stamp(now), text))
    history('ALERT: ' + text)
    if TEST:
        return
    ps = ("[Windows.UI.Notifications.ToastNotificationManager,Windows.UI.Notifications,ContentType=WindowsRuntime]|Out-Null;"
          "$t=[Windows.UI.Notifications.ToastNotificationManager]::GetTemplateContent(1);"
          "$t.GetElementsByTagName('text')[0].AppendChild($t.CreateTextNode('WWE2K14 tidy: %s'))|Out-Null;"
          "[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier('WWE2K14 tidy').Show([Windows.UI.Notifications.ToastNotification]::new($t))"
          % text.replace("'", "''"))
    try:
        subprocess.run(['powershell.exe', '-NoProfile', '-Command', ps], capture_output=True, timeout=30, creationflags=NO_WINDOW)
    except (OSError, subprocess.TimeoutExpired):
        pass


# ---------------------------------------------------------------- commands
KEEP_LIST = os.environ.get('TIDY_TEST_KEEP') or next(
    (c for c in (os.path.join(REPO, 'tools', 'harness', 'keep_list.txt'), os.path.join(os.path.dirname(os.path.abspath(__file__)), 'keep_list.txt'))
     if os.path.isfile(c)), os.path.join(REPO, 'tools', 'harness', 'keep_list.txt'))


def protect_keys():
    """-> (keys, named): the exact paths a check names (S102 TIDY2: the state docs no longer protect anything) and the --hold carry-overs."""
    named = dict(named_inputs())
    return set(named) | carried_keys(), named


_held_cache = []


def held_index(fresh=False):
    """-> (HeldIndex, problems): the allowlist, the keep list, the exact paths a check names, the carry-overs. Cached per process."""
    global HELD
    if _held_cache and not fresh:
        return _held_cache[0]
    entries, problems = tidy_out.load_keep_list(KEEP_LIST)
    _, named = protect_keys()
    version = ''
    try:
        version = open(os.path.join(REPO, 'VERSION'), encoding='utf-8').read().strip().lower()
    except OSError:
        pass
    h = tidy_out.HeldIndex(version, entries, named, carried_keys())
    _held_cache[:] = [(h, problems)]
    HELD = h
    return _held_cache[0]


class Sweep:
    def __init__(self, items, rows, notes):
        self.items, self.rows, self.notes = items, rows, notes


def scan_out(wts, idle_h=None):
    """S102 TIDY2: everything in out\\ that is not held (tidy_out.sweep). idle_h: only things idle this long (the emergency pass while a session is live)."""
    held, _ = held_index()
    skip = {os.path.normcase(os.path.abspath(w['path'])) for w in wts}
    cutoff = time.time() - idle_h * HOUR if idle_h else None
    items, rows, notes = tidy_out.sweep(OUT, held, skip, cutoff, type_of, busy_dirs=busy_exe_dirs())
    return Sweep(items, rows, notes)


def running_image_paths():
    """S106 G2L: exe image paths of every running process (psutil; TIDY_TEST_PROCS = a JSON list in the selftest). Unreadable processes are skipped."""
    if TEST and os.environ.get('TIDY_TEST_PROCS') is not None:
        try:
            return [str(x) for x in json.loads(os.environ['TIDY_TEST_PROCS'])]
        except ValueError:
            return []
    try:
        import psutil
    except ImportError:
        return []
    out = []
    for p in psutil.process_iter(['exe']):
        try:
            if p.info.get('exe'):
                out.append(p.info['exe'])
        except (psutil.Error, OSError):
            pass
    return out


def busy_exe_dirs():
    """S106 G2L: out\\exe\\<name> folders a running process image sits in: the sweep skips them whole."""
    return tidy_out.busy_exe_dirs(OUT, running_image_paths())


def disk_status(log=False):
    """-> (the [disk] line, hours until the reserve or None). log=True appends the free-space row first (the watch does)."""
    now, f = time.time(), free_gb()
    if log:
        tidy_disk.log_free(TIDY, f, now)
    return tidy_disk.disk_line(f, tidy_disk.read_free(TIDY), now, RESERVE_GB)


def named_fn():
    h = held_index()[0]
    return lambda path: bool(h.why(repo_key(path)))


def cmd_dupes():
    """R12 change 5, report only: byte-identical capture folders under out\\ (a folder holding images.bin / manifest.json), each set once; the
    copy a check names is the one to keep. Nothing is deleted, linked or moved."""
    t0 = time.time()
    cand = []
    for dp, dn, fn in os.walk(OUT):
        rel = out_rel(dp)
        if any(rel == n or rel.startswith(n + '/') for n in NO_TOUCH):
            dn[:] = []
            continue
        if CAPTURE_MARKERS & {x.lower() for x in fn}:
            cand.append(dp)
            dn[:] = []
    groups = tidy_disk.find_dupes(cand)
    L, saved = tidy_disk.dupe_lines(groups, named_fn(), human)
    L.insert(0, 'duplicate captures: %d capture folders, %d identical sets, %s could be stored once (report only; scan took %.0f s)' % (
        len(cand), len(groups), human(saved), time.time() - t0))
    os.makedirs(TIDY, exist_ok=True)
    with open(os.path.join(TIDY, 'dupes.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(L) + '\n')
    print('\n'.join(L))
    return 0


def cmd_plan(ahead=0.0):
    """What the END OF A SESSION would delete now (S102 TIDY2). ahead is accepted and ignored: there are no ages any more."""
    real_now = time.time()
    live = sessions(real_now)
    age = Age(real_now - WT_IDLE_H * HOUR)
    protect, _named = protect_keys()
    wts = check_worktrees(age, protect, real_now, {s_[0] for s_ in live})
    held, problems = held_index()
    sw = scan_out(wts)
    L, total = report_lines(sw, wts, live, problems, held)
    sc_total, sc_lines = safe_scratch(real_now, True)
    L[3:3] = [disk_status()[0]] + missing_lines() + budget_lines() + (['agent worktree out\\ scratch (would clear now: %s):' % human(sc_total)] + ['  ' + x for x in sc_lines] if sc_lines else [])
    os.makedirs(TIDY, exist_ok=True)
    name = 'plan_' + time.strftime('%Y%m%d_%H%M')
    save_json(name + '.json', {'made': real_now, 'ahead': 0, 'items': sw.items, 'worktrees': [w for w in wts if w['state'] == 'remove']})
    with open(os.path.join(TIDY, name + '.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(L) + '\n')
    print('\n'.join(L[:L.index('every item:')]))
    print('plan: out\\tidy\\%s.json   full list: out\\tidy\\%s.txt   scan took %.1f s' % (name, name, time.time() - real_now))
    return 0


def cmd_apply(path):
    plan = json.load(open(path, encoding='utf-8'))
    if plan.get('ahead'):
        print('tidy: that plan is a forecast (--ahead), not applicable')
        return 2
    t0 = time.time()
    if t0 - plan['made'] > PLAN_MAX_H * HOUR:
        print('tidy: this plan is over %d h old, make a new one with --plan' % PLAN_MAX_H)
        return 2
    d = Deleter(plan['made'])
    d.remove(plan['items'], plan['worktrees'])
    finish('apply', d, t0)
    if not d.errors or len(d.errors) < 200:
        with open(os.path.join(TIDY, 'enabled'), 'w') as f:
            f.write(stamp() + '\n')
    return 0


def finish(label, d, t0):
    line = '%s: removed %s (%d files) in %.0f s, %d skipped as changed, %d errors, C: free %.0f GB' % (
        label, human(d.bytes), d.files, time.time() - t0, d.skipped, len(d.errors), free_gb())
    history(line)
    print(line)
    if d.errors:
        name = 'errors_' + time.strftime('%Y%m%d_%H%M%S') + '.txt'
        with open(os.path.join(TIDY, name), 'w', encoding='utf-8') as f:
            f.write('\n'.join(d.errors) + '\n')
        print('details: out\\tidy\\' + name)
        prune_reports('errors_', 10)


def low_order(i):
    t = i['types']
    top = max(t, key=t.get) if t else 'other'
    return (LOW_ORDER.index(top) if top in LOW_ORDER else len(LOW_ORDER), -i['b'] if top == 'worktree' else i['t'])


def surface_job_alive():
    """A Surface runner (tools\\surface\\s0_5_jobs.ps1 keeps its pid in out\\surface_runner.lock) that is still alive."""
    try:
        txt = open(os.path.join(OUT, 'surface_runner.lock'), encoding='utf-8', errors='replace').read()
        m = re.search(r'\d+', txt)
        return bool(m) and pid_alive(int(m.group(0)))
    except OSError:
        return False


def own_activity(now, live):
    """'' or why a session end must not sweep everything: a running agent of a live session (its meta has no finished flag), or a Surface runner."""
    try:
        live_ids = {x[0] for x in live}
        for i in agent_infos():
            if i['has_meta'] and i['finished'] is not True and i['parent'] in live_ids and max(i['tr'], i['meta_t']) >= now - ORPHAN_META_H * HOUR:
                return 'agent %s of this session is still running' % i['id']
    except Exception as e:
        return 'could not read the agents (%s): treated as running' % repr(e)[:60]
    if surface_job_alive():
        return 'a Surface runner (out\\surface_runner.lock) is alive'
    return ''


def out_sweep_decision(trigger, live, session_id, now=None):
    """S102 TIDY2: -> (sweep out\\ now?, why not, idle hours or None). out\\ is cleared at the end of a session and when no session is live; never while
    one is live. The hourly sweep takes only items untouched HOURLY_IDLE_H; session end takes only items idle ACTIVE_IDLE_H while this session's own
    agents / a Surface job are still running."""
    now = now or time.time()
    if trigger == 'end':
        me = next((x for x in live if session_id and x[0].startswith(session_id)), None) or (live[0] if len(live) == 1 and not session_id else None)
        others = [x for x in live if me is None or x[0] != me[0]]
        if others:
            return False, 'another session is live (%s): out\\ left alone' % ', '.join(x[0][:8] for x in others), None
        busy = own_activity(now, live)
        if busy:
            return True, '', ACTIVE_IDLE_H
        return True, '', None
    if trigger == 'hourly':
        if live:
            return False, 'a session is live: out\\ left alone', None
        return True, '', HOURLY_IDLE_H
    return False, 'out\\ is cleared at session end or when no session is live', None


def cmd_auto(trigger, dry, session_id):
    if not os.path.exists(os.path.join(TIDY, 'enabled')) and not dry:
        print('tidy: automatic mode is off until the first clean-up has been approved (--apply)')
        return 0
    now = time.time()
    live = sessions(now)
    age = Age(now - WT_IDLE_H * HOUR)      # plain worktrees: merged and finished go at the next pass
    released_before = None
    if trigger == 'end':
        me = next((s_ for s_ in live if session_id and s_[0].startswith(session_id)), None) or (live[0] if len(live) == 1 and not session_id else None)
        if me and len(live) == 1:
            released_before = me[1]
    released = carry_expire(now, released_before=released_before) if not dry else []
    if released:
        history('carry-over released: ' + ', '.join(c['path'] for c in released))
    protect, _ = protect_keys()
    wts = check_worktrees(age, protect, now)
    do_out, why_not, idle_h = out_sweep_decision(trigger, live, session_id, now)
    held, problems = held_index(fresh=True)
    if do_out and problems:      # fix 4: FAIL CLOSED. A bad keep line would stop holding its inputs: no sweep until it is fixed
        do_out, why_not = False, 'LOUD: %d lint / keep-list problem(s): out\\ NOT swept until fixed (first: %s)' % (len(problems), problems[0][:140])
        history('out\\ sweep skipped: ' + why_not)
        if not dry:
            st_ = load_json('state.json', {})
            if now - st_.get('alert', {}).get('time', 0) > ALERT_EVERY_H * HOUR:
                alert(st_, why_not[:200], now)
                save_json('state.json', st_)
    if do_out and idle_h and trigger == 'end':
        print('out\\: swept only items idle %g min (%s)' % (idle_h * 60, own_activity(now, live)))
    sw = scan_out(wts, idle_h) if do_out else Sweep([], [], {'links': 0, 'worktrees': [], 'recent': 0, 'keeps': []})
    items = sw.items
    rm = [w for w in wts if w['state'] == 'remove']
    if dry:
        total = sum(i['b'] for i in items)
        print('tidy --dry (%s): would remove %s from out\\%s' % (trigger, human(total), '' if do_out else ' (%s)' % why_not))
        area, _t = summarize(items, wts)
        for k, v in area.most_common(12):
            print('  %8s  %s' % (human(v), k))
        for line in agent_summary(wts):
            print(line)
        sc_total, sc_lines = safe_scratch(now, True)
        for line in sc_lines:
            print(line)
        for line in missing_lines() + budget_lines():
            print(line)
        for x in problems:
            print('  problem: ' + x)
        return 0
    d = Deleter(now)
    d.remove(items, rm, [w for w in wts if w['state'] == 'gone'])
    finish(trigger, d, now)
    if not do_out:
        print('out\\: %s' % why_not)
    for k in sw.notes['keeps'][:10]:
        print('KEEP file ignored (move it to tools\\harness\\keep_list.txt with a reason): ' + os.path.relpath(k, REPO))
    for x in problems:
        print('problem: ' + x)
    for b, rel, why, _p in held_sizes(sw)[:12] if do_out else []:
        print('  held %8s  %s  [%s]%s' % (human(b), rel, why[:80], '  BIG: confirm with Jarrod' if b > BIG_HELD_GB * GB else ''))
    sc_total, sc_lines = safe_scratch(now)
    for line in sc_lines:
        print(line)
    bl = budget_lines()
    for line in bl + missing_lines():
        print(line)
    history(bl[0])
    state = load_json('state.json', {})
    free = free_gb()
    if free < LOW_GB:
        free = low_pass(live, protect)
        if free < LOW_GB and now - state.get('alert', {}).get('time', 0) > ALERT_EVERY_H * HOUR:
            alert(state, 'C: is at %.0f GB free and nothing else can be cleared without touching a live session.' % free, now)
    elif os.path.exists(os.path.join(TIDY, 'alert.txt')):
        os.unlink(os.path.join(TIDY, 'alert.txt'))
    state['last'] = now
    save_json('state.json', state)
    prune_reports('plan_', 6)
    return 0


def item_cat(i):
    if 'wt' in i:
        return 'agent worktrees' if i['wt'].get('agent') else 'worktrees'
    t = i['types']
    return max(t, key=t.get) if t else 'other'


def run_batches(d, items, target, cats=None):
    """Deletes items in order until free >= target GB. Batches are one category each (cats: category -> [bytes, items])."""
    done = 0
    while done < len(items) and free_gb() < target:
        deficit = (target - free_gb()) * GB
        batch, size = [], 0
        cat = item_cat(items[done])
        while done < len(items) and len(batch) < 25 and size < deficit and item_cat(items[done]) == cat:     # never much past the target
            batch.append(items[done])
            size += items[done]['b']
            done += 1
        before = d.bytes
        plain = [i for i in batch if 'wt' not in i]
        files = [i for i in plain if not i['dir']]
        list(d.pool.map(d.file, files))
        d.folders([i['p'] for i in plain if i['dir']])
        d.unlock_prune([i['wt'] for i in batch if 'wt' in i and d.worktree(i['wt'])])
        if cats is not None:
            c = cats.setdefault(cat, [0, 0])
            c[0] += d.bytes - before
            c[1] += len(batch)
    return done


def wt_item(w):
    return {'p': w['path'], 'b': w['b'], 'n': w['n'], 't': w['t'], 'types': {'worktree': w['b']}, 'dir': True, 'wt': w}


def low_pass(live, protect):
    """Under 50 GB: past the rolling rule (lane 154: 1 h, or LOW_LIVE_KEEP_H while a session is live), finished agent
    worktrees included, biggest first. Stops at 75 GB free. Lane 187: a finished MERGED agent worktree idle NOW_IDLE_H (30 min)
    goes too (an unmerged one still waits 24 h here; --now takes those as well: their commits are saved under refs/tidy)."""
    now = time.time()
    age = Age(now - WT_IDLE_H * HOUR)
    wts = check_worktrees(age, protect, now, merged_h=NOW_IDLE_H)
    items = scan_out(wts, LOW_LIVE_KEEP_H if live else 1).items      # S102 TIDY2: below the reserve only: idle 2 h while a session is live
    rm = [w for w in wts if w['state'] == 'remove']
    for w in rm:
        items.append(wt_item(w))
    items.sort(key=low_order)
    t0 = now
    d = Deleter(now)
    run_batches(d, items, TARGET_GB)
    d.unlock_prune([w for w in wts if w['state'] == 'gone'])
    d.pool.shutdown()
    finish('low disk', d, t0)
    return free_gb()


def refusal_bucket(w):
    why = w.get('why', '')
    if 'still running' in why or 'running again' in why:
        return 'running agent'
    if 'transcript' in why or why.startswith('active within') or why == 'recent activity':
        return 'active in the last %d min' % int(NOW_IDLE_H * 60)
    if 'KEEP' in why:
        return 'KEEP file'
    if 'named in a state doc' in why:
        return 'held / named in a state doc'
    if 'real edit' in why:
        return 'real edits (reported)'
    if 'junction' in why:
        return 'junction (reported)'
    return why[:50] or 'other'


def cmd_now(dry=False, label='now', target=None):
    """Lane 187 (Jarrod 2026-11-12 23:4x: clearing storage must be one command, not an analysis). Frees space at once, no prompts:
    (a) finished MERGED agent worktrees idle NOW_IDLE_H (30 min), biggest first (unmerged ones, under review or resumed for fixes,
    keep AGENT_LIVE_H and go at 30 min only below HARD_FLOOR_GB free; commits no branch holds go under refs/tidy/, branches stay;
    a running agent is found from its meta / transcript, never typed); (b) then exactly the under-50-GB pass (its ages, plain
    worktrees included). Stops at `target` (default NOW_TARGET_GB) free."""
    global ANY_PARENT
    ANY_PARENT = True
    target = NOW_TARGET_GB if target is None else target
    t0 = now = time.time()
    free0 = free_gb()
    if free0 >= target:
        print('tidy --now: %.0f GB free already (target %d GB), nothing to do' % (free0, target))
        return 0
    live = sessions(now)
    protect, _ = protect_keys()
    age = Age(now - WT_IDLE_H * HOUR)
    floor = free0 < HARD_FLOOR_GB
    wts = check_worktrees(age, protect, now, {s_[0] for s_ in live}, NOW_IDLE_H, NOW_IDLE_H if floor else AGENT_LIVE_H)
    rm_a = sorted((w for w in wts if w.get('agent') and w['state'] == 'remove'), key=lambda w: -w['b'])
    cats, errors, stopped = {}, [], ''
    kept = collections.OrderedDict()
    for w in wts:
        if w['state'] in ('kept', 'report'):
            e = kept.setdefault(refusal_bucket(w), [0, 0])
            e[0] += w['b']
            e[1] += 1
    d = None
    if dry:
        free = free0
        for w in rm_a:
            if free >= target:
                break
            free += w['b'] / GB
            c = cats.setdefault('agent worktrees', [0, 0])
            c[0] += w['b']
            c[1] += 1
    else:
        d = Deleter(now - NOW_IDLE_H * HOUR)
        run_batches(d, [wt_item(w) for w in rm_a], target, cats)
    now_free = (lambda: free0 + sum(c[0] for c in cats.values()) / GB) if dry else free_gb
    sc_lines = []
    if now_free() < target:     # lane 294: agent worktree out\ scratch (finished agents; everything over 10 GB while under 100 GB free)
        sc_total, sc_lines = safe_scratch(now, dry, now_free(), [w['path'] for w in rm_a])
        if sc_total:
            cats['agent worktree out\\ scratch'] = [sc_total, len(sc_lines)]
    if now_free() < target:
        busy = lock_busy() if not (dry or LOCKED) else ''     # inside the run-lock hold the status would always read busy
        if busy:
            stopped = 'skipped the out\\ pass: a game or build is running (%s)' % busy[:60]
        else:
            items = scan_out(wts, LOW_LIVE_KEEP_H if live else 1).items
            for w in wts:
                if w['state'] == 'remove' and not w.get('agent'):
                    items.append(wt_item(w))
            items.sort(key=low_order)
            if dry:
                free = now_free()
                for i in items:
                    if free >= target:
                        break
                    free += i['b'] / GB
                    c = cats.setdefault(item_cat(i), [0, 0])
                    c[0] += i['b']
                    c[1] += 1
            else:
                run_batches(d, items, target, cats)
    if d is not None:
        d.unlock_prune([w for w in wts if w['state'] == 'gone'])
        d.pool.shutdown()
        errors = d.errors
    free1 = now_free()
    total = sum(c[0] for c in cats.values())
    L = ['tidy --%s%s %s: C: free %.1f GB -> %.1f GB%s (target %d GB), %s %.1f GB in %.0f s' % (
        label, ' --dry' if dry else '', stamp(), free0, free1, ' (projected)' if dry else '', target,
        'would free' if dry else 'freed', total / GB, time.time() - t0)]
    for k, (b, n) in sorted(cats.items(), key=lambda kv: -kv[1][0]):
        L.append('  %s %6.1f GB  %s (%d)' % ('would free' if dry else 'freed', b / GB, k, n))
    if not cats:
        L.append('  nothing was removable')
    for k, (b, n) in list(kept.items())[:6]:
        L.append('  kept %6.1f GB  %s (%d)' % (b / GB, k, n))
    if floor:
        L.append('  HARD FLOOR (under %d GB free): finished UNMERGED agent worktrees idle %d min were taken too (commits kept under refs/tidy, branches stay)' % (HARD_FLOOR_GB, int(NOW_IDLE_H * 60)))
    if stopped:
        L.append('  ' + stopped)
    if errors:
        L.append('  %d refusals / errors (first: %s)' % (len(errors), errors[0][-110:]))
    if not dry and free1 < target and not stopped:
        L.append('  below the target and nothing else may go: the rest is live, held, KEEP or a real edit')
    L += ['  ' + x for x in sc_lines if x.startswith('LOUD')]
    L += missing_lines(4)       # no budget walk here: --now measures nothing it does not need (lane 187); the hourly pass prints it
    if len(L) > 24:
        L = L[:23] + ['  ...']
    print('\n'.join(L))
    if not dry:
        history('%s: %s' % (label, L[0]))
        state = load_json('state.json', {})
        if free1 < LOW_GB and now - state.get('alert', {}).get('time', 0) > ALERT_EVERY_H * HOUR:
            alert(state, 'C: is at %.0f GB free after --%s; nothing else can be cleared without touching a live session.' % (free1, label), now)
        state['last'] = now
        save_json('state.json', state)
    return 0


def cmd_hold(path, why, until=None, days=None):
    p = os.path.abspath(path if os.path.isabs(path) else os.path.join(REPO, path))
    if not (is_allowed(p) or wt_allowed(p)):
        print('tidy: --hold only makes sense for folders under out\\, thirdparty\\sdk-wt or an agent worktree in .claude\\worktrees')
        return 2
    if days is not None and days <= 0:
        print('tidy: --days wants a number above 0')
        return 2
    now = time.time()
    fixed, expires = False, now + CARRY_DAYS * 86400
    if until:       # lane 154: a hold with its own expiry, the end of that day (local time); --auto end never releases it
        try:
            expires = time.mktime(time.strptime(until + ' 23:59:59', '%Y-%m-%d %H:%M:%S'))
        except ValueError:
            print('tidy: --until wants YYYY-MM-DD')
            return 2
        fixed = True
    elif days is not None:
        expires, fixed = now + days * 86400, True
    if fixed and expires <= now:
        print('tidy: that expiry is already past')
        return 2
    key = repo_key(p)
    old = next((c for c in carry_entries() if c['path'] == key), None)
    if old and old['fixed'] and not fixed and old['expires'] > now:
        print('tidy: %s already has a hold until %s; kept (release it first to replace it with a plain hold)' % (key, stamp(old['expires'])))
        return 0
    rows = [c for c in carry_entries() if c['path'] != key]
    rows.append({'path': key, 'why': why, 'added': now, 'expires': expires, 'fixed': fixed})
    save_json('carry.json', rows)
    if fixed:
        print('held until %s (only its own expiry releases it, never --auto end): %s' % (stamp(expires), key))
    else:
        print('held until the next session ends (at most %d days): %s' % (CARRY_DAYS, key))
    return 0


def cmd_release(path, everything=False):
    """--release PATH drops that one; --release alone drops the plain holds and keeps --until / --days ones; --release --all drops all."""
    if path:
        key = repo_key(os.path.abspath(path if os.path.isabs(path) else os.path.join(REPO, path)))
        rows = [c for c in carry_entries() if c['path'] != key]
    elif everything:
        rows = []
    else:
        rows = [c for c in carry_entries() if c['fixed']]
        if rows:
            print('kept %d hold(s) with their own expiry (--release --all drops those too)' % len(rows))
    save_json('carry.json', rows)
    print('carry-over now holds %d item(s)' % len(rows))
    return 0


def lock_shared_ok(a):
    """S102 K1 (R12 change 7): only --scratch and the watch run when other SHARED / build holders are about (R12 assessed those two: they touch
    only expired agent scratch). The hourly pass and --now keep the strict check. NOTE: the watch's scratch escalation now really deletes while
    a shared slot is held; before this it was skipped whenever any slot was busy."""
    return a.auto == 'watch' or bool(getattr(a, 'scratch', False))


def wants_lock(a):
    """Every command that deletes goes through the busy check and the shared run-lock hold: --apply, --auto (watch too), --now."""
    return bool(a.apply) or (a.auto in ('start', 'end', 'hourly', 'watch') and not a.dry) or (bool(a.now) and not a.dry) or (getattr(a, 'scratch', False) and not a.dry)


def main():
    if sys.stdout is None:      # pythonw
        sys.stdout = sys.stderr = open(os.devnull, 'w')
    ap = argparse.ArgumentParser()
    ap.add_argument('--plan', action='store_true')
    ap.add_argument('--apply')
    ap.add_argument('--auto', choices=['start', 'end', 'hourly', 'watch'])
    ap.add_argument('--target', type=float, help='with --now: stop at this much free space (GB, default 100)')
    ap.add_argument('--now', action='store_true', help='free space at once (finished agent worktrees idle 30 min, then throwaways) up to 100 GB')
    ap.add_argument('--scratch', action='store_true', help='clear agent worktree out\\ scratch only (finished agents; over 10 GB under pressure); with --dry only report')
    ap.add_argument('--reserve-ok', action='store_true', help='S102 K1: print the [disk] line; exit 1 when C: has less than the reserve free (do not start lanes that write captures)')
    ap.add_argument('--disk', action='store_true', help='S102 K1: print the [disk] line (free space and its rate of fall)')
    ap.add_argument('--scratchpads', action='store_true', help='S102 K1: also run the Claude scratchpad delete pass (OFF by default until Jarrod says yes)')
    ap.add_argument('--dupes', action='store_true', help='S102 K1: report byte-identical capture folders (reads only)')
    ap.add_argument('--missing', action='store_true', help='list inputs a check names that do not exist')
    ap.add_argument('--guard', nargs='+', metavar='PATH', help='exit 1 and name every PATH a delete must refuse (an input a check names, inside one, or holding one)')
    ap.add_argument('--dry', action='store_true')
    ap.add_argument('--ahead', type=float, help='with --plan: a read-only forecast, as if it were N hours later')
    ap.add_argument('--session', help='this session id, for --auto end (default: the transcript written most recently)')
    ap.add_argument('--hold', nargs=2, metavar=('PATH', 'WHY'))
    ap.add_argument('--until', metavar='YYYY-MM-DD', help='with --hold: released only at the end of that day')
    ap.add_argument('--days', type=float, help='with --hold: released only after N days')
    ap.add_argument('--holds', action='store_true')
    ap.add_argument('--release', nargs='?', const='')
    ap.add_argument('--all', action='store_true', help='with --release and no path: drop the --until / --days holds too')
    ap.add_argument('--detach', action='store_true', help='return at once and run in the background')
    ap.add_argument('--locked', action='store_true', help=argparse.SUPPRESS)
    a = ap.parse_args()
    if (a.until or a.days is not None) and not a.hold:
        print('tidy: --until / --days only go with --hold PATH WHY')
        return 2
    if os.environ.get('TIDY_REPO') and not os.environ.get('TIDY_TEST_ROOT') and not (a.plan or a.missing or a.guard or a.disk or a.reserve_ok or a.dupes or (a.dry and (a.now or a.scratch))):
        print('tidy: TIDY_REPO (run against another checkout) is for --plan only (or --now --dry)')      # N7
        return 2

    if a.guard:
        a.guard = [os.path.abspath(x) for x in a.guard]     # relative paths mean the CURRENT directory, before any re-run against the main checkout
    if (a.missing or a.guard) and not os.environ.get('TIDY_REPO') and not TEST:       # S5: always judge against the main checkout
        mc = main_checkout()
        if os.path.normcase(mc) != os.path.normcase(REPO):
            env = dict(os.environ, TIDY_REPO=mc)
            argv = ['--guard'] + a.guard if a.guard else ['--missing']
            return subprocess.run([sys.executable, os.path.abspath(__file__)] + argv, env=env).returncode
    if a.reserve_ok or a.disk:
        line, _h = disk_status()
        print(line)
        return 1 if (a.reserve_ok and free_gb() < RESERVE_GB) else 0
    if a.dupes:
        return cmd_dupes()
    if a.missing:
        ml = missing_inputs()
        print('\n'.join('MISSING INPUT: %s  (named by %s)' % m for m in ml) or 'no named input is missing')
        return 0
    if a.guard:
        bad = [(p, r) for p, r in guard_paths(a.guard) if r]
        for p, r in bad:
            print('REFUSE %s  %s' % (p, r))
        print('%d of %d paths refused' % (len(bad), len(a.guard)))
        return 1 if bad else 0
    if a.holds:
        for c in carry_entries():
            print('%s  (%s, until %s%s)' % (c['path'], c['why'], stamp(c['expires']), ', own expiry: --auto end keeps it' if c['fixed'] else ''))
        return 0
    if a.hold:
        return cmd_hold(a.hold[0], a.hold[1], a.until, a.days)
    if a.release is not None:
        return cmd_release(a.release, a.all)
    if a.detach:
        args = [x for x in sys.argv[1:] if x != '--detach']
        exe = sys.executable.replace('python.exe', 'pythonw.exe')
        subprocess.Popen([exe, os.path.abspath(__file__)] + args, creationflags=DETACHED | NEW_GROUP | NO_WINDOW, close_fds=True)
        return 0

    if a.target is not None and not a.now:
        print('tidy: --target only goes with --now')
        return 2
    if a.target is not None and a.target <= 0:
        print('tidy: --target wants a number above 0 (GB)')
        return 2
    global LOCKED, SCRATCHPAD_PASS
    LOCKED = a.locked
    if a.scratchpads:
        SCRATCHPAD_PASS = True
    if a.auto == 'watch':       # lane 187: free space only, no scan, no lock: exits at once while there is room
        global PRESSURE_FORCE
        if not (a.dry or TEST):      # S103: the PC watchdog (notices and alerts only, never kills) rides the same 10-minute task
            try:
                subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pc_watchdog.py')], capture_output=True, timeout=120, creationflags=NO_WINDOW)
            except Exception:
                pass
        # QTF: tools\harness\proc_sweep.py is deliberately NOT wired here until its second Opus review passes
        # (then: proc_sweep.sweep(kill=not (a.dry or TEST), log=history, quiet=True, worktree_busy=<agent_state check>), in a try / except).
        if not (a.dry or TEST):      # S102 K1 (R12 change 8): every watch run logs free space, enabled or not
            line, hours = disk_status(log=True)
            if hours is not None and hours < 3:
                st_ = load_json('state.json', {})
                if time.time() - st_.get('alert', {}).get('time', 0) > ALERT_EVERY_H * HOUR:
                    alert(st_, line, time.time())
                    save_json('state.json', st_)
            if hours is not None and hours < 2:
                PRESSURE_FORCE = True       # the quota / pressure rules act before the 50 GB floor
                history(line)
        if not (os.path.exists(os.path.join(TIDY, 'enabled')) or a.dry):
            return 0
        if free_gb() >= LOW_GB:
            # lane 294 S6: with agent worktrees around the watch looks at their out\ scratch, WITHOUT the run lock; only if something would
            # go does it escalate (and take the lock). Cheap when nothing is clearable.
            try:
                if not has_agent_out() or (not TEST and lock_busy(True)):       # nothing to look at, or a timed run holds the lock: no disk walk now
                    return 0
                if not safe_scratch(time.time(), True)[0]:
                    return 0
            except Exception:
                return 0
            a.scratch = True
        else:
            a.now = True
    deletes = wants_lock(a)
    if deletes and not a.locked and not TEST:
        # Checked here, before taking a slot: under our own hold the status would always read busy.
        busy = lock_busy(shared_ok=lock_shared_ok(a))      # S102 K1: shared holds do not stop --scratch and the watch only
        if busy:
            os.makedirs(TIDY, exist_ok=True)
            if a.auto != 'watch':       # the 10-minute watch would log this all day
                history('%s: skipped, %s' % (a.auto or ('now' if a.now else 'apply'), busy))
            print('tidy: skipped, a game or build is running: ' + busy)
            if a.auto == 'watch' and free_gb() < HARD_FLOOR_GB:       # lane 187: low on space and the lock keeps the tidy out: say so (once a day)
                state = load_json('state.json', {})
                t = time.time()
                if t - state.get('alert', {}).get('time', 0) > ALERT_EVERY_H * HOUR:
                    alert(state, 'C: is at %.0f GB free and a game or build holds the run lock, so the tidy could not clear anything.' % free_gb(), t)
                    save_json('state.json', state)
            return 0
        # A shared run-lock hold, so a timed run and the delete never overlap. Short wait: the next trigger tries again.
        rl = os.path.join(REPO, 'tools', 'runlock.ps1')
        cmd = ['pwsh', '-NoProfile', '-File', rl, 'run', '-Shared', '-Name', 'tidy', '-TimeoutMinutes', '1', '-MaxMinutes', '60', '--',
               sys.executable, os.path.abspath(__file__)] + sys.argv[1:] + ['--locked']
        # S101: capture and re-print the child's output: with NO_WINDOW and no pipes a real --now printed nothing (its summary only
        # reached history.txt), which looked like a silent no-op to the person who ran it.
        r = subprocess.run(cmd, creationflags=NO_WINDOW, capture_output=True, text=True, errors='replace')
        if r.stdout:
            print(r.stdout.rstrip())
        if r.stderr:
            print(r.stderr.rstrip(), file=sys.stderr)
        return r.returncode

    os.makedirs(TIDY, exist_ok=True)
    fh = open(os.path.join(TIDY, '.run.lock'), 'w')
    try:
        msvcrt.locking(fh.fileno(), msvcrt.LK_NBLCK, 1)
    except OSError:
        print('tidy: another tidy is running')
        return 0
    try:
        if a.plan:
            return cmd_plan(a.ahead or 0.0)
        if a.apply:
            return cmd_apply(a.apply)
        if a.scratch:
            return cmd_scratch(a.dry)
        if a.now:
            return cmd_now(a.dry, 'auto watch' if a.auto == 'watch' else 'now', a.target)
        if a.auto:
            return cmd_auto(a.auto, a.dry, a.session)
    except Exception as e:
        history('CRASHED (%s): %s' % (a.auto or 'plan/apply', repr(e)[:300]))
        state = load_json('state.json', {})
        now = time.time()
        if now - state.get('alert', {}).get('time', 0) > ALERT_EVERY_H * HOUR:
            alert(state, 'the tidy crashed: %s' % repr(e)[:160], now)
            save_json('state.json', state)
        raise
    ap.print_help()
    return 2


if __name__ == '__main__':
    sys.exit(main())
