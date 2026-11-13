"""The project's automatic disk tidy (Jarrod, 2026-11-09: storage should stay ~75 GB by itself, nobody runs or checks it).

Replaces tools\\harness\\disk_cleanup.ps1. Rules and where they came from: docs\\DECISIONS.md; how sessions use it: CLAUDE.md (Safety).

WHAT IT MAY TOUCH: out\\, the SDK worktrees in thirdparty\\sdk-wt, and the Claude Code agent worktrees .claude\\worktrees\\agent-* (a
whole worktree, or only the contents of its own out\\ scratch: LANE 294 below; never any other file there). Deletes permanently,
never follows a junction or link.
  - out\\ is temporary by default. Anything not on the keep list expires, except small notes (.md .txt .json .toml .py .ps1 up to
    2 MB: source comments and docs cite them). Expiry is ROLLING (lane 154, Jarrod 2026-11-12): output made DURING a live session
    expires at LIVE_KEEP_H (6 h); output made before it (last night's) and everything when no session is live expires at
    EXPIRE_H (24 h). Sheets made for Jarrod overnight need a hold.
  - RUN FOLDERS out\\sNN\\runs\\YYYYMMDD_HHMMSS_<route> (lane 294): the newest of each route, and any within RUN_PAIR_MIN (30 min) of it
    (a run's case and record folders, A/B pairs), keep the old rule (whole live session + 24 h); every other run folder goes after
    RUN_H (2 h). A run named in a state doc / --hold / gate lane, or holding a valid KEEP, stays. An unpacked capture copy
    (images.bin / manifest.json / case_*.zip) outside a run goes at CAPTURE_COPY_H (6 h). A file that cannot be removed stops its
    folder (capture markers go first); never half a capture.
  - KEEP FILES EXPIRE (lane 294, Jarrod 2026-11-13): a KEEP holds only while it has a reason AND (1) a future "until YYYY-MM-DD"
    in it, or (2) a gate lane / a check, selftest, oracle or gate script in tools\\ names the folder's own out path, or its reason
    cites an existing such script or a tools\\gate\\lanes json. A bare KEEP is a 7-day hold from its mtime, then no hold. --plan
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
    .ninja_log - and anything holding a junction stay). Same refusals as any other delete: a --hold, a KEEP, an owner process.

NEVER REMOVED, on every trigger: anything younger than the rolling limit above, anything under a folder with an ACTIVE KEEP (above),
anything on the carry-over list (--hold), any path named in STATUS.md, PLAYBOOK.md, CLAUDE.md or the newest session plan / summary
(a session root or an out\\sNN\\runs namespace named there protects nothing), any out path (depth 2 and deeper) a gate lane names
(tools\\gate\\lanes\\*.json: needs and commands; a glob cut at * is a prefix) and every capture / run folder a check script names
(whatever its age); other folders a check / selftest / oracle / gate script names while they were touched within TOOL_STALE_DAYS
(7). A game or build holding the run lock = the tidy skips this time. Every pass except --now prints the project's size against the
~75 GB budget and the top 5 folders, and MISSING GATE INPUT for gate-named paths that do not exist.

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
                                                     holding an active KEEP; a gate-named input, inside one or above one
  py tools\\harness\\tidy.py --missing              gate-named inputs that do not exist (MISSING GATE INPUT)
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
AGENT_UNMERGED_H = 24   # an agent worktree whose HEAD is not merged into the session branch waits this long
ORPHAN_META_H = 24      # a meta without "finished" counts as running only while it (or its transcript) was written this recently
LOW_GB = 50
TARGET_GB = 75
NOW_IDLE_H = 0.5        # --now / --auto watch / the under-50-GB pass: a finished agent worktree idle this long goes (S101 lane 187)
NOW_TARGET_GB = 100     # --now stops once this much is free
HARD_FLOOR_GB = 15      # --now only: below this much free, finished UNMERGED agent worktrees go at NOW_IDLE_H too
ANY_PARENT = False      # --now: a meta without 'finished' written within ORPHAN_META_H counts as running whatever its parent session
LOCKED = False         # this process runs inside the run-lock hold (--locked)
LIVE_IDLE_H = 3     # a session whose transcript was written this recently is live
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
KEEP_BARE_DAYS = 7      # lane 294: a KEEP with neither an expiry nor a gate / tool that names it holds this long from its mtime
SCRATCH_REPORT_GB = 10      # lane 294: an agent worktree's out\ over this is reported loudly every pass
SCRATCH_PRESSURE_GB = 100   # and cleared (even if its agent is live) while C: has less than this free
SCRATCH_RECENT_MIN = 15     # under pressure only files modified in the last 15 min stay
BUDGET_GB = 75              # CLAUDE.md: the project stays ~75 GB

NO_TOUCH = {'bench', 'replays', 'scenarios', 'recordings', 'harness/screenshots', 'backup', 'tidy'}
KEEP_TREES = {
    'build', 'tools', 'assets', 'wm30', 'pgo', 'glslang-build', 'spirv-tools-build', 'shaders', 'native', 'trons',
    'owned_check', 'launcher_mobile', 'director', 'seeds', 'live', 'q', 'input', 'logo', 'logos', 'harness_vs',
    'harness/index', 'harness/atlas', 'harness/labelling', 'harness/regress', 'harness/runlock', 'harness/run_list',
    'harness/triage', 'harness/coverage', 'harness/sweep', 'harness/determinism', 'harness/watch',
    'android/apk', 'android/apk-obj', 'android/game-arm64', 'android/launcher-arm64', 'android/sdk-build-arm64',
    'android/sdk-install-arm64', 'android/symbols', 'android/drivers', 'android/cpu_semantics',
    'release/build',
}
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
        return Age(cut), Age(min([now - HOUR] + ([start] if live else [])))
    c24 = now - EXPIRE_H * HOUR
    if live:
        return (Age(c24, own_since, (start, now - LIVE_KEEP_H * HOUR)), Age(min(c24, start), own_since))
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


def named_paths():
    """Repo-relative paths (depth >= 2) named in the state docs. A bare out\\s95 does not protect all of Session 95."""
    files = [os.path.join(REPO, n) for n in REF_FILES_FIXED]
    newest = {}
    try:
        for n in os.listdir(REPO):
            m = re.fullmatch(r'SESSION(\d+)_(PLAN|SUMMARY)\.md', n)
            if m:
                newest.setdefault(int(m.group(1)), []).append(n)
    except OSError:
        pass
    if newest:
        files += [os.path.join(REPO, n) for n in newest[max(newest)]]
    found = {}
    for f in files:
        try:
            text = open(f, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        for m in REF_RE.finditer(text):
            k = re.sub(r'[\\/]+', '/', m.group(1)).rstrip('./').lower()
            parts = k.split('/')
            depth = len(parts) - (3 if k.startswith('thirdparty/') else 1)
            if k.startswith('out/') and _doc_namespace(k[4:]):
                continue        # lane 294 S1: a doc naming out/s101/runs (a namespace) must not hold every run under it
            if depth >= 2 or (k.startswith('thirdparty/') and depth >= 0):
                found.setdefault(k, os.path.basename(f))
    return found


FIXED_WHY = re.compile(r'jarrod|until\s+\d{4}-\d\d-\d\d', re.I)


GATE_RE = re.compile(r'(?<![A-Za-z0-9_])(out[\\/]+[^\s`\'"<>|*?(),;:\[\]{}]+)')
JOIN_RE = re.compile(r"""['"]out['"]((?:\s*[,/]\s*['"][^'"\\/\n]+['"])+)""")
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
    star = k.endswith('*')      # lane 294 S4: a glob cut at '*' / '?' is a PREFIX key (ends with '*')
    k = re.sub(r'[\\/]+', '/', k.rstrip('*')).rstrip('./').lower()
    parts = k.split('/')
    if len(parts) < 2 or parts[0] != 'out':
        return
    if star:
        if len(parts) >= 2 and not _namespace(k[4:]):
            found.setdefault(k + '*', src)
        return
    cands = [k]
    if len(parts) >= 3 and re.search(r'\.[a-z0-9]{1,5}$', parts[-1]):
        cands.append('/'.join(parts[:-1]))      # a file: its folder too (a capture's manifest is not the capture)
    for c in cands:
        if len(c.split('/')) >= 2 and not _namespace(c[4:]):
            found.setdefault(c, src)


def touched_within(path, days):
    """True when the folder, or something directly in it, changed within that many days."""
    try:
        t = os.stat(lp(path)).st_mtime
        for e in os.scandir(lp(path)):
            t = max(t, e.stat(follow_symlinks=False).st_mtime)
    except OSError:
        return False
    return time.time() - t < days * 86400


def _scan_text(text, found, src):
    for m in GATE_RE.finditer(text):
        _add_input(found, m.group(1) + ('*' if text[m.end():m.end() + 1] in ('*', '?') else ''), src)
    for m in JOIN_RE.finditer(text):      # os.path.join(..., 'out', 'a', 'b') and 'out' / 'a' / 'b'
        _add_input(found, 'out/' + '/'.join(LIT_RE.findall(m.group(1))), src)


def required_input(k):
    """Lane 294 (2026-11-13: the clean-up deleted ring gate captures J1 / 7149 that d3_ring_check.py needs): a capture or run folder a check
    names is a REQUIRED input and is protected whatever its age (no TOOL_STALE_DAYS rule): its last part is capture, or a part is case_*
    or a runs/<stamp> folder. Other tool-named folders keep the 7-day rule."""
    parts = k.split('/')
    return parts[-1] == 'capture' or any(x.startswith('case_') for x in parts) or 'runs' in parts[1:-1]


def gate_input_split():
    """-> (lane inputs, tool inputs): the two dicts gate_inputs() merges (the tools' names unfiltered by age)."""
    """Repo-relative paths (depth >= 2, so out/post_plan_cache counts) that lanes and tools read (lane 154, S3):
      - tools\\gate\\lanes\\*.json: every string naming out\\... (needs, command arguments);
      - tools\\**\\*check*.py / *selftest*.py / *oracle*.py / *gate*.py: out path literals ('out/a/b', r'out\\a\\b', os.path.join(..., 'out', 'a', 'b'), 'out' / 'a').
    Gates, fview and the check tools take no run lock, so the tidy cannot know when they run."""
    found = {}
    d = os.path.join(REPO, 'tools', 'gate', 'lanes')
    try:
        names = sorted(n for n in os.listdir(d) if n.endswith('.json'))
    except OSError:
        names = []

    def walk(x, src):
        if isinstance(x, str):
            _scan_text(x, found, src)
        elif isinstance(x, list):
            for y in x:
                walk(y, src)
        elif isinstance(x, dict):
            for y in x.values():
                walk(y, src)

    for n in names:
        try:
            with open(os.path.join(d, n), encoding='utf-8') as f:
                walk(json.load(f), 'tools/gate/lanes/' + n)
        except (OSError, ValueError):
            continue
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
            _scan_text(text, py, os.path.relpath(fp, REPO).replace('\\', '/'))
    return found, py


def gate_inputs():
    """Protected inputs. A name only a tool's source mentions protects the folder while it is in use (touched within TOOL_STALE_DAYS);
    the gate lanes' own inputs are standing and stay protected whatever their age, and so does every required_input() (capture / case /
    run folders a check names: lane 294)."""
    found, py = gate_input_split()
    for k, v in py.items():
        if k not in found and (required_input(k) or touched_within(os.path.join(REPO, k.replace('/', os.sep)), TOOL_STALE_DAYS)):
            found[k] = v
    return found


MISSING_IGNORE_SOURCES = {'tools/renderer/scene_start_check.py'}      # names out/s101/runs/run/case_* as a placeholder, not an input


def missing_gate_inputs():
    """Lane 294: [(path, source)] of gate-named inputs that do not exist now: every path a lane json names, and every required_input()
    a check script names (a glob prefix counts: some scripts name case_<stamp>*). Outputs a lane writes can show here before they ran."""
    lanes, py = gate_input_split()
    out = []
    for k, v in sorted({**{k: v for k, v in py.items() if required_input(k)}, **lanes}.items()):
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
        miss = missing_gate_inputs()
    except Exception as e:
        return ['MISSING GATE INPUT check failed: %r' % (e,)]
    L = ['MISSING GATE INPUT: %s  (named by %s)' % m for m in miss[:limit]]
    if len(miss) > limit:
        L.append('MISSING GATE INPUT: ... and %d more (tidy.py --missing)' % (len(miss) - limit))
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
    """At, under or ABOVE a keep-list / never-touch folder of out\\: the tidy never deletes those, so a hand-written delete must not."""
    if not os.path.normcase(ap).startswith(os.path.normcase(OUT) + os.sep):
        return ''
    r = out_rel(ap)
    for k in sorted(KEEP_TREES | NO_TOUCH):
        if r == k or r.startswith(k + '/'):
            return 'at or under the keep-list / never-touch folder out\\' + k.replace('/', '\\')
        if k.startswith(r + '/'):
            return 'above the keep-list / never-touch folder out\\' + k.replace('/', '\\')
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


def lock_busy():
    """Why the tidy should skip now, or ''."""
    if TEST:
        return ''
    try:
        o = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(REPO, 'tools', 'runlock.ps1'), 'status'],
                           capture_output=True, text=True, errors='replace', timeout=60, creationflags=NO_WINDOW).stdout
    except (OSError, subprocess.TimeoutExpired) as e:
        return 'run lock unreadable (%s)' % e
    for line in o.splitlines():
        s = line.strip()
        if s.startswith('mode:') and s != 'mode: free':
            return s
        if re.match(r'(slot|build)\d: ', s) and not s.endswith(': free') and 'STALE' not in s.upper():
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
    """(text, lane names): tools\\gate\\lanes\\*.json and tools\\**\\*.py, lower case, slashes normalised. Read once, only when a KEEP needs it."""
    if _corpus_cache:
        return _corpus_cache[0]
    parts, lanes = [], set()
    d = os.path.join(REPO, 'tools', 'gate', 'lanes')
    try:
        for n in sorted(os.listdir(d)):
            if n.endswith('.json'):
                lanes.add(n[:-5].lower())
                parts.append(open(os.path.join(d, n), encoding='utf-8', errors='replace').read())
    except OSError:
        pass
    for dp, dn, fn in os.walk(os.path.join(REPO, 'tools')):
        dn[:] = [x for x in dn if x not in ('__pycache__', 'node_modules')]
        for n in fn:
            if n.endswith('.py') and not n.startswith('tidy') and TOOL_READERS.search(n):     # S2: check / selftest / oracle / gate readers only, never generators
                fp = os.path.join(dp, n)
                try:
                    if os.path.getsize(fp) <= TOOLS_PY_MAX:
                        parts.append(open(fp, encoding='utf-8', errors='replace').read())
                except OSError:
                    pass
    text = re.sub(r'[\\/]+', '/', '\n'.join(parts)).lower()
    _corpus_cache.append((text, lanes))
    return _corpus_cache[0]


def keep_info(d):
    """None when d has no KEEP file, else {'valid': bool (holds now), 'kind': str, 'why': str (first words of its reason)}.
    Valid: a reason + an expiry date still ahead; or a reason that a gate lane / tool names; or a bare KEEP younger than
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
        corpus, lanes = reader_corpus()
        try:
            key = repo_key(d)
        except ValueError:
            key = ''
        low = re.sub(r'[\\/]+', '/', reason).lower()
        named = bool(key) and bool(re.search(r'(?<![\w./-])' + re.escape(key) + r'(?![\w-])', corpus))     # a reader names the folder's own path
        named = named or any(os.path.isfile(os.path.join(REPO, t.rstrip('./').replace('/', os.sep))) and
                             (TOOL_READERS.search(os.path.basename(t)) or t.startswith('tools/gate/lanes/')) for t in TOOL_REF_RE.findall(low))
        if named:
            info.update(valid=True, kind='named by a gate lane / tool')
    if not info['valid'] and not info['kind'].startswith('expired'):
        left = st.st_mtime + KEEP_BARE_DAYS * 86400
        if now < left:
            info.update(valid=True, kind='bare: %d-day hold until %s' % (KEEP_BARE_DAYS, stamp(left)))
        else:
            info['kind'] = 'bare, older than %d days (no hold)' % KEEP_BARE_DAYS
    _keep_cache[ck] = info
    return info


def keep_active(d):
    i = keep_info(d)
    return bool(i and i['valid'])


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


class Scan:
    def __init__(self, age, protect, wt_paths, strict=None, now=None):
        self.age = age
        self.strict = strict or age
        self.now = now or time.time()
        self.run_age = Age(self.now - RUN_H * HOUR)         # lane 294: runs that are not the newest of their route
        self.cap = Age(max(self.strict.cutoff, self.now - CAPTURE_COPY_H * HOUR), self.strict.own_since)     # unpacked capture copies
        self.run_newest = {}                                # 's101/runs' -> {names of the newest run per route}
        self.keeps = []                                     # [path, keep_info, bytes] of every KEEP folder seen
        self.prefixes = tuple(k[:-1] for k in protect if k.endswith('*'))     # S4: gate-named glob prefixes
        self.protect = protect                 # repo-relative lower-case keys
        self.wt = wt_paths                     # normcased worktree folders: opaque here
        self.kept = collections.Counter()      # reason -> bytes
        self.named_hits = collections.Counter()
        self.links = 0
        self.version = ''
        try:
            self.version = open(os.path.join(REPO, 'VERSION'), encoding='utf-8').read().strip().lower()
        except OSError:
            pass

    def prot(self, key):
        return key in self.protect or (bool(self.prefixes) and key.startswith(self.prefixes))

    def policy(self, rel, parent):
        if parent.startswith('hold') or parent == 'keep':
            return parent
        if rel in NO_TOUCH:
            return 'hold:never touched'
        if rel in KEEP_TREES:
            return 'keep'
        if self.version and rel == 'release/stage/wwe2k14-recomp-' + self.version:
            return 'keep'
        return 'temp'

    def visit(self, path, rel, policy, strict=False, ao=None):
        """-> (whole folder can go, items, bytes, files, newest, types). ao: an age that wins for this folder and everything under it."""
        types = collections.Counter()
        try:
            entries = list(os.scandir(lp(path)))
            dir_t = os.stat(lp(path)).st_mtime
        except OSError:
            return False, [], 0, 0, time.time(), types
        cap_dir = is_capture_dir([e.name for e in entries])
        in_runs = RUNS_RE.match(rel) is not None
        if not strict and (in_runs or cap_dir):
            strict = True       # S3: the old rule (whole live session + 24 h) for this folder and everything under it
        age = self.strict if strict else self.age
        if ao is None:
            mrun = RUN_CHILD_RE.fullmatch(rel)
            if mrun:        # lane 294: a run folder: the newest of its route keeps the old rule, every other one goes at RUN_H
                ao = self.strict if (mrun.group(2) in self.run_newest.get(mrun.group(1), ()) or not RUN_NAME_RE.match(mrun.group(2))) else self.run_age
            elif cap_dir and not in_runs and not _namespace(rel):
                ao = self.cap       # lane 294: an unpacked capture copy outside a run goes at CAPTURE_COPY_H
        if ao is not None:
            age = ao
        if RUNS_DIR_RE.fullmatch(rel):
            self.run_newest[rel] = newest_run_names([e.name for e in entries if e.is_dir(follow_symlinks=False)])
        keep_row = None
        if any(e.name.lower() == 'keep' and e.is_file(follow_symlinks=False) for e in entries):
            ki = keep_info(path)
            if ki is not None:
                keep_row = [path, ki, 0]
                self.keeps.append(keep_row)
                if ki['valid']:
                    policy = 'hold:KEEP file'
        if any(e.name == '.git' and e.is_file(follow_symlinks=False) for e in entries) and rel != '.':
            b, f, n, _, _ = walk_tree(path)
            self.kept['hold:unregistered git worktree'] += b
            return False, [], b, f, n, types
        logs = [e.name for e in entries if RUNLOG_NUM.match(e.name)]
        top_log = max(logs, key=lambda n: int(RUNLOG_NUM.match(n).group(1))) if logs else None
        whole = policy == 'temp'
        # Moved or robocopied files keep their old dates, but the folder receiving them gets a new one. Busy parent
        # folders that only hold other folders are exempt, or nothing under them would ever expire.
        fresh = policy == 'temp' and rel not in CONTAINERS and not RUNS_DIR_RE.fullmatch(rel) and not age.expired(dir_t)     # lane 294: a runs folder gets a new date with every new run
        items, bytes_, files, newest = [], 0, 0, 0
        child_rows = []
        for e in entries:
            child = os.path.join(path, e.name)
            crel = (rel + '/' + e.name.lower()) if rel != '.' else e.name.lower()
            try:
                st = e.stat(follow_symlinks=False)
            except OSError:
                whole = False
                continue
            if st.st_file_attributes & REPARSE:
                self.links += 1
                whole = False
                continue
            key = 'out/' + crel
            if e.is_dir(follow_symlinks=False):
                if os.path.normcase(child) in self.wt:
                    whole = False
                    continue
                cp = self.policy(crel, policy)
                if not cp.startswith('hold') and self.prot(key):
                    cp = 'hold:named or carried'
                    self.named_hits[crel] += 1
                ok, sub, b, f, n, t = self.visit(child, crel, cp, strict, ao)
                bytes_ += b
                files += f
                newest = max(newest, n)
                if ok and fresh:
                    whole = False
                    self.kept['recent (live session or under the age limit)'] += b
                elif ok:
                    child_rows.append({'p': child, 'b': b, 'n': f, 't': n, 'types': dict(t), 'dir': True})
                    types.update(t)
                else:
                    whole = False
                    items += sub
                continue
            t = born(st)
            bytes_ += st.st_size
            files += 1
            newest = max(newest, t)
            ok, why = self.file_ok(e.name, st, t, policy, rel, top_log, key, age, cap_dir)
            if ok and fresh:
                ok, why = False, 'recent (live session or under the age limit)'
            if ok:
                ty = type_of(e.name)
                child_rows.append({'p': child, 'b': st.st_size, 'n': 1, 't': t, 'types': {ty: st.st_size}, 'dir': False})
                types[ty] += st.st_size
            else:
                whole = False
                self.kept[why] += st.st_size
        if keep_row is not None:
            keep_row[2] = bytes_
        if whole and age.expired(dir_t):
            return True, [], bytes_, files, newest, types
        if is_capture_dir([e.name for e in entries]):
            # a capture folder that does not go whole keeps EVERY file in it: no one-by-one deletion of old frames next to a
            # fresh images.bin (tools re-extract only when images.bin is missing)
            for r in child_rows:
                if not r['dir']:
                    self.kept['hold:capture folder (not all of it expired)'] += r['b']
            child_rows = [r for r in child_rows if r['dir']]
        return False, items + child_rows, bytes_, files, newest, collections.Counter()

    def file_ok(self, name, st, t, policy, rel, top_log, key, age, cap_dir=False):
        if policy.startswith('hold'):
            return False, policy
        if self.prot(key):
            return False, 'hold:named or carried'
        if not age.expired(t):
            return False, 'recent (live session or under the age limit)'
        if self.version and rel == 'release' and name.lower() == 'wwe2k14-recomp-%s.zip' % self.version:
            return False, 'hold:current release'
        if policy == 'keep':
            extra = next((rx for k, rx in KEEP_EXTRA.items() if rel == k or rel.startswith(k + '/')), None)
            if (is_throw(name) and name != top_log) or (extra is not None and extra.search(name)):
                return True, ''
            return False, 'keep list (current builds, tools, indexes)'
        if os.path.splitext(name)[1].lower() in NOTE_EXT and st.st_size <= NOTE_MAX and not (cap_dir and name.lower() in CAPTURE_MARKERS):     # lane 294: a capture's manifest.json is part of the capture, not a note
            return False, 'small notes'
        return True, ''


def scan_out(age, protect, wts, strict=None, now=None):
    wt_paths = {os.path.normcase(w['path']) for w in wts}
    s = Scan(age, protect, wt_paths, strict, now)
    ok, items, *_ = s.visit(OUT, '.', 'keep-root')
    return s, items


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


def check_agent_worktree(w, now, infos, live_ids, branch, protect, carried, merged_h=AGENT_LIVE_H, unmerged_h=AGENT_UNMERGED_H):
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
        w['why'] = 'active within %d h%s' % (hours, '' if merged else ' (branch not merged into %s: kept %d h)' % (branch or '?', hours))
    if w['state'] in ('remove', 'report') and running:
        w['state'] = 'kept'
        w['save_ref'] = False
        w['why'] = 'its agent is still running (its meta has no finished flag)'
    elif w['state'] in ('remove', 'report') and last >= cutoff:
        w['state'] = 'kept'
        w['save_ref'] = False
        w['why'] = 'its agent wrote a transcript %.0f min ago%s' % ((now - last) / 60, '' if merged else ' (branch not merged: kept %d h)' % hours)
    return w


def check_plain_worktree(w, age, now, protect, carried, subs, branches):
    """sdk-wt and out\\wt worktrees (N3): one whose HEAD is not merged into its repo's checked-out branch waits 24 h."""
    repo = w['repo']
    if repo not in branches:
        branches[repo] = session_branch(repo)
    merged = is_merged(repo, w.get('head'), branches[repo])
    check_worktree(w, age if merged else Age(now - AGENT_UNMERGED_H * HOUR), protect, carried, subs)
    w['merged'] = merged
    return w


def check_worktrees(age, protect, now=None, live_ids=None, merged_h=AGENT_LIVE_H, unmerged_h=AGENT_UNMERGED_H, agents_only=False):
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
    if arows:
        infos, branch = agent_infos(), session_branch()
        live_ids = live_ids if live_ids is not None else {s_[0] for s_ in sessions(now)}
    branches = {}
    with ThreadPoolExecutor(8) as pool:
        res = list(pool.map(lambda w: check_plain_worktree(w, age, now, protect, carried, subs, branches), rows))
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
        if not (is_allowed(p) or wt_allowed(p) or in_agent_out(p)) or self.protected_zone(p):
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
        return any(r == n or r.startswith(n + '/') for n in NO_TOUCH)

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
                build = build or nm in BUILD_MARKERS
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


def clear_scratch(o, wt, guard_t, d, dry, skip_build):
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


def scratch_pass(now, dry=False, free=None, skip=()):
    """Rule 3. -> (bytes freed or that would be, lines). Finished agent (meta flag, no later transcript): its out\\ goes once its newest
    file is older than NOW_IDLE_H (merged branch) or AGENT_UNMERGED_H (unmerged: a review may still need it) - S3. Any agent: out\\ over
    SCRATCH_REPORT_GB is reported loudly, and while C: has under SCRATCH_PRESSURE_GB free every top-level folder whose newest file is older
    than SCRATCH_RECENT_MIN goes (pressure overrides the waits), a live agent's build folders excepted. Same refusals as any delete (B2)."""
    free = free_gb() if free is None else free
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
            over = b > SCRATCH_REPORT_GB * GB
            if over:
                lines.append('LOUD: %s out\\ scratch is %s (limit %d GB)' % (name, human(b), SCRATCH_REPORT_GB))
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
                    continue
            else:
                continue
            r = scratch_skip_reason(w, o, d)
            if r:
                lines.append('skipped %s out\\ scratch: %s' % (name, r))
                continue
            got, e = clear_scratch(o, w['path'], guard, d, dry, skip_build=not finished)
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


def safe_scratch(now, dry=False, free=None, skip=()):
    """scratch_pass that can never stop the pass around it (a git timeout, a vanished folder)."""
    try:
        return scratch_pass(now, dry, free, skip)
    except Exception as e:
        history('scratch pass FAILED: %r' % (e,))
        return 0, ['scratch pass failed: %s' % repr(e)[:100]]


def has_agent_out():
    """Cheap: does any agent worktree folder have an out\\ at all (no git, no walk)?"""
    try:
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
    elif not TEST:      # only this project's scratchpads: Temp\claude\<project>\<live session id>
        base = os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Temp', 'claude')
        ids = {s_[0] for s_ in sessions(time.time())}
        for g in glob.glob(os.path.join(glob.escape(base), '*', '*')):
            if os.path.basename(g) in ids and os.path.isdir(g):
                cands.append(('scratchpad\\' + os.path.basename(g)[:8], g, True))

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
    return L


def keep_lines(scan):
    """Rule 2: every KEEP folder with its size and state."""
    if not scan.keeps:
        return []
    L = ['', 'KEEP folders (%d, %s):' % (len(scan.keeps), human(sum(k[2] for k in scan.keeps)))]
    for path, ki, b in sorted(scan.keeps, key=lambda k: -k[2]):
        L.append('  %8s  %s  [%s%s] %s' % (human(b), repo_key(path), 'HOLDS: ' if ki['valid'] else 'NO HOLD: ', ki['kind'], ki['why']))
    return L


# ---------------------------------------------------------------- reporting
def human(b):
    return '%.1f GB' % (b / GB)


def summarize(items, wts):
    area, types = collections.Counter(), collections.Counter()
    for i in items:
        parts = out_rel(i['p']).split('/')
        area['/'.join(parts[:2]) if parts[0] in ('harness', 'android', 'release', 's95') and len(parts) > 2 else parts[0]] += i['b']
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


def report_lines(scan, items, wts, age, live, named):
    cutoff = age.cutoff
    area, types = summarize(items, wts)
    total = sum(types.values())
    L = ['tidy plan %s. Temporary output expires when older than %s (%d h)%s. Capture folders and out\\s*\\runs keep the whole live session + 24 h.' % (
        stamp(), stamp(cutoff), EXPIRE_H,
        ' or made since %s and older than %s (%d h, rolling)' % (stamp(age.rolling[0]), stamp(age.rolling[1]), LIVE_KEEP_H) if age.rolling else ''),
         'live sessions: ' + (', '.join('%s (since %s)' % (s[0][:8], stamp(s[1])) for s in live) or 'none'),
         'C: free now: %.0f GB' % free_gb(), '',
         'WOULD DELETE PERMANENTLY: %s' % human(total), '', 'by type:']
    L += ['  %8s  %s' % (human(v), k) for k, v in types.most_common()]
    L += ['', 'by folder:'] + ['  %8s  %s' % (human(v), k) for k, v in area.most_common(60)]
    rm = [w for w in wts if w['state'] == 'remove']
    L += ['', 'worktrees removed (%d, %s). Their branches stay; commits on no branch are saved under refs/tidy/:' % (
        len(rm), human(sum(w['b'] for w in rm)))]
    L += ['  %8s  %s  [%s]%s%s' % (human(w['b']), repo_key(w['path']), w.get('branch') or 'detached',
                                   '  (commits saved to refs/tidy)' if w.get('save_ref') else '',
                                   '  (merged)' if w.get('merged') else '') for w in sorted(rm, key=lambda x: -x['b'])]
    L += [''] + agent_summary(wts)
    for w in sorted((w for w in wts if w.get('agent') and w['state'] == 'kept' and ('transcript' in w['why'] or 'still running' in w['why'] or w['why'].startswith('active within'))), key=lambda x: os.path.basename(x['path'])):
        L.append('    live: %s [%s] %s' % (os.path.basename(w['path']), 'merged' if w['merged'] else 'not merged', w['why']))
    other = [w for w in wts if w['state'] in ('report', 'kept')]
    L += ['', 'worktrees NOT removed (%d, %s):' % (len(other), human(sum(w['b'] for w in other)))]
    L += ['  %8s  %s  %s: %s' % (human(w['b']), repo_key(w['path']), w['state'], w['why']) for w in sorted(other, key=lambda x: -x['b'])]
    L += ['', 'kept:'] + ['  %8s  %s' % (human(v), k.replace('hold:', '')) for k, v in scan.kept.most_common()]
    L += keep_lines(scan)
    L += ['', 'folders kept only because a state doc names them:'] + ['  out/%s  (%s)' % (k, named.get('out/' + k, '?')) for k in sorted(scan.named_hits)]
    if scan.links:
        L.append('junctions / links left alone in out: %d' % scan.links)
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
def protect_keys():
    named = named_paths()
    for k, v in gate_inputs().items():
        named.setdefault(k, v)
    return set(named) | carried_keys(), named


def cmd_plan(ahead=0.0):
    """ahead > 0: a read-only forecast, the same plan as if it were that many hours later (what the next hours would delete)."""
    real_now = time.time()
    now = real_now + ahead * HOUR
    live = sessions(real_now)
    age, strict = make_ages(now, live)     # lane 154: two intervals, not "since the oldest live session began"
    cutoff = age.cutoff
    protect, named = protect_keys()
    wts = check_worktrees(age, protect, now, {s_[0] for s_ in live})
    scan, items = scan_out(age, protect, wts, strict, now)
    L, total = report_lines(scan, items, wts, age, live, named)
    sc_total, sc_lines = safe_scratch(real_now, True)
    L[3:3] = missing_lines() + budget_lines() + (['agent worktree out\\ scratch (would clear now: %s):' % human(sc_total)] + ['  ' + x for x in sc_lines] if sc_lines else [])
    os.makedirs(TIDY, exist_ok=True)
    name = 'plan_' + time.strftime('%Y%m%d_%H%M') + ('_ahead%g' % ahead if ahead else '')
    if ahead:
        L.insert(0, 'FORECAST: the same plan as if it were %g h from now, assuming the live session goes on (its running agents stay; not applicable).' % ahead)
    save_json(name + '.json', {'made': real_now, 'ahead': ahead, 'cutoff': cutoff, 'items': items, 'worktrees': [w for w in wts if w['state'] == 'remove']})
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


def cmd_auto(trigger, dry, session_id):
    if not os.path.exists(os.path.join(TIDY, 'enabled')) and not dry:
        print('tidy: automatic mode is off until the first clean-up has been approved (--apply)')
        return 0
    now = time.time()
    live = sessions(now)
    age, strict = make_ages(now, live)     # lane 154: two intervals (S1)
    released_before = None
    if trigger == 'end':
        # Own files may go only when this session is known for certain and no other session is live. Otherwise the
        # normal rule applies and the hourly pass collects them a day later.
        me = next((s for s in live if session_id and s[0].startswith(session_id)), None) or (live[0] if len(live) == 1 and not session_id else None)
        if me and len(live) == 1:
            age, strict = make_ages(now, live, own_since=me[1])
            released_before = me[1]
    released = carry_expire(now, released_before=released_before) if not dry else []
    if released:
        history('carry-over released: ' + ', '.join(c['path'] for c in released))
    protect, _ = protect_keys()
    wts = check_worktrees(age, protect, now)
    scan, items = scan_out(age, protect, wts, strict)
    rm = [w for w in wts if w['state'] == 'remove']
    if dry:
        _, types = summarize(items, wts)
        print('tidy --dry (%s): would remove %s; files after %s stay%s' % (
            trigger, human(sum(types.values())), stamp(age.cutoff), ', except this session\'s own' if age.own_since else ''))
        for k, v in types.most_common():
            print('  %8s  %s' % (human(v), k))
        for line in agent_summary(wts):
            print(line)
        sc_total, sc_lines = safe_scratch(now, True)
        for line in sc_lines:
            print(line)
        for line in missing_lines() + budget_lines():
            print(line)
        return 0
    d = Deleter(now)
    d.remove(items, rm, [w for w in wts if w['state'] == 'gone'])
    finish(trigger, d, now)
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
    age, strict = make_ages(now, live, low=True)
    wts = check_worktrees(age, protect, now, merged_h=NOW_IDLE_H)
    scan, items = scan_out(age, protect, wts, strict)
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
    age, strict = make_ages(now, live, low=True)     # exactly the low pass's ages (captures, runs, plain worktrees)
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
            scan, items = scan_out(age, protect, wts, strict)
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
    ap.add_argument('--missing', action='store_true', help='list gate-named inputs that do not exist')
    ap.add_argument('--guard', nargs='+', metavar='PATH', help='exit 1 and name every PATH a delete must refuse (a gate input, inside one, or holding one)')
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
    if os.environ.get('TIDY_REPO') and not os.environ.get('TIDY_TEST_ROOT') and not (a.plan or a.missing or a.guard or (a.dry and (a.now or a.scratch))):
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
    if a.missing:
        ml = missing_gate_inputs()
        print('\n'.join('MISSING GATE INPUT: %s  (named by %s)' % m for m in ml) or 'no gate input is missing')
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
    global LOCKED
    LOCKED = a.locked
    if a.auto == 'watch':       # lane 187: free space only, no scan, no lock: exits at once while there is room
        if not (os.path.exists(os.path.join(TIDY, 'enabled')) or a.dry):
            return 0
        if free_gb() >= LOW_GB:
            # lane 294 S6: with agent worktrees around the watch looks at their out\ scratch, WITHOUT the run lock; only if something would
            # go does it escalate (and take the lock). Cheap when nothing is clearable.
            try:
                if not has_agent_out() or (not TEST and lock_busy()):       # nothing to look at, or a timed run holds the lock: no disk walk now
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
        busy = lock_busy()
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
