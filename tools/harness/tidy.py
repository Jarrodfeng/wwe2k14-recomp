"""The project's automatic disk tidy (Jarrod, 2026-11-09: storage should stay ~75 GB by itself, nobody runs or checks it).

Replaces tools\\harness\\disk_cleanup.ps1. Rules and where they came from: docs\\DECISIONS.md; how sessions use it: CLAUDE.md.

WHAT IT MAY TOUCH: only out\\ and the SDK worktrees in thirdparty\\sdk-wt. Deletes permanently, never follows a junction.
  - out\\ is temporary by default. Anything not on the keep list expires, except small notes (.md .txt .json .toml .py
    .ps1 up to 2 MB: source comments and docs cite them).
  - KEEP_TREES (current builds, tools, indexes) stay, minus the throwaway run files inside them (THROW).
  - NO_TOUCH (bench data, replays, scenarios, recordings, ...) is never touched.
  - Finished git worktrees are removed whole. A worktree with real edits is reported, never removed. Commits that no
    branch holds are saved first under refs/tidy/.

NEVER REMOVED, on every trigger: anything created or changed since the oldest live Claude session started, anything
under a folder with a file named KEEP, anything on the carry-over list (--hold), any path named in STATUS.md, PLAYBOOK.md,
CLAUDE.md or the newest session plan / summary. A game or build holding the run lock = the tidy skips this time.

  py tools\\harness\\tidy.py --plan                  dry run: writes out\\tidy\\plan_<time>.json/.txt, prints the summary
  py tools\\harness\\tidy.py --apply PLAN.json       deletes exactly that list (the first clean-up; also turns --auto on)
  py tools\\harness\\tidy.py --auto start|end|hourly  one pass that decides and deletes (no dry run). Add --dry to only print
  py tools\\harness\\tidy.py --hold PATH "why"       carry a folder over to the next session (released when that session
                                                     ends, or after 7 days)
  py tools\\harness\\tidy.py --holds                 list the carry-over   --release [PATH]  drop one, or all

Triggers: --auto start from the SessionStart hook; --auto end is a step of ending a session (CLAUDE.md "End", after
asking Jarrod when it is not clearly the end or the next session will use the files); --auto hourly from the scheduled
task "wwe2k14-tidy" (below 50 GB free on C: it goes past the 24 h rule, oldest and biggest throwaway types first, never
into live sessions' files, and stops at 75 GB free; if that is not enough it tells Jarrod, at most once a day).
"""
import argparse
import calendar
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

REPO = os.environ.get('TIDY_TEST_ROOT') or os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TEST = bool(os.environ.get('TIDY_TEST_ROOT'))
OUT = os.path.join(REPO, 'out')
SDK = os.path.join(REPO, 'thirdparty', 'rexglue-sdk-src')
TIDY = os.path.join(OUT, 'tidy')
WT_ROOTS = [os.path.join(REPO, 'thirdparty', 'sdk-wt'), os.path.join(SDK, 'thirdparty', 'sdk-wt')]
ALLOWED_ROOTS = [OUT] + WT_ROOTS
PROJECTS = os.path.expanduser(r'~\.claude\projects')
PROJECT_PREFIX = 'C--Users-Jarrod-wwe2k14-recomp'

HOUR = 3600.0
GB = 1e9
EXPIRE_H = 24
LOW_GB = 50
TARGET_GB = 75
LIVE_IDLE_H = 3     # a session whose transcript was written this recently is live
LIVE_MAX_H = 72     # a session left open for days protects at most this far back
LIVE_AGENT_MAIN_H = 12     # its agents' transcripts count only if the session itself wrote within this
CARRY_DAYS = 7
NOTE_MAX = 2 * 1024 * 1024
ALERT_EVERY_H = 24
PLAN_MAX_H = 24     # an older plan is refused: the disk has moved on
REPARSE = stat.FILE_ATTRIBUTE_REPARSE_POINT
WORKERS = 16

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

    def __init__(self, cutoff, own_since=None):
        self.cutoff = cutoff
        self.own_since = own_since

    def expired(self, t):
        return t < self.cutoff or (self.own_since is not None and t >= self.own_since)


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
        return [tuple(x) for x in json.loads(os.environ.get('TIDY_TEST_LIVE', '[]'))]
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
            if depth >= 2 or (k.startswith('thirdparty/') and depth >= 0):
                found.setdefault(k, os.path.basename(f))
    return found


def carry_entries():
    return load_json('carry.json', [])


def carried_keys():
    return {c['path'] for c in carry_entries()}


def carry_expire(now, released_before=None):
    keep, gone = [], []
    for c in carry_entries():
        if now > c['expires'] or (released_before is not None and c['added'] < released_before):
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
                keeps += e.name.lower() == 'keep'
    return b, f, n, links, keeps


def keep_above(path):
    """A KEEP file in any folder above path, up to out\\ or the worktree root."""
    d = os.path.dirname(os.path.abspath(path))
    while is_allowed(d):
        if os.path.exists(lp(os.path.join(d, 'KEEP'))):
            return True
        d = os.path.dirname(d)
    return False


class Scan:
    def __init__(self, age, protect, wt_paths):
        self.age = age
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

    def visit(self, path, rel, policy):
        """-> (whole folder can go, items, bytes, files, newest, types)"""
        types = collections.Counter()
        try:
            entries = list(os.scandir(lp(path)))
            dir_t = os.stat(lp(path)).st_mtime
        except OSError:
            return False, [], 0, 0, time.time(), types
        if any(e.name.lower() == 'keep' and e.is_file(follow_symlinks=False) for e in entries):
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
        fresh = policy == 'temp' and rel not in CONTAINERS and not self.age.expired(dir_t)
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
                if not cp.startswith('hold') and key in self.protect:
                    cp = 'hold:named or carried'
                    self.named_hits[crel] += 1
                ok, sub, b, f, n, t = self.visit(child, crel, cp)
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
            ok, why = self.file_ok(e.name, st, t, policy, rel, top_log, key)
            if ok and fresh:
                ok, why = False, 'recent (live session or under the age limit)'
            if ok:
                ty = type_of(e.name)
                child_rows.append({'p': child, 'b': st.st_size, 'n': 1, 't': t, 'types': {ty: st.st_size}, 'dir': False})
                types[ty] += st.st_size
            else:
                whole = False
                self.kept[why] += st.st_size
        if whole and self.age.expired(dir_t):
            return True, [], bytes_, files, newest, types
        return False, items + child_rows, bytes_, files, newest, collections.Counter()

    def file_ok(self, name, st, t, policy, rel, top_log, key):
        if policy.startswith('hold'):
            return False, policy
        if key in self.protect:
            return False, 'hold:named or carried'
        if not self.age.expired(t):
            return False, 'recent (live session or under the age limit)'
        if self.version and rel == 'release' and name.lower() == 'wwe2k14-recomp-%s.zip' % self.version:
            return False, 'hold:current release'
        if policy == 'keep':
            extra = next((rx for k, rx in KEEP_EXTRA.items() if rel == k or rel.startswith(k + '/')), None)
            if (is_throw(name) and name != top_log) or (extra is not None and extra.search(name)):
                return True, ''
            return False, 'keep list (current builds, tools, indexes)'
        if os.path.splitext(name)[1].lower() in NOTE_EXT and st.st_size <= NOTE_MAX:
            return False, 'small notes'
        return True, ''


def scan_out(age, protect, wts):
    wt_paths = {os.path.normcase(w['path']) for w in wts}
    s = Scan(age, protect, wt_paths)
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
    if not is_allowed(p):
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
    benign = BUILD_SETUP if os.path.normcase(w['repo']) == os.path.normcase(REPO) else subs
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


def check_worktrees(age, protect):
    rows = list_worktrees(REPO) + list_worktrees(SDK)
    rows = [w for w in rows if is_allowed(w['path'])]
    subs = submodule_paths() if os.path.isdir(SDK) else set()
    carried = carried_keys()
    with ThreadPoolExecutor(8) as pool:
        return list(pool.map(lambda w: check_worktree(w, age, protect, carried, subs), rows))


# ---------------------------------------------------------------- deleting
class Deleter:
    def __init__(self, guard_t):
        self.guard = guard_t       # a file created or changed after this moment is left alone
        self.pool = ThreadPoolExecutor(WORKERS)
        self.lock = threading.Lock()
        self.bytes = self.files = self.skipped = 0
        self.errors = []
        self.clean_dirs = set()
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
                return
        except OSError as e:
            self.err(p, e)
            return
        with self.lock:
            self.bytes += size
            self.files += 1

    def clear_dir(self, d):
        subs = []
        try:
            entries = list(os.scandir(lp(d)))
        except OSError as e:
            self.err(d, e)
            return subs
        if any(e.name.lower() == 'keep' for e in entries):
            with self.lock:
                self.skipped += 1
            return subs
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
            else:
                self.unlink(child, st.st_size)
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
        if not is_allowed(p) or self.protected_zone(p):
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
        roots = {os.path.normcase(os.path.abspath(r)) for r in ALLOWED_ROOTS}
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

    def remove(self, items, worktrees):
        files = [i for i in items if not i['dir']]
        list(self.pool.map(self.file, files))
        self.folders([i['p'] for i in items if i['dir']])
        repos = {w['repo'] for w in worktrees if self.worktree(w)}
        for repo in repos:
            git(repo, 'worktree', 'prune')
        self.pool.shutdown()

    def worktree(self, w):
        """Re-checks one worktree right before deleting it. True when it went."""
        p = w['path']
        if not is_allowed(p):
            self.err(p, 'refused: outside the allowed folders')
            return False
        if self.link_on_path(p):
            self.err(p, 'refused: a junction or link at or above it')
            return False
        _, _, t, links, keeps = walk_tree(p)
        if links or keeps or t >= self.guard:
            self.err(p, 'worktree changed since it was checked: left alone')
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


def report_lines(scan, items, wts, cutoff, live, named):
    area, types = summarize(items, wts)
    total = sum(types.values())
    L = ['tidy plan %s. Files created or changed after %s are left alone.' % (stamp(), stamp(cutoff)),
         'live sessions: ' + (', '.join('%s (since %s)' % (s[0][:8], stamp(s[1])) for s in live) or 'none'),
         'C: free now: %.0f GB' % free_gb(), '',
         'WOULD DELETE PERMANENTLY: %s' % human(total), '', 'by type:']
    L += ['  %8s  %s' % (human(v), k) for k, v in types.most_common()]
    L += ['', 'by folder:'] + ['  %8s  %s' % (human(v), k) for k, v in area.most_common(60)]
    rm = [w for w in wts if w['state'] == 'remove']
    L += ['', 'worktrees removed (%d, %s). Their branches stay; commits on no branch are saved under refs/tidy/:' % (
        len(rm), human(sum(w['b'] for w in rm)))]
    L += ['  %8s  %s  [%s]%s' % (human(w['b']), repo_key(w['path']), w.get('branch') or 'detached',
                                 '  (commits saved to refs/tidy)' if w.get('save_ref') else '') for w in sorted(rm, key=lambda x: -x['b'])]
    other = [w for w in wts if w['state'] in ('report', 'kept')]
    L += ['', 'worktrees NOT removed (%d, %s):' % (len(other), human(sum(w['b'] for w in other)))]
    L += ['  %8s  %s  %s: %s' % (human(w['b']), repo_key(w['path']), w['state'], w['why']) for w in sorted(other, key=lambda x: -x['b'])]
    L += ['', 'kept:'] + ['  %8s  %s' % (human(v), k.replace('hold:', '')) for k, v in scan.kept.most_common()]
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
    return set(named) | carried_keys(), named


def cmd_plan():
    now = time.time()
    live = sessions(now)
    cutoff = min([now - EXPIRE_H * HOUR] + [s[1] for s in live])
    protect, named = protect_keys()
    age = Age(cutoff)
    wts = check_worktrees(age, protect)
    scan, items = scan_out(age, protect, wts)
    L, total = report_lines(scan, items, wts, cutoff, live, named)
    os.makedirs(TIDY, exist_ok=True)
    name = 'plan_' + time.strftime('%Y%m%d_%H%M')
    save_json(name + '.json', {'made': now, 'cutoff': cutoff, 'items': items, 'worktrees': [w for w in wts if w['state'] == 'remove']})
    with open(os.path.join(TIDY, name + '.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(L) + '\n')
    print('\n'.join(L[:L.index('every item:')]))
    print('plan: out\\tidy\\%s.json   full list: out\\tidy\\%s.txt   scan took %.1f s' % (name, name, time.time() - now))
    return 0


def cmd_apply(path):
    plan = json.load(open(path, encoding='utf-8'))
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
    return (LOW_ORDER.index(top) if top in LOW_ORDER else len(LOW_ORDER), i['t'])


def cmd_auto(trigger, dry, session_id):
    if not os.path.exists(os.path.join(TIDY, 'enabled')) and not dry:
        print('tidy: automatic mode is off until the first clean-up has been approved (--apply)')
        return 0
    now = time.time()
    live = sessions(now)
    normal = min([now - EXPIRE_H * HOUR] + [s[1] for s in live])
    age, released_before = Age(normal), None
    if trigger == 'end':
        # Own files may go only when this session is known for certain and no other session is live. Otherwise the
        # normal rule applies and the hourly pass collects them a day later.
        me = next((s for s in live if session_id and s[0].startswith(session_id)), None) or (live[0] if len(live) == 1 and not session_id else None)
        if me and len(live) == 1:
            age = Age(now - EXPIRE_H * HOUR, own_since=me[1])
            released_before = me[1]
    released = carry_expire(now, released_before=released_before) if not dry else []
    if released:
        history('carry-over released: ' + ', '.join(c['path'] for c in released))
    protect, _ = protect_keys()
    wts = check_worktrees(age, protect)
    scan, items = scan_out(age, protect, wts)
    rm = [w for w in wts if w['state'] == 'remove']
    if dry:
        _, types = summarize(items, wts)
        print('tidy --dry (%s): would remove %s; files after %s stay%s' % (
            trigger, human(sum(types.values())), stamp(age.cutoff), ', except this session\'s own' if age.own_since else ''))
        for k, v in types.most_common():
            print('  %8s  %s' % (human(v), k))
        return 0
    d = Deleter(now)
    d.remove(items, rm)
    finish(trigger, d, now)
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


def low_pass(live, protect):
    """Under 50 GB: past the 24 h rule, never past the live sessions' start. Stops at 75 GB free."""
    now = time.time()
    cutoff = min([now - HOUR] + [s[1] for s in live])
    age = Age(cutoff)
    wts = check_worktrees(age, protect)
    scan, items = scan_out(age, protect, wts)
    rm = [w for w in wts if w['state'] == 'remove']
    for w in rm:
        items.append({'p': w['path'], 'b': w['b'], 'n': w['n'], 't': w['t'], 'types': {'worktree': w['b']}, 'dir': True, 'wt': w})
    items.sort(key=low_order)
    t0 = now
    d = Deleter(now)
    done = 0
    while done < len(items) and free_gb() < TARGET_GB:
        deficit = (TARGET_GB - free_gb()) * GB
        batch, size = [], 0
        while done < len(items) and len(batch) < 25 and size < deficit:     # never much past the target
            batch.append(items[done])
            size += items[done]['b']
            done += 1
        plain = [i for i in batch if 'wt' not in i]
        files = [i for i in plain if not i['dir']]
        list(d.pool.map(d.file, files))
        d.folders([i['p'] for i in plain if i['dir']])
        for i in batch:
            if 'wt' in i and d.worktree(i['wt']):
                git(i['wt']['repo'], 'worktree', 'prune')
    d.pool.shutdown()
    finish('low disk', d, t0)
    return free_gb()


def cmd_hold(path, why):
    p = os.path.abspath(path if os.path.isabs(path) else os.path.join(REPO, path))
    if not is_allowed(p):
        print('tidy: --hold only makes sense for folders under out\\ or thirdparty\\sdk-wt')
        return 2
    now = time.time()
    rows = [c for c in carry_entries() if c['path'] != repo_key(p)]
    rows.append({'path': repo_key(p), 'why': why, 'added': now, 'expires': now + CARRY_DAYS * 86400})
    save_json('carry.json', rows)
    print('held until the next session ends (at most %d days): %s' % (CARRY_DAYS, repo_key(p)))
    return 0


def cmd_release(path):
    if path:
        key = repo_key(os.path.abspath(path if os.path.isabs(path) else os.path.join(REPO, path)))
        rows = [c for c in carry_entries() if c['path'] != key]
    else:
        rows = []
    save_json('carry.json', rows)
    print('carry-over now holds %d item(s)' % len(rows))
    return 0


def main():
    if sys.stdout is None:      # pythonw
        sys.stdout = sys.stderr = open(os.devnull, 'w')
    ap = argparse.ArgumentParser()
    ap.add_argument('--plan', action='store_true')
    ap.add_argument('--apply')
    ap.add_argument('--auto', choices=['start', 'end', 'hourly'])
    ap.add_argument('--dry', action='store_true')
    ap.add_argument('--session', help='this session id, for --auto end (default: the transcript written most recently)')
    ap.add_argument('--hold', nargs=2, metavar=('PATH', 'WHY'))
    ap.add_argument('--holds', action='store_true')
    ap.add_argument('--release', nargs='?', const='')
    ap.add_argument('--detach', action='store_true', help='return at once and run in the background')
    ap.add_argument('--locked', action='store_true', help=argparse.SUPPRESS)
    a = ap.parse_args()

    if a.holds:
        for c in carry_entries():
            print('%s  (%s, until %s)' % (c['path'], c['why'], stamp(c['expires'])))
        return 0
    if a.hold:
        return cmd_hold(*a.hold)
    if a.release is not None:
        return cmd_release(a.release)
    if a.detach:
        args = [x for x in sys.argv[1:] if x != '--detach']
        exe = sys.executable.replace('python.exe', 'pythonw.exe')
        subprocess.Popen([exe, os.path.abspath(__file__)] + args, creationflags=DETACHED | NEW_GROUP | NO_WINDOW, close_fds=True)
        return 0

    deletes = bool(a.apply) or (a.auto and not a.dry)
    if deletes and not a.locked and not TEST:
        # Checked here, before taking a slot: under our own hold the status would always read busy.
        busy = lock_busy()
        if busy:
            os.makedirs(TIDY, exist_ok=True)
            history('%s: skipped, %s' % (a.auto or 'apply', busy))
            print('tidy: skipped, a game or build is running: ' + busy)
            return 0
        # A shared run-lock hold, so a timed run and the delete never overlap. Short wait: the next trigger tries again.
        rl = os.path.join(REPO, 'tools', 'runlock.ps1')
        cmd = ['pwsh', '-NoProfile', '-File', rl, 'run', '-Shared', '-Name', 'tidy', '-TimeoutMinutes', '1', '-MaxMinutes', '60', '--',
               sys.executable, os.path.abspath(__file__)] + sys.argv[1:] + ['--locked']
        return subprocess.run(cmd, creationflags=NO_WINDOW).returncode

    os.makedirs(TIDY, exist_ok=True)
    fh = open(os.path.join(TIDY, '.run.lock'), 'w')
    try:
        msvcrt.locking(fh.fileno(), msvcrt.LK_NBLCK, 1)
    except OSError:
        print('tidy: another tidy is running')
        return 0
    try:
        if a.plan:
            return cmd_plan()
        if a.apply:
            return cmd_apply(a.apply)
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
