#!/usr/bin/env python3
r"""Hand-back check (lane HBK, 2026-11-20): one fast command a builder runs in its worktree before it hands back. It catches the classes of slip the
Opus reviewers and broken merges kept finding in Session 103 (inventory: docs\agents\handback_checks.md).

  py -I tools\handback\check.py [--base REV] [--json FILE] [--only NAMES] [--skip-compile] [--allow-delete PATH ...] [--root DIR]

The change = BASE -> the working tree of --root (default: the current folder), uncommitted edits and untracked files included. BASE default = the
rule of tools\gate\game_syntax.py --changed (default_base, imported): in a worktree the merge-base of HEAD with the main checkout's branch; on the main
checkout its last-pass sha or HEAD^1.

NEW AGAINST BASE (review fix round): a finding is the builder's only when the same check on the BASE tree does not give it. Each lint / judgement that
finds something at head is run again on the base tree (a detached temporary worktree under out\hbk_base, removed at the end) and only the NEW findings
count (lines compared with digits, slashes and the lints' prefixes normalised). The base result is cached per (base commit, check version, inputs) in
out\hbk_base\cache, so a second run on the same base needs no base tree. A check that cannot be run at base is BLOCKED, never PASS.

One line per check: name, verdict, seconds, a one-line reason; NOTE lines under it (things the hand-back must name, never a failure); a total; then
one final machine-readable line `HANDBACK {json}` (with "partial": true when --only / --skip-compile left checks out: each gets a SKIP (by flag) row).
Verdicts: PASS; FAIL (the check ran and the change is wrong); BLOCKED (the check could not run: it names the missing tool / tree / input);
SKIP (nothing in the diff triggers it). Exit 0 only when there is no FAIL and no BLOCKED (S103: 7,356 of 7,914 gate FAIL rows were "could not run").

Checks, cheapest first (names for --only, comma separated; a letter selects its group, e.g. --only a,e):
  a.deleted        a D line of the diff FAILS unless --allow-delete names it
  a.stale_base     a silent revert of base lines: your added line equals a line one of the file's last 12 base commits removed, while your side drops lines
                   those commits made (or adds back >= 2 such lines); excused only by one of your commits that touches the file and says "revert" + the sha.
                   Also a merge you made dropping lines the base added after that merge's branch point. NOTE: files the base branch changed too.
  a.control_chars  control characters on added lines (tools\lint\no_control_chars.py's scanner) + a lone CR inside a line (the keep_list K44 break)
  a.game_text      tools\harness\game_text_lint.py: an ADDED line of a published file holds game text (string.pac / CSDT, >= 5 words or >= 24 chars): FAIL
  a.build_lists    tools\gate\build_list_lint.py (new against base) + a new src\ .cpp: named by no real build list (CMakeLists.txt, the keep_stream source
                   lists, tools\**\build*.ps1 / .bat), or missing from CMakeLists.txt while its siblings are game units: FAIL
  b.cvar_builds    (a diff under src\ or to a build list) every cvar a game unit reads is declared in that unit, defined by a unit of the same game build, and
                   defined by at most one unit per build (ON = WWE2K14_STREAM() 1, OFF = 0), judged from the REAL preprocessor's text of every game unit
                   (tools\handback\cvar_pp.py: clang -E -fdirectives-only with each unit's own build flags; LNK round 3, 2026-11-20); new against base = FAIL;
                   a unit that cannot be preprocessed, or no clang / configured build = BLOCKED, never PASS
  b.lints          the switch / build / census lints the diff triggers; a non-zero exit is a FAIL unless the base gives the same lines; a crash is BLOCKED
  c.defaults       what the diff turns on for every player (a bool cvar defaulting true, Register* / promotions lines): NOTE only
  d.log_readers    a log / census line whose format string changed: the tools that parse its tag; a reader whose --selftest fails (and passes at base) FAILS
  e.units          units and rigs: added lines that call a function whose only production call sites are inside a hook, without calling that hook function,
                   FAIL (the ARM2 class); an `// entry: <symbol>` line (Python `# entry:`) must name a symbol the unit calls and that is not file-local or
                   hook-only; a unit with no entry line is a NOTE
  f.compile        tools\gate\game_syntax.py --changed BASE --build both (timed apart; --skip-compile leaves it out; game_syntax runs up to 8 clang parses)
No check polls or sleeps. 0 game runs."""
import argparse
import bisect
import collections
import difflib
import hashlib
import itertools
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tools', 'gate'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'lint'))
import tree as _tree   # noqa: E402

T_GIT = 120
VERDICTS = ('PASS', 'FAIL', 'BLOCKED', 'SKIP')
with open(os.path.abspath(__file__), 'rb') as _f:
    CHECK_VERSION = hashlib.sha256(_f.read()).hexdigest()[:12]

# Each rule can be switched off on its own (the planted faults of refind.py / the selftest switch ONE rule off inside its check).
RULES = {k: True for k in ('deleted', 'stale_revert', 'stale_merge', 'base_note', 'ctl', 'lone_cr', 'build_lint', 'new_src', 'cvar_mode', 'cvar_link', 'cvar_dup',
                           'lints', 'reader_selftest', 'unit_bypass', 'unit_entry')}


# ----------------------------------------------------------------------------------------------------------------------------- git / diff

class GitError(RuntimeError):
    pass


class Blocked(RuntimeError):
    """The check could not run (a missing tree, tool, or a stop before the end): its verdict is BLOCKED."""


def git(root, *args, ok_codes=(0,), binary=False, timeout=T_GIT):
    """stdout of one git command; raises GitError on another exit code (a failed git call is never "no output")."""
    try:
        r = subprocess.run(['git', '-C', root] + list(args), capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise GitError('git %s: timed out after %d s' % (' '.join(args[:3]), timeout))
    if r.returncode not in ok_codes:
        raise GitError('git %s: exit %d: %s' % (' '.join(args[:3]), r.returncode, r.stderr.decode('utf-8', 'replace').strip()[:300]))
    return r.stdout if binary else r.stdout.decode('utf-8', 'replace')


HUNK = re.compile(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@')


class Hunk:
    def __init__(self, path, old_start, new_start):
        self.path, self.old_start, self.new_start = path, old_start, new_start
        self.removed = []   # (old line no, text)
        self.added = []     # (new line no, text)


def parse_diff(text):
    """{path: [Hunk]} of a `git diff -U0` text. Hunk bodies are read by their line COUNTS, so an added line `++ x` / removed `-- x` is never taken for a header."""
    hunks, path, cur, ro, rn = {}, None, None, 0, 0
    for ln in text.splitlines():
        if cur is not None and (ro > 0 or rn > 0):
            if ln.startswith('-') and ro > 0:
                cur.removed.append((cur._o, ln[1:])); cur._o += 1; ro -= 1
            elif ln.startswith('+') and rn > 0:
                cur.added.append((cur._n, ln[1:])); cur._n += 1; rn -= 1
            elif ln.startswith('\\'):
                pass   # "\ No newline at end of file"
            else:
                cur._o += 1; cur._n += 1; ro -= 1; rn -= 1   # a context line (not with -U0)
            continue
        if ln.startswith('\\'):
            continue
        if ln.startswith('diff --git'):
            m = re.match(r'diff --git a/(.*) b/(.*)$', ln)
            path, cur = (m.group(2) if m else None), None
            continue
        if ln.startswith('+++ '):
            p = ln[4:].strip()
            if p != '/dev/null':
                path = p[2:] if p.startswith('b/') else p
            continue
        m = HUNK.match(ln)
        if m and path:
            cur = Hunk(path, int(m.group(1)), int(m.group(3)))
            cur._o, cur._n = int(m.group(1)), int(m.group(3))
            ro = int(m.group(2)) if m.group(2) is not None else 1
            rn = int(m.group(4)) if m.group(4) is not None else 1
            hunks.setdefault(path, []).append(cur)
    return hunks


class Change:
    """The change BASE -> working tree of root."""

    def __init__(self, root, base):
        self.root = root
        self.base = git(root, 'rev-parse', '--verify', base + '^{commit}').strip()
        self.status = {}    # path -> 'A' | 'M' | 'D' | 'T' ...
        for ln in git(root, 'diff', '--name-status', '--no-renames', self.base).splitlines():
            parts = ln.split('\t')
            if len(parts) >= 2:
                self.status[parts[1].replace('\\', '/')] = parts[0][0]
        self.untracked = [p.replace('\\', '/') for p in git(root, 'ls-files', '--others', '--exclude-standard').splitlines() if p.strip()]
        for p in self.untracked:
            self.status[p] = 'A'
        self.hunks = parse_diff(git(root, 'diff', '-U0', '--no-renames', '--no-color', self.base))
        for p in self.untracked:
            txt = self.read(p)
            if txt is None or '\0' in txt[:4000]:
                continue
            h = Hunk(p, 0, 1)
            h.added = [(i + 1, t) for i, t in enumerate(txt.splitlines())]
            self.hunks.setdefault(p, []).append(h)
        self.basetree = BaseTree(root, self.base)

    def files(self, *states):
        return sorted(p for p, s in self.status.items() if not states or s in states)

    def added(self, path):
        return [a for h in self.hunks.get(path, []) for a in h.added]

    def removed(self, path):
        return [r for h in self.hunks.get(path, []) for r in h.removed]

    def read(self, rel):
        try:
            with open(os.path.join(self.root, rel), encoding='utf-8', errors='replace') as f:
                return f.read()
        except OSError:
            return None

    def read_bytes(self, rel):
        try:
            with open(os.path.join(self.root, rel), 'rb') as f:
                return f.read()
        except OSError:
            return None


def main_checkout(root):
    out = git(root, 'rev-parse', '--git-common-dir').strip()
    return os.path.dirname(os.path.abspath(os.path.join(root, out)))


def default_base(root):
    """game_syntax --changed's rule (tools\\gate\\game_syntax.py default_base), imported so the two cannot drift."""
    import game_syntax as gs
    return gs.default_base(root, main_checkout(root))


# ----------------------------------------------------------------------------------------------------------------------------- the base tree + cache

BASE_DIR = os.path.join(REPO, 'out', 'hbk_base')            # base trees: BASE_DIR\wt_<pid>_<random> (the selftest points both settings into its temp folder)
CACHE_DIR = os.path.join(BASE_DIR, 'cache')


class BaseTree:
    """The BASE commit on disk, made only when a check needs it (a detached worktree under BASE_DIR, removed by close()). make_secs = the time spent making
    it: the checks' time budget does not count it (8 agents at once can make it slow)."""

    def __init__(self, head_root, base):
        self.head_root, self.base = head_root, base
        self.path, self.err, self.made, self.make_secs = None, None, False, 0.0

    def get(self):
        if self.path:
            return self.path
        if self.err:
            raise Blocked(self.err)
        t0 = time.time()
        try:
            p = _tree.new_name(BASE_DIR)
            _tree.checkout(self.head_root, p, self.base)
        except (_tree.TreeError, OSError, subprocess.TimeoutExpired) as e:
            self.err = 'the base tree could not be made: %s' % e
            raise Blocked(self.err)
        finally:
            self.make_secs += time.time() - t0
        self.path, self.made = p, True
        return p

    def close(self):
        if self.made:
            why = _tree.remove(self.head_root, self.path)
            self.path, self.made = None, False
            return why
        return None


class Deadline:
    """The checks' time budget, started after the change was read; the time spent making the base tree is added back."""

    def __init__(self, ch, budget_s):
        self.ch, self.end = ch, time.time() + budget_s

    def passed(self):
        return time.time() > self.end + self.ch.basetree.make_secs


def cache_key(base, check, *inputs):
    h = hashlib.sha256(('%s|%s|%s' % (CHECK_VERSION, check, '|'.join(str(i) for i in inputs))).encode()).hexdigest()[:20]
    return os.path.join(CACHE_DIR, '%s_%s_%s.json' % (base[:12], check.replace('.', '_'), h))


def cache_get(key):
    try:
        with open(key, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def cache_put(key, value):
    try:
        os.makedirs(os.path.dirname(key), exist_ok=True)
        tmp = key + '.%d.tmp' % os.getpid()
        with open(tmp, 'w', encoding='utf-8') as f:
            json.dump(value, f)
        os.replace(tmp, key)
    except OSError:
        pass


def file_hash(path):
    try:
        with open(path, 'rb') as f:
            return hashlib.sha256(f.read()).hexdigest()[:12]
    except OSError:
        return 'missing'


IMPORT_RX = re.compile(r'^\s*(?:from\s+([\w.]+)\s+import|import\s+([\w., ]+))', re.M)


def deps_hash(script, root):
    """Hash of a lint and the helper modules it imports from the repo (one level: its own folder, tools\\gate, tools\\harness, tools\\lint, tools\\q, tools\\scene):
    a cache entry made with an older helper is not reused (round 3 item 10)."""
    h = hashlib.sha256(file_hash(script).encode())
    try:
        with open(script, encoding='utf-8', errors='replace') as f:
            text = f.read()
    except OSError:
        return h.hexdigest()[:12]
    mods = set()
    for a_, b_ in IMPORT_RX.findall(text):
        for m in ([a_] if a_ else [x.strip().split(' ')[0] for x in b_.split(',')]):
            if m:
                mods.add(m.split('.')[0])
    dirs = [os.path.dirname(script)] + [os.path.join(root, 'tools', d) for d in ('gate', 'harness', 'lint', 'q', 'scene')]
    for m in sorted(mods):
        for d in dirs:
            p = os.path.join(d, m + '.py')
            if os.path.isfile(p):
                h.update(('%s=%s' % (m, file_hash(p))).encode())
                break
    return h.hexdigest()[:12]


PREFIX = re.compile(r'^(?:[-*>|]+\s*)?(?:VIOLATION|PROBLEM|ERROR|FAIL|WARN(?:ING)?|error|warning)\b\s*:?\s*', re.I)
PATH_RX = re.compile(r'\b((?:src|tools|config|docs)/[\w/.+-]+\.\w+)')
RULE_RX = re.compile(r'\b(R\d+[a-z]?|L\d+|F\d+)\b')
SYM_RX = re.compile(r'\b(wwe2k14_\w+|WWE_\w+|k[A-Z]\w+|sub_[0-9A-Fa-f]{8}|0x[0-9A-Fa-f]{6,})\b|"([^"]{2,80})"')


def norm_line(ln):
    """A finding line as compared between base and head: prefixes, digits, slashes, case and spacing do not count (a line number moves when lines above move)."""
    s = ln.strip().replace('\\', '/')
    for _ in range(2):
        s = PREFIX.sub('', s)
    s = re.sub(r'\d+', '#', s)
    return re.sub(r'\s+', ' ', s).strip().lower()


def finding_key(ln):
    """The subtraction key of one finding line. STRUCTURED (file, rule, symbols) when the line names a src\\ / tools\\ path and a rule id (R3, F2) or a symbol
    (a wwe2k14_ cvar, WWE_ env name, kId switch, sub_ address, a quoted name): a moved line number or reworded text then still matches. TEXT (norm_line)
    otherwise (summary lines, lints that print no path or no symbol)."""
    s = ln.strip().replace('\\', '/')
    for _ in range(2):
        s = PREFIX.sub('', s)
    paths = PATH_RX.findall(s)
    rules = RULE_RX.findall(s)
    syms = sorted(set(a or b for a, b in SYM_RX.findall(s)))
    if paths and (rules or syms):
        return ('S', paths[0].lower(), ','.join(rules[:1]), tuple(syms))
    return ('T', norm_line(ln))


def new_lines(head_lines, base_lines, key=finding_key):
    """The head lines whose key is not at base (a multiset: two copies at head, one at base = one new)."""
    left = collections.Counter(key(x) for x in base_lines)
    out = []
    for x in head_lines:
        k = key(x)
        if left[k] > 0:
            left[k] -= 1
        else:
            out.append(x)
    return out


# ----------------------------------------------------------------------------------------------------------------------------- results

class Result:
    def __init__(self, name, verdict, reason, notes=None, details=None, by_flag=False):
        assert verdict in VERDICTS, verdict
        self.name, self.verdict, self.reason = name, verdict, reason
        self.notes = list(notes or [])
        self.details = list(details or [])
        self.by_flag = by_flag
        self.secs = 0.0

    def as_dict(self):
        return {'name': self.name, 'verdict': self.verdict, 'secs': round(self.secs, 2), 'reason': self.reason, 'notes': self.notes, 'details': self.details,
                'by_flag': self.by_flag}


CRASH = re.compile(r'Traceback \(most recent call last\)|^\w*Error: |Exception:', re.M)


def run_script(script, args, cwd, timeout=120):
    """(rc, text). rc None = could not run (timeout / missing)."""
    if not os.path.isfile(script):
        return None, 'missing %s' % script
    try:
        r = subprocess.run([sys.executable, '-I', script] + list(args), capture_output=True, timeout=timeout, cwd=cwd)
    except subprocess.TimeoutExpired:
        return None, '%s timed out after %d s' % (os.path.basename(script), timeout)
    return r.returncode, (r.stdout + r.stderr).decode('utf-8', 'replace')


def tree_script(root, rel):
    """The tree's own copy of a script, else this checkout's."""
    for base in (root, REPO):
        p = os.path.join(base, rel)
        if os.path.isfile(p):
            return p
    return None


def lint_lines(text):
    return [ln.rstrip() for ln in text.splitlines() if ln.strip()]


COULD_NOT_RUN = re.compile(r'^usage: |unrecognized arguments|No such file or directory|FileNotFoundError|PermissionError|\bcould not (?:run|read|open)\b|\bcannot (?:run|read|open)\b'
                           r'|\bnot found\b.*(?:install|PATH|root|\.exe)|^BLOCKED\b|NEEDS-MAIN', re.M | re.I)


def _could_not_run(rc, text):
    """Why a lint run is not a verdict (crash / could-not-run output), or None."""
    if rc is None:
        return text
    if rc != 0 and CRASH.search(text):
        return 'crashed: %s' % (lint_lines(text) or ['?'])[-1][:200]
    if rc != 0:
        m = COULD_NOT_RUN.search(text)
        if m:
            return 'could not run (%s)' % next((ln for ln in lint_lines(text) if COULD_NOT_RUN.search(ln)), m.group(0))[:200]
    return None


def _inside(path, folder):
    return os.path.normcase(os.path.abspath(path)).startswith(os.path.normcase(os.path.abspath(folder)) + os.sep)


def _run_lint(ch, tag, script, target, args_fn, timeout, cacheable):
    """{'rc', 'lines'} of `script` run on the tree `target`; cached (per base commit, the script + its helpers, the args) only when target is the base tree.
    Raises Blocked when the run is not a verdict."""
    holder = ch.basetree.path if ch.basetree.path and _inside(script, ch.basetree.path) else (ch.root if _inside(script, ch.root) else REPO)
    key = cache_key(ch.base, tag, deps_hash(script, holder), ' '.join(args_fn('<root>')))
    got = cache_get(key) if cacheable else None
    if got is None:
        rc, text = run_script(script, args_fn(target), target, timeout)
        why = _could_not_run(rc, text)
        if why:
            raise Blocked('%s: %s' % (tag, why))
        got = {'rc': rc, 'lines': lint_lines(text) if rc else []}
        if cacheable:
            cache_put(key, got)
    return got


def _base_run(ch, name, script, args_fn, timeout, rooted, on_head=False):
    """The lint at base: the head's copy (a lint without --root: the base tree's own copy) on the base tree."""
    bt = ch.basetree.get()   # raises Blocked
    rel = os.path.relpath(script, ch.root) if _inside(script, ch.root) else None
    bscript = script if rooted or rel is None else os.path.join(bt, rel)
    return _run_lint(ch, 'lint.%s at base' % name, bscript, bt, args_fn, timeout, cacheable=True)


def run_against_base(ch, name, script, args_fn, timeout, rooted=True):
    """Run a lint at head; when it exits non-zero, run the SAME script (a lint without --root: the base tree's own copy) on the base tree and keep the new findings.
    A crash or "could not run" output at head or base is BLOCKED, never subtracted. When the diff edits the lint itself, the BASE's copy also runs on the head
    tree (a weakened lint that now exits 0 must not pass the change it was weakened for). Returns (verdict, reason, new_lines, notes)."""
    rc, text = run_script(script, args_fn(ch.root), ch.root, timeout)
    why = _could_not_run(rc, text)
    if why:
        return 'BLOCKED', '%s at head: %s' % (name, why), [], []
    notes, new = [], []
    if rc != 0:
        got = _base_run(ch, name, script, args_fn, timeout, rooted)
        new = new_lines(lint_lines(text), got['lines'])
        if not new:
            notes.append('%s exits %d at base with the same %d finding(s): not yours' % (name, rc, len(lint_lines(text))))
    rel = os.path.relpath(script, ch.root).replace('\\', '/') if _inside(script, ch.root) else None
    if rel and ch.status.get(rel) == 'M' and rooted:
        bt = ch.basetree.get()
        bcopy = os.path.join(bt, rel)
        if os.path.isfile(bcopy):
            on_head = _run_lint(ch, 'lint.%s base copy on the head tree' % name, bcopy, ch.root, args_fn, timeout, cacheable=False)
            if on_head['rc']:
                at_base = _run_lint(ch, 'lint.%s base copy at base' % name, bcopy, bt, args_fn, timeout, cacheable=True)
                extra = new_lines(on_head['lines'], at_base['lines'])
                if extra:
                    new += ['(the base\'s copy of %s, which your diff edits, finds) %s' % (rel, x) for x in extra]
    if not new:
        return 'PASS', '', [], notes
    shown = [x for x in new if not re.match(r'^\s*(?:PASS|ok)\b', x)]
    return 'FAIL', '%s: %s' % (name, (shown or new)[0].strip()[:220]), shown or new, notes


SIGNIFICANT = re.compile(r'[A-Za-z0-9]')


def significant(text):
    t = text.strip()
    return len(t) >= 16 and SIGNIFICANT.search(t) is not None


def ws(text):
    """A line with its whitespace collapsed (a re-indented revert is still a revert)."""
    return ' '.join(text.split())


# ----------------------------------------------------------------------------------------------------------------------------- check a

def check_deleted(ch, a):
    allow = set(p.replace('\\', '/').strip('/') for p in (a.allow_delete or []))
    dels = ch.files('D') if RULES['deleted'] else []
    bad = [p for p in dels if p not in allow]
    if not dels:
        return Result('a.deleted', 'PASS', 'no file deleted')
    if not bad:
        return Result('a.deleted', 'PASS', '%d deleted, all named in --allow-delete' % len(dels), notes=['deleted (allowed): ' + p for p in dels])
    return Result('a.deleted', 'FAIL', '%d file(s) deleted that --allow-delete does not name: %s%s' % (len(bad), ', '.join(bad[:4]), ' ...' if len(bad) > 4 else ''),
                  details=['D ' + p for p in bad])


def _own_commits(ch):
    """[(sha, message, set of files)] of BASE..HEAD."""
    out = []
    try:
        raw = git(ch.root, 'log', '--format=%x01%H%x02%B%x03', '--name-only', '%s..HEAD' % ch.base)
    except GitError:
        return out
    for chunk in raw.split('\x01')[1:]:
        sha, rest = chunk.split('\x02', 1)
        msg, files = rest.split('\x03', 1) if '\x03' in rest else (rest, '')
        out.append((sha.strip(), msg, set(f.strip() for f in files.splitlines() if f.strip())))
    return out


def _excused(own, c, path):
    """A revert of c in `path` is meant when one of your commits that touches the file says "revert" and names c."""
    for sha, msg, files in own:
        if path in files and re.search(r'revert', msg, re.I) and c[:9] in msg:
            return sha
    return None


def _blame(root, base, path, ranges):
    """{old line no: (sha or None for a boundary, the file name at that commit)} via --line-porcelain (survives renames)."""
    res = {}
    for i in range(0, len(ranges), 150):   # chunked: never truncated
        args = ['blame', '--line-porcelain']
        for s, e in ranges[i:i + 150]:
            args += ['-L', '%d,%d' % (s, e)]
        out = git(root, *(args + [base, '--', path]))
        cur = None
        for ln in out.splitlines():
            m = re.match(r'^([0-9a-f]{40}) (\d+) (\d+)(?: \d+)?$', ln)
            if m:
                cur = [m.group(1), int(m.group(3)), path, False]
                continue
            if cur is None:
                continue
            if ln == 'boundary':
                cur[3] = True
            elif ln.startswith('filename '):
                cur[2] = ln[len('filename '):]
            elif ln.startswith('\t'):
                res[cur[1]] = (None if cur[3] else cur[0], cur[2])
                cur = None
    return res


class History:
    """The last commits of a file on base and the lines each removed / added (cached per (commit, path))."""

    def __init__(self, root, base):
        self.root, self.base, self.cache, self.recent = root, base, {}, {}

    def lines(self, c, path):
        """(removed, added, pairs) of commit c in path, whitespace collapsed; pairs = {(added, removed)} of its one-for-one changed lines."""
        key = (c, path)
        if key not in self.cache:
            rem, add, pairs = set(), set(), set()
            parents = git(self.root, 'rev-list', '--parents', '-n', '1', c).split()
            if len(parents) >= 2:
                d = git(self.root, 'diff', '-U0', '--no-color', '--no-renames', parents[1], c, '--', path)
                for h in parse_diff(d).values():
                    for hk in h:
                        rem.update(ws(t) for _n, t in hk.removed if ws(t))
                        add.update(ws(t) for _n, t in hk.added if ws(t))
                        if len(hk.removed) == len(hk.added):
                            pairs.update((ws(a_), ws(r_)) for (_m, r_), (_n, a_) in zip(hk.removed, hk.added))
            self.cache[key] = (rem, add, pairs)
        return self.cache[key]

    def last(self, path, n=12):
        if path not in self.recent:
            self.recent[path] = [x for x in git(self.root, 'log', '-n', str(n), '--no-merges', '--format=%H', self.base, '--', path).split() if x]
        return self.recent[path]


def _merge_branch_points(ch):
    bps = set()
    for ln in git(ch.root, 'rev-list', '--merges', '--parents', '%s..HEAD' % ch.base).splitlines():
        ps = ln.split()[1:]
        if len(ps) >= 2:
            mb = git(ch.root, 'merge-base', ps[0], ps[1], ok_codes=(0, 1)).strip()
            if mb:
                bps.add(mb)
    return bps


def check_stale_base(ch, a, deadline):
    notes, fails = [], []
    mods = [p for p in ch.files('M') if ch.hunks.get(p)]
    if RULES['base_note']:
        try:
            md = main_checkout(ch.root)
            br = git(md, 'rev-parse', '--abbrev-ref', 'HEAD').strip()
            tip = git(ch.root, 'rev-parse', '--verify', '-q', br + '^{commit}', ok_codes=(0, 1)).strip()
            if tip:
                bp = git(ch.root, 'merge-base', 'HEAD', tip, ok_codes=(0, 1)).strip()
                if bp:
                    theirs = set(x.replace('\\', '/') for x in git(ch.root, 'diff', '--name-only', '--no-renames', bp, tip).splitlines())
                    for p in sorted(theirs & set(ch.status)):
                        notes.append('base branch %s changed %s since your branch point %s: merge it in and check the result' % (br, p, bp[:9]))
        except GitError as e:
            notes.append('base-branch list not made: %s' % e)
    own = _own_commits(ch)
    hist = History(ch.root, ch.base)
    bps = _merge_branch_points(ch) if RULES['stale_merge'] else set()
    anc = {}
    excused = []
    for i, p in enumerate(mods):
        if deadline.passed():
            raise Blocked('stopped at the time budget: %d of %d modified files judged (the rest unknown)' % (i, len(mods)))
        hunks = ch.hunks[p]
        any_added = any(ws(t) for h in hunks for _n, t in h.added)
        if not any_added and not bps:
            continue
        recent = hist.last(p) if any_added else []
        with_removed = [h for h in hunks if h.removed]
        bl = _blame(ch.root, ch.base, p, [(h.removed[0][0], h.removed[-1][0]) for h in with_removed]) if with_removed else {}
        for h in hunks:
            if deadline.passed():
                raise Blocked('stopped at the time budget inside %s' % p)
            restored = collections.defaultdict(list)   # c -> [(line no, text)]
            ours = dict(zip((ws(t) for _n, t in h.added), (ws(t) for _n, t in h.removed))) if len(h.added) == len(h.removed) else {}
            for n, t in h.added:
                w = ws(t)
                if not w:
                    continue
                for c in recent:
                    rem, _add, pairs = hist.lines(c, p)
                    # a long line put back, or a short one whose exact (old, new) pair c made the other way round (`return true;` <-> `return false;`)
                    if w in rem and (significant(t) or (w in ours and (ours[w], w) in pairs)):
                        restored[c].append((n, t))
                        break
            if RULES['stale_revert'] and restored:
                dropped = set(bl.get(n, (None, p))[0] for n, t in h.removed if ws(t)) - {None}
                for c, rows in restored.items():
                    if h.removed:
                        # the lines you drop must be c's own, or those of a later commit that changed what c wrote (A -> B by c, B -> B2 by d, you put A back)
                        chain = [d for d in dropped if d != c and hist.lines(d, p)[0] & hist.lines(c, p)[1]]
                        hit = ({c} & dropped) or set(chain)
                        if not hit:
                            continue
                    else:
                        hit = set()
                        c_rem = hist.lines(c, p)[0]
                        if not (len(rows) >= 2 or (len(rows) == 1 and c_rem == {ws(rows[0][1])})):
                            continue   # one restored line counts only when it is all c removed in this file
                    ex = _excused(own, c, p)
                    if ex:
                        excused.append('%s: the revert of %s is named by your commit %s' % (p, c[:9], ex[:9]))
                        continue
                    via = sorted(hit)[0][:9] if hit else None
                    fails.append('%s:%d: your side puts back %d line(s) that base commit %s removed%s (silent revert; first: %s)'
                                 % (p, rows[0][0], len(rows), c[:9], (' and drops lines of %s' % via) if via else ' (pure addition)', rows[0][1].strip()[:100]))
            for bp in bps:
                for n, t in h.removed:
                    c = bl.get(n, (None, p))[0]
                    if not c or not significant(t):
                        continue
                    if (c, bp) not in anc:
                        anc[(c, bp)] = subprocess.run(['git', '-C', ch.root, 'merge-base', '--is-ancestor', c, bp], capture_output=True, timeout=T_GIT).returncode == 0
                    if anc[(c, bp)] or _excused(own, c, p):
                        continue
                    cands = [x for _m, x in h.added][:60]
                    if any(difflib.SequenceMatcher(None, t.strip(), x.strip()).quick_ratio() >= 0.6 and difflib.SequenceMatcher(None, t.strip(), x.strip()).ratio() >= 0.6 for x in cands):
                        continue
                    fails.append('%s:%d: removes a line base commit %s added after your merge\'s branch point %s: %s' % (p, n, c[:9], bp[:9], t.strip()[:100]))
    notes += excused
    if fails:
        return Result('a.stale_base', 'FAIL', '%d silent revert(s) of base lines: %s' % (len(fails), fails[0][:220]), notes=notes, details=fails)
    return Result('a.stale_base', 'PASS', '%d modified file(s) judged against their last base commits, no base line reverted' % len(mods), notes=notes)


def check_control_chars(ch, a):
    try:
        import no_control_chars as ncc
    except Exception as e:
        return Result('a.control_chars', 'BLOCKED', 'tools\\lint\\no_control_chars.py not importable: %s' % e)
    hits, n = [], 0
    for p in ch.files('A', 'M'):
        if os.path.splitext(p)[1].lower() not in ncc.EXT:
            continue
        data = ch.read_bytes(p)
        if data is None:
            continue
        n += 1
        added = set(ln for ln, _t in ch.added(p))
        if RULES['ctl']:
            for line, byte, ctx in ncc.scan_bytes(data):
                if line in added:
                    hits.append('%s:%d: 0x%02X near "%s"' % (p, line, byte, ctx))
        if RULES['lone_cr']:
            # no_control_chars allows CR (CRLF files); a CR that does not end its line splits it for every reader (keep_list K44)
            for i, raw in enumerate(data.split(b'\n'), 1):
                if i in added and b'\r' in raw.rstrip(b'\r'):
                    hits.append('%s:%d: a lone CR inside the line near "%s"' % (p, i, raw[:raw.index(b'\r')][-20:].decode('utf-8', 'replace')))
    if not n:
        return Result('a.control_chars', 'SKIP', 'no changed text file')
    if hits:
        return Result('a.control_chars', 'FAIL', '%d control character(s) on added lines: %s' % (len(hits), hits[0]), details=hits)
    return Result('a.control_chars', 'PASS', '%d changed text file(s), no control character on an added line' % n)


def check_game_text(ch, a):
    """S105 R1L: rule 1. Game text on an added line of a published file (the player's own string tables / captions)."""
    try:
        sys.path.insert(0, os.path.join(REPO, 'tools', 'harness'))
        import game_text_lint as gtl
        idx = gtl.Index(gtl.load_set())
    except Exception as e:
        return Result('a.game_text', 'BLOCKED', 'tools/harness/game_text_lint.py cannot run (game data missing?): %s' % e)
    allow = gtl.load_allow()
    hits, n = [], 0
    for p in ch.files('A', 'M'):
        if gtl.is_local(p) or p in gtl.SELF or p.lower().endswith(gtl.SKIP_EXT):
            continue
        data = ch.read_bytes(p)
        if data is None or len(data) > gtl.MAX_FILE or b'\0' in data[:4096]:
            continue
        n += 1
        added = set(ln for ln, _t in ch.added(p))
        for line, label, first3 in idx.find(data.decode('utf-8', 'replace')):
            if line in added and not gtl.allowed(allow, p, label):
                hits.append('%s:%d: game text %s ("%s ...")' % (p, line, label, first3))
    if not n:
        return Result('a.game_text', 'SKIP', 'no changed published text file')
    if hits:
        return Result('a.game_text', 'FAIL', "%d line(s) of the game's own text (rule 1): %s" % (len(hits), hits[0]), details=hits)
    return Result('a.game_text', 'PASS', '%d changed published file(s), no game text on an added line' % n)


def _real_lists(ch):
    """The real build lists of the tree: CMakeLists.txt, the keep_stream source lists, tools\\**\\build*.ps1 / .bat. {path: text}."""
    out = {}
    for rel in ('CMakeLists.txt', 'tools/scene/keep_stream_sources.txt', 'tools/scene/keep_stream_off_sources.txt'):
        t = ch.read(rel)
        if t is not None:
            out[rel] = t.replace('\\', '/')
    for dp, _dn, fn in os.walk(os.path.join(ch.root, 'tools')):
        for f in fn:
            if f.lower().startswith('build') and f.lower().endswith(('.ps1', '.bat')):
                rel = os.path.relpath(os.path.join(dp, f), ch.root).replace('\\', '/')
                t = ch.read(rel)
                if t is not None:
                    out[rel] = t.replace('\\', '/')
    return out


def check_build_lists(ch, a):
    srcs = [p for p in ch.files('A', 'M') if (p.endswith(('.cpp', '.h', '.ps1', '.py', '.bat')) and p.startswith(('src/', 'tools/'))) or p == 'CMakeLists.txt']
    if not srcs:
        return Result('a.build_lists', 'SKIP', 'no src\\ / tools\\ source or build list changed')
    fails, notes = [], []
    if RULES['build_lint']:
        script = tree_script(ch.root, 'tools/gate/build_list_lint.py')
        if not script:
            return Result('a.build_lists', 'BLOCKED', 'tools\\gate\\build_list_lint.py not found')
        v, why, new, nts = run_against_base(ch, 'build_list_lint', script, lambda r: ['--root', r], 120)
        notes += nts
        if v == 'BLOCKED':
            return Result('a.build_lists', 'BLOCKED', why, notes=notes)
        if v == 'FAIL':
            fails += [x.strip()[:240] for x in new]
    lists = _real_lists(ch) if RULES['new_src'] else {}
    cm = lists.get('CMakeLists.txt', '')
    off = lists.get('tools/scene/keep_stream_off_sources.txt', '')
    for p in ch.files('A'):
        if not RULES['new_src'] or not p.endswith('.cpp') or not p.startswith('src/'):
            continue
        named = sorted(rel for rel, t in lists.items() if p in t or (rel.startswith('tools/') and rel.endswith(('.ps1', '.bat')) and p[4:] in t))
        if not named:
            fails.append('%s: a new src unit named by no build list (CMakeLists.txt, the keep_stream source lists, a tools build*.ps1 / .bat): nothing compiles it' % p)
            continue
        d = os.path.dirname(p)
        try:
            sib = [s for s in os.listdir(os.path.join(ch.root, d)) if s.endswith('.cpp') and s != os.path.basename(p) and (d + '/' + s) in cm]
        except OSError:
            sib = []
        if sib and p not in cm and p not in off:
            fails.append('%s: its siblings (%s) are game units in CMakeLists.txt, it is not (only in %s): the game build does not compile it' % (p, ', '.join(sib[:3]), ', '.join(named[:3])))
    for p in ch.files('A'):
        if p.endswith('.cpp') and p.startswith('tools/') and RULES['new_src']:
            if not any(os.path.basename(p) in t for rel, t in lists.items() if rel.startswith('tools/')):
                notes.append('%s: no tools build script names it yet' % p)
    if fails:
        return Result('a.build_lists', 'FAIL', fails[0][:240], notes=notes, details=fails)
    return Result('a.build_lists', 'PASS', 'build_list_lint: nothing new against base; %d new src .cpp checked' % len([p for p in ch.files('A') if p.endswith('.cpp') and p.startswith('src/')]), notes=notes)


# ----------------------------------------------------------------------------------------------------------------------------- check b

CVAR_USE = re.compile(r'REXCVAR_(GET|SET|DEFINE_[A-Z0-9_]+|DECLARE)\s*\(\s*([^,()]+?)\s*(?:,\s*([^,()]+?)\s*)?[,)]')
DIRECTIVE = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif|else|endif|include)\b\s*(.*)$')


def cvar_names(line):
    """[(kind, name)]: REXCVAR_GET(name) / SET(name, v) / DEFINE_*(name, default, ...) take the name first; DECLARE(type, name) second."""
    out = []
    for m in CVAR_USE.finditer(line):
        kind = m.group(1)
        name = ((m.group(3) if kind == 'DECLARE' else m.group(2)) or '').strip()
        if re.match(r'^[A-Za-z_]\w*$', name):
            out.append((kind, name))
    return out


def _read(root, rel):
    try:
        with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
            return f.read()
    except OSError:
        return None


def cmake_sources(text):
    """{src/..cpp|.cc} the GAME target compiles per CMakeLists.txt (LNK review item 3): the paths inside `set(WWE2K14_SOURCES ...)` and `target_sources(wwe2k14 ...)`, in code only
    (a `#` comment is not a source; before, a substring test on the whole text counted a commented-out .cpp and the launcher's sources as game units). The keep_stream lists are
    read apart (cvar_findings). Not read: generator expressions, list(APPEND ...) of other variables; every `if(ANDROID)` / `if(WIN32)` branch counts."""
    code = '\n'.join(ln.split('#', 1)[0] for ln in text.replace('\\', '/').splitlines())
    out = set()
    for m in re.finditer(r'\b(?:set\s*\(\s*WWE2K14_SOURCES|target_sources\s*\(\s*wwe2k14)(?=[\s)])', code):
        depth, i = 1, code.index('(', m.start()) + 1
        start = i
        while i < len(code) and depth:
            depth += {'(': 1, ')': -1}.get(code[i], 0)
            i += 1
        out.update(re.findall(r'(?<![\w/.-])src/[\w/.-]+\.(?:cpp|cc)(?![\w.])', code[start:i]))
    return out


def cvar_findings(root, names=None, notes=None, overlay=None):
    """The judgement of one tree from what each game unit REALLY compiles (LNK round 3: tools\\handback\\cvar_pp.py runs clang -E -fdirectives-only with the unit's own build flags,
    per mode; nothing about the preprocessor is modelled here). -> (findings [(key, message)], rows, blocked [(unit, mode, first error line)]):
      undeclared  a unit reads a cvar its own text never declares or defines (a compile error)
      link        a game build reads a cvar that no unit of that build defines (REXCVAR_DEFINE_*, or an inline FLAGS_<name>_storage_() { ... })
      duplicate   two or more units of one build define it (REXCVAR_DEFINE_* has external linkage); key (mode, cvar) + the unit count
    A cvar that a header outside our src declares or defines (the SDK's own: clock_no_scaling) is the SDK library's and is not judged. `names` (None = every cvar) limits the cvars;
    `overlay` {path: text} serves planted texts (the lint's twins). A unit cvar_pp could not preprocess is in `blocked`, never judged clean; a unit it does not judge (a platform branch)
    goes to `notes`. RuntimeError when nothing can be judged (no clang, no configured build)."""
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)   # (py -I does not put the script's folder on the path)
    import cvar_pp
    out, blocked, not_judged = cvar_pp.judge_tree(root, overlay=overlay)
    if notes is not None:
        for u, mode, why in not_judged:
            notes.append('%s (%s) not judged: %s' % (u, 'ON' if mode else 'OFF', why))
    found, rows = [], 0
    # every iteration is sorted: a message naming another file at base than at head would look new (round 3 item 4); the subtraction keys on (unit, mode, cvar, kind)
    for mode in (1, 0):
        mtxt = 'ON' if mode else 'OFF'
        units = out[mode]
        defined, anydef, ext = {}, set(), set()
        for u in sorted(units):
            r = units[u]
            for n in r['def']:
                defined.setdefault(n, []).append(u)
            anydef.update(r['def'])
            anydef.update(r['inline'])
            ext.update(r['ext'])
        for u in sorted(units):
            r = units[u]
            own = set(r['decl']) | set(r['def']) | set(r['inline'])
            for nm in r['read']:
                if (names is not None and nm not in names) or nm in ext:
                    continue
                rows += 1
                if nm not in own:
                    if RULES['cvar_mode']:
                        found.append(([u, mtxt, nm, 'undeclared'], '%s (%s game build): reads %s, but nothing in that unit declares or defines it (a compile error)' % (u, mtxt, nm)))
                    continue
                if RULES['cvar_link'] and nm not in anydef:
                    found.append(([u, mtxt, nm, 'link'], '%s (%s game build): reads %s, declared, but no unit of the %s game build defines it (REXCVAR_DECLARE is only a declaration: a '
                                                          'link error)' % (u, mtxt, nm, mtxt)))
        if RULES['cvar_dup']:
            for nm in sorted(defined):
                if (names is not None and nm not in names) or nm in ext:
                    continue
                us = sorted(set(defined[nm]))
                if len(us) > 1:
                    # LNK review item 4: the key is (mode, cvar) + the unit count; check_cvar_builds calls it old when base has the same (mode, cvar) with as many units or more
                    found.append((['*', mtxt, nm, 'duplicate', len(us)], '%s is defined in %d units of the %s game build (a duplicate symbol at link): %s' % (nm, len(us), mtxt, ', '.join(us))))
    # an OFF-only game unit that a tool build compiles in ON (cvar_pp mode 2; S103 SCS): only `undeclared` applies (a compile error of that tool build; it links nothing of the game's)
    for u in sorted(out.get(2, {})):
        r = out[2][u]
        own = set(r['decl']) | set(r['def']) | set(r['inline'])
        for nm in r['read']:
            if (names is not None and nm not in names) or nm in r['ext']:
                continue
            rows += 1
            if nm not in own and RULES['cvar_mode']:
                found.append(([u, 'ON-tool', nm, 'undeclared'], '%s (ON, as a tool build compiles it: an OFF-only game unit): reads %s, but nothing in that unit declares or defines it '
                                                                  'in ON (a compile error of that tool build)' % (u, nm)))
    return found, rows, blocked


BUILD_LIST_FILES = ('CMakeLists.txt', 'tools/scene/keep_stream_sources.txt', 'tools/scene/keep_stream_off_sources.txt')


def check_cvar_builds(ch, a):
    # LNK round 3: any change under src\ or to a build list can change what a unit compiles (an #include, an #if, a define, a unit moved): every cvar of the tree is judged, from
    # the real preprocessor's text (cvar_findings / cvar_pp); only findings new against base are the builder's
    touched = [p for p in ch.hunks if p.startswith('src/') or p in BUILD_LIST_FILES]
    if not touched:
        return Result('b.cvar_builds', 'SKIP', 'the diff changes nothing under src\\ and no build list')
    jnotes = []
    try:
        head, rows, blocked = cvar_findings(ch.root, None, jnotes)
    except RuntimeError as e:
        return Result('b.cvar_builds', 'BLOCKED', 'the cvar judge cannot preprocess the game units: %s' % str(e)[:250])
    jnotes = jnotes[:8]
    blocked_txt = ['BLOCKED (not judged): %s [%s]: %s' % (u, 'ON' if m else 'OFF', e) for u, m, e in blocked]
    if not head and not blocked:
        return Result('b.cvar_builds', 'PASS', '%d (unit, mode) reads of every cvar, each declared and defined where it is read (real preprocessor)' % rows, notes=jnotes)
    if not head:
        return Result('b.cvar_builds', 'BLOCKED', '%d unit-build(s) could not be preprocessed (never judged clean); first: %s' % (len(blocked), blocked_txt[0][:250]),
                      notes=blocked_txt[:8] + jnotes)
    import cvar_pp
    key = cache_key(ch.base, 'b.cvar_builds', 'all', cvar_pp.SCAN_VERSION, ','.join(k for k, v in sorted(RULES.items()) if v))
    got = cache_get(key)
    if got is None:
        bt = ch.basetree.get()
        try:
            got = {'keys': [k for k, _m in cvar_findings(bt, None)[0]]}
        except RuntimeError as e:
            return Result('b.cvar_builds', 'BLOCKED', 'the base tree cannot be preprocessed, so no finding can be called new or old: %s' % str(e)[:200],
                          notes=[m for _k, m in head][:8])
        cache_put(key, got)
    at_base = set(tuple(k) for k in got['keys'])
    # LNK review item 4: a duplicate is old when base has the same (mode, cvar) duplicate with as many units or more (a copy removed, or an unrelated include, is not new)
    base_dup = {(k[1], k[2]): k[4] for k in got['keys'] if len(k) > 4 and k[3] == 'duplicate'}

    def old(k):
        if len(k) > 4 and k[3] == 'duplicate':
            return base_dup.get((k[1], k[2]), 0) >= k[4]
        return tuple(k) in at_base
    new = [m for k, m in head if not old(k)]
    notes = blocked_txt[:8] + ['at base too (not yours): ' + m for k, m in head if old(k)][:8] + jnotes
    if new:
        return Result('b.cvar_builds', 'FAIL', new[0][:300], notes=notes, details=new)
    if blocked:
        return Result('b.cvar_builds', 'BLOCKED', '%d unit-build(s) could not be preprocessed (never judged clean); first: %s' % (len(blocked), blocked_txt[0][:250]), notes=notes)
    return Result('b.cvar_builds', 'PASS', '%d (unit, mode) reads; %d finding(s) all present at base' % (rows, len(head)), notes=notes)


CENSUS_WORDS = re.compile(r'[Cc]ensus|LogFinal|\b600\b|"\[(?:scene|rdr|owner)\]')
LINTS = [
    # (name, script, trigger(ch, s) -> bool, args(root), timeout, rooted)
    ('dev_cvar', 'tools/scene/dev_cvar_lint.py', lambda ch, s: s['src'], lambda r: ['--root', r], 60, True),
    ('getenv', 'tools/scene/getenv_lint.py', lambda ch, s: 'getenv' in s['text'], lambda r: ['--root', r], 60, True),
    ('rule_register', 'tools/renderer/rule_register_lint.py', lambda ch, s: 'SHADER-INPUT' in s['text'], lambda r: ['--root', r], 60, True),
    ('switch_defined', 'tools/scene/switch_defined_lint.py', lambda ch, s: s['src'], lambda r: ['--root', r], 60, True),
    ('version_define', 'tools/gate/version_define_lint.py', lambda ch, s: 'WWE2K14_VERSION' in s['text'], lambda r: ['--root', r], 120, True),
    ('start_parity', 'tools/scene/start_parity_lint.py', lambda ch, s: bool(s['start_files']), lambda r: ['--root', r], 60, True),
    ('register_switch', 'tools/scene/register_switch_lint.py', lambda ch, s: bool({'src/wwe2k14_app.h', 'src/bundle3.h'} & set(ch.status)), lambda r: ['--root', r], 120, True),
    ('census_flush', 'tools/renderer/census_flush_lint.py', lambda ch, s: s['census'], lambda r: ['--root', r], 60, True),
    ('census_reach', 'tools/scene/census_reach_lint.py', lambda ch, s: s['census'], lambda r: ['--root', r], 60, True),
    ('off_census', 'tools/scene/off_census_lint.py', lambda ch, s: s['census'], lambda r: ['--root', r], 60, True),
    ('nrbf_hole', 'tools/scene/nrbf_hole_lint.py', lambda ch, s: 'src/renderer_scene/rf_wrestlers.cpp' in ch.status, lambda r: ['--root', r], 60, True),
    ('fallback_census_binder', 'tools/scene/fallback_census_binder_lint.py', lambda ch, s: 'src/renderer_adapter/binder.h' in ch.status, lambda r: [], 60, False),
]


def check_lints(ch, a):
    srcs = [p for p in ch.status if p.startswith('src/')]
    text = '\n'.join(t for p in srcs for _n, t in ch.added(p) + ch.removed(p))
    start_list = set(x.split('|')[0].strip().replace('\\', '/') for x in (ch.read('tools/scene/start_paths.txt') or '').splitlines() if x.strip() and not x.startswith('#'))
    s = {'src': bool(srcs), 'text': text, 'census': bool(srcs) and bool(CENSUS_WORDS.search(text)),
         'start_files': [p for p in srcs if any(x and (p == x or x.endswith(p) or p.endswith(x)) for x in start_list)]}
    if not RULES['lints']:
        return Result('b.lints', 'SKIP', 'planted: the lint rule is switched off')
    ran, fails, notes, blocked = [], [], [], []
    for name, rel, trig, args_fn, tmo, rooted in LINTS:
        if not trig(ch, s):
            continue
        script = os.path.join(ch.root, rel) if not rooted else tree_script(ch.root, rel)
        if not script or not os.path.isfile(script):
            blocked.append('%s: %s not found' % (name, rel))
            continue
        try:
            v, why, new, nts = run_against_base(ch, name, script, args_fn, tmo, rooted)
        except Blocked as e:
            v, why, new, nts = 'BLOCKED', '%s: %s' % (name, e), [], []
        ran.append(name)
        notes += nts
        if v == 'BLOCKED':
            blocked.append(why)
        elif v == 'FAIL':
            fails.append(why)
            fails += ['%s: %s' % (name, x.strip()[:220]) for x in new[1:6]]
    if fails:
        return Result('b.lints', 'FAIL', fails[0], notes=notes + blocked, details=fails)
    if blocked:
        return Result('b.lints', 'BLOCKED', blocked[0], notes=notes)
    if not ran:
        return Result('b.lints', 'SKIP', 'no switch / build / census lint is triggered by the diff')
    return Result('b.lints', 'PASS', 'ran %s: nothing new against base' % ', '.join(ran), notes=notes)


# ----------------------------------------------------------------------------------------------------------------------------- check c

def check_defaults(ch, a):
    srcs = [p for p in ch.hunks if p.startswith('src/')]
    if not srcs:
        return Result('c.defaults', 'SKIP', 'no src\\ change')
    notes = []
    for p in srcs:
        old_defs = {}
        for _n, t in ch.removed(p):
            m = re.search(r'REXCVAR_DEFINE_(\w+)\s*\(\s*(\w+)\s*,\s*([^,]+),', t)
            if m:
                old_defs[m.group(2)] = m.group(3).strip()
        for n, t in ch.added(p):
            st = t.strip()
            if st.startswith('//'):
                continue
            m = re.search(r'REXCVAR_DEFINE_(\w+)\s*\(\s*(\w+)\s*,\s*([^,]+),', t)
            if m and m.group(1) == 'BOOL' and m.group(3).strip() == 'true' and old_defs.get(m.group(2)) != 'true':
                notes.append('%s:%d: %s defaults to true: every player gets it on' % (p, n, m.group(2)))
            if re.search(r'\b(?:\w+::)*Register\w*\s*\(', st) and not st.startswith(('void ', 'bool ', 'inline ', 'static ')):
                notes.append('%s:%d: a Register call: name whether every player runs it (%s)' % (p, n, st[:120]))
            if p == 'src/bundle3.h' and SIGNIFICANT.search(st):
                notes.append('%s:%d: the player default bundle (promotions) changed: %s' % (p, n, st[:120]))
    return Result('c.defaults', 'PASS', '%d player default(s) to name in the hand-back' % len(notes) if notes else 'nothing new is switched on for every player', notes=notes)


# ----------------------------------------------------------------------------------------------------------------------------- check d

LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')
TAG = re.compile(r'\[[a-z0-9_]+\](?:\[[a-z0-9_.]+\])*')


def check_log_readers(ch, a, deadline):
    changed_tags = {}
    for p in ch.hunks:
        if not p.startswith('src/'):
            continue
        rem, add, at = {}, {}, {}
        for side, rows in ((rem, ch.removed(p)), (add, ch.added(p))):
            for n, t in rows:
                for lit in LIT.findall(t):
                    for tag in TAG.findall(lit):
                        side.setdefault(tag, set()).add(lit)
                        at.setdefault(tag, n)
        for tag in set(rem) | set(add):
            if tag in rem and rem.get(tag, set()) != add.get(tag, set()):   # an existing line whose format changed
                changed_tags.setdefault(tag, '%s:%s' % (p, at[tag]))
    if not changed_tags:
        return Result('d.log_readers', 'SKIP', 'no tagged log / census format string changed')
    notes, fails, ran = [], [], 0
    for tag, where in sorted(changed_tags.items()):
        readers = [x for x in git(ch.root, 'grep', '--untracked', '-l', '-F', tag, '--', 'tools', ok_codes=(0, 1)).splitlines() if x and not x.startswith('tools/handback/')]
        if not readers:
            notes.append('%s changed at %s: no tool under tools\\ names the tag' % (tag, where))
            continue
        tested, untested = [], []
        for r in readers:
            if not r.endswith('.py'):
                continue
            if '--selftest' not in (ch.read(r) or ''):
                continue
            if ran >= 6 or deadline.passed():
                untested.append(r)
                continue
            ran += 1
            rc, out = run_script(os.path.join(ch.root, r), ['--selftest'], ch.root, timeout=45)
            if rc is None:
                raise Blocked('%s --selftest could not run (%s)' % (r, out))
            if rc and RULES['reader_selftest']:
                if r not in ch.status:   # the reader is not yours: is it already red at base, with the SAME failure lines?
                    bt = ch.basetree.get()
                    brc, bout = run_script(os.path.join(bt, r), ['--selftest'], bt, timeout=45)
                    if brc is None or (brc != 0 and CRASH.search(bout)):
                        raise Blocked('%s --selftest fails at head and cannot be judged at base (%s)' % (r, bout if brc is None else 'crashed: ' + (lint_lines(bout) or ['?'])[-1][:120]))
                    if brc:
                        fresh = new_lines([x for x in lint_lines(out) if re.search(r'FAIL|ERROR|error|assert', x)], [x for x in lint_lines(bout) if re.search(r'FAIL|ERROR|error|assert', x)])
                        if not fresh:
                            notes.append('%s --selftest fails at base too, with the same failure lines: not yours' % r)
                            continue
                fails.append('%s changed at %s; its reader %s --selftest FAILS: %s' % (tag, where, r, (lint_lines(out) or ['?'])[-1][:160]))
            else:
                tested.append(r)
        if untested:
            raise Blocked('%d reader selftest(s) of %s not run (the cap / time budget): %s' % (len(untested), tag, ', '.join(untested[:3])))
        notes.append('%s changed at %s: read by %s%s' % (tag, where, ', '.join(readers[:6]) + (' ...' if len(readers) > 6 else ''), ('; selftest passed: ' + ', '.join(tested)) if tested else '; no reader has a selftest'))
    if fails:
        return Result('d.log_readers', 'FAIL', fails[0][:260], notes=notes, details=fails)
    return Result('d.log_readers', 'PASS', '%d changed tag(s); %d reader selftest(s) ran, none newly failed' % (len(changed_tags), ran), notes=notes)


# ----------------------------------------------------------------------------------------------------------------------------- check e

UNIT_RX = re.compile(r'(?:^|/)(?:[\w.-]*_unit|[\w.-]*selftest[\w.-]*|[\w.-]*_test|[\w.-]*_rig|[\w.-]*_emit)\.(?:cpp|py)$')
UNIT_RX = re.compile(r'(?:^|/)(?:[\w.-]*_unit|[\w.-]*selftest[\w.-]*|[\w.-]*_test|[\w.-]*_rig|[\w.-]*_emit)\.(?:cpp|py)$')
ENTRY_RX = re.compile(r'^\s*(?://|#)\s*entry:\s*([~\w:]+)', re.M)
# a call: `ns::F(`, `obj.F(`, `p->F(` or a bare `F(`; any case, 4+ characters (round 3 item 1: CamelCase-only, 8+ characters missed most calls)
CALL_RX = re.compile(r'(?:(?:\b\w+::)+|\.|->|(?<![\w:.>]))\s*([A-Za-z_]\w{3,})\s*\(')
DEF_LINE = re.compile(r'^\s*(?:template\s*<[^>]*>\s*)?(?:(?:inline|static|constexpr|virtual|explicit|extern)\s+)*[\w:<>,\*&\s]+?[\s\*&](?:\w+::)*(~?\w+)\s*\([^;]*$')
KEYWORDS = {'if', 'for', 'while', 'switch', 'catch', 'return', 'sizeof', 'else', 'do', 'namespace', 'struct', 'class', 'union', 'enum', 'alignof', 'decltype',
            'static_assert', 'static_cast', 'reinterpret_cast', 'const_cast', 'dynamic_cast', 'defined'}
HOOK_MACRO = re.compile(r'^(?:REX_HOOK\w*|PPC_FUNC_IMPL|PPC_FUNC|PPC_WEAK_FUNC)$')
MAX_SITES = 40   # a function called from more places than this is a shared helper: not judged by the bypass rule


def is_unit(ch, p):
    if not p.startswith(('tools/', 'src/', 'probes/')) or not p.endswith(('.cpp', '.py')):
        return False
    if UNIT_RX.search(p):
        return True
    if p.startswith('tools/') and p.endswith('.cpp'):   # a rig or test program built against our src (e.g. tools/scene/wrestler_emit.cpp)
        t = ch.read(p) or ''
        return re.search(r'#\s*include\s*"(?:\.\./)+src/', t) is not None and re.search(r'\bint\s+main\s*\(', t) is not None
    return False


def _strip_code(line):
    line = re.sub(r'"(?:[^"\\]|\\.)*"', '""', line)
    line = re.sub(r"'(?:[^'\\]|\\.)*'", "''", line)
    return line.split('//')[0]


def code_only(text, python=False):
    """The text without comments, strings and entry lines (a name in a comment is not a call)."""
    if python:
        out = []
        for ln in text.splitlines():
            if ENTRY_RX.match(ln):
                continue
            ln = re.sub(r'"(?:[^"\\]|\\.)*"|\'(?:[^\'\\]|\\.)*\'', '""', ln)
            out.append(ln.split('#')[0])
        return '\n'.join(out)
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    return '\n'.join(_strip_code(ln) for ln in text.splitlines() if not ENTRY_RX.match(ln))


def enclosing_function(lines, idx):
    """The name of the named function (or hook macro: REX_HOOK_RAW, PPC_FUNC_IMPL) whose body holds line idx (0-based), skipping lambdas and control blocks."""
    depth = 0
    for j in range(idx, max(-1, idx - 600), -1):
        for chx in reversed(_strip_code(lines[j])):
            if chx == '}':
                depth += 1
            elif chx == '{':
                if depth == 0:
                    head = ' '.join(_strip_code(x) for x in lines[max(0, j - 3):j + 1])
                    head = head[:head.rfind('{')]
                    m = re.search(r'(~?\w+)\s*\([^(){};]*(?:\([^()]*\)[^(){};]*)*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?(?:->\s*[\w:<>]+\s*)?$', head)
                    if m and m.group(1) not in KEYWORDS and not re.search(r'\]\s*\([^()]*\)\s*(?:mutable\s*)?(?:->\s*[\w:<>]+\s*)?$', head):
                        return m.group(1)
                else:
                    depth -= 1
    return None


def _sites(root, names, texts=None):
    """{name: {'defs': [(path, line, text)], 'calls': [(path, line, text, enclosing)]}} in src\\ (one git grep)."""
    res = {n: {'defs': [], 'calls': []} for n in names}
    if not names:
        return res
    texts = {} if texts is None else texts
    pat = r'\b(' + '|'.join(sorted(names)) + r')\s*\('
    out = git(root, 'grep', '--untracked', '-n', '-I', '-E', pat, '--', 'src', ok_codes=(0, 1))
    for ln in out.splitlines():
        m = re.match(r'^(.*?):(\d+):(.*)$', ln)
        if not m:
            continue
        path, line, text = m.group(1), int(m.group(2)), m.group(3)
        st = text.strip()
        if st.startswith(('//', '*', '#')):
            continue
        for n in set(re.findall(pat, _strip_code(text))):
            d = DEF_LINE.match(text)
            is_def = d is not None and d.group(1) == n and not st.startswith(('return', 'if', 'else', 'auto ', 'const auto')) and '=' not in text.split(n)[0]
            if is_def:
                res[n]['defs'].append((path, line, st[:140]))
            elif len(res[n]['calls']) <= MAX_SITES:
                if path not in texts:
                    texts[path] = (_read(root, path) or '').splitlines()
                res[n]['calls'].append((path, line, st[:140], enclosing_function(texts[path], line - 1)))
    return res


class HookIndex:
    """What is hook code in the checked tree, from the tree itself (round 3 item 1: not the file name): a body of REX_HOOK* / PPC_FUNC_IMPL, a midasm hook
    function named in config\\*.toml, any function in a file that holds such a macro, or a function whose every production call site is hook code (3 levels)."""

    def __init__(self, root):
        self.root, self.memo, self.texts = root, {}, {}
        self.files = set(git(root, 'grep', '--untracked', '-l', '-E', r'\b(REX_HOOK\w*|PPC_FUNC_IMPL)\s*\(', '--', 'src', ok_codes=(0, 1)).split())
        toml = git(root, 'grep', '--untracked', '-h', '-E', r'^\s*name\s*=\s*"', '--', 'config', ok_codes=(0, 1))
        self.midasm = set(re.findall(r'name\s*=\s*"(\w+)"', toml))

    def site(self, c, depth=0):
        path, _line, _text, encl = c
        if encl and (HOOK_MACRO.match(encl) or encl in self.midasm):
            return True
        if path in self.files:
            return True
        return bool(encl) and depth < 3 and self.only_from_hooks(encl, depth + 1)

    def only_from_hooks(self, name, depth=0):
        if (name, depth) not in self.memo:
            self.memo[(name, depth)] = False   # cycle guard
            calls = _sites(self.root, {name}, self.texts)[name]['calls']
            self.memo[(name, depth)] = bool(calls) and len(calls) <= MAX_SITES and all(self.site(c, depth) for c in calls)
        return self.memo[(name, depth)]


def _file_local(site):
    for path, line, text in site['defs']:
        if text.startswith('static ') or ' static ' in text.split('(')[0]:
            return 'file-local (static) at %s:%d' % (path, line)
    return None


def _calls_in(code, name):
    return re.search(r'(?<![\w])%s\s*\(' % re.escape(name), code) is not None


def check_units(ch, a):
    units = [p for p in ch.files('A', 'M') if is_unit(ch, p)]
    if not units:
        return Result('e.units', 'SKIP', 'no unit, selftest or rig changed')
    fails, notes = [], []
    hooks = None
    for u in units:
        txt = ch.read(u) or ''
        py = u.endswith('.py')
        code = code_only(txt, python=py)
        entries = ENTRY_RX.findall(txt)
        new = ch.status.get(u) == 'A'
        calls = set()
        if not py:
            for _n, t in ch.added(u):   # a new file: every line
                for n in CALL_RX.findall(_strip_code(t)):
                    if n not in KEYWORDS and not n.isupper():
                        calls.add(n)
        entry_names = set(e.split('::')[-1] for e in entries if not py)
        names = calls | entry_names
        sites = _sites(ch.root, names)
        # the evidence rule: an added call of a function whose every production call site is hook code, inside functions the unit never calls
        # (a function the unit names as its entry is judged by the entry rule below instead)
        for n in sorted(calls - entry_names):
            s = sites.get(n)
            if not s or not s['defs'] or not s['calls'] or len(s['calls']) > MAX_SITES or not RULES['unit_bypass']:
                continue
            encl = set(c[3] for c in s['calls'])
            if any(e and _calls_in(code, e) for e in encl):
                continue   # the unit drives a function that calls it
            hooks = hooks or HookIndex(ch.root)
            where = '; '.join('%s:%d in %s' % (c[0], c[1], c[3] or '?') for c in s['calls'][:3])
            if all(hooks.site(c) for c in s['calls']):
                fails.append('%s: calls %s, whose only production call site(s) are hook code the unit never runs (%s): the unit skips the hook\'s own steps' % (u, n, where))
            elif all(c[3] for c in s['calls']):
                notes.append('%s: calls %s directly; production calls it only from %s: name why the unit enters there' % (u, n, where))
        if not entries:
            notes.append('%s: no `%s entry: <symbol>` line (%s unit)' % (u, '#' if py else '//', 'a new' if new else 'an existing'))
            continue
        if not RULES['unit_entry']:
            continue
        for e in entries:
            short = e.split('::')[-1]
            if not _calls_in(code, short):
                fails.append('%s: entry %s is never called by the unit (a mention in a comment is not a call)' % (u, e))
                continue
            if py:
                found = git(ch.root, 'grep', '--untracked', '-l', '-E', r'^\s*def %s\s*\(' % re.escape(short), '--', 'tools', 'src', ok_codes=(0, 1)).strip()
                if not found:
                    fails.append('%s: entry %s is not defined in tools\\ or src\\' % (u, e))
                continue
            s = sites.get(short)
            if not s or not (s['defs'] or s['calls']):
                fails.append('%s: entry %s is not found in src\\' % (u, e))
                continue
            why = _file_local(s)
            if not why and s['calls'] and len(s['calls']) <= MAX_SITES:
                hooks = hooks or HookIndex(ch.root)
                # the entry may be the function a hook body (REX_HOOK*, PPC_FUNC_IMPL, a midasm function) calls directly; it is inner when every call site
                # is hook code under some other function in between (ARM2: SlotOwner under the hook's OnSkelExit)
                direct = any(c[3] and (HOOK_MACRO.match(c[3]) or c[3] in hooks.midasm) for c in s['calls'])
                if not direct:
                    # S105 PLATE review: an entry reached through a binder (RawEntry -> RawHookEntry) whose every call site is directly in a hook
                    # macro is the hook's own entry (the binder has no step of its own); an `// entry: RawEntry` line could not run offline either
                    binders = set(c[3] for c in s['calls'] if c[3])
                    if binders and all(c[3] for c in s['calls']):
                        bsites = _sites(ch.root, binders)
                        direct = all(bsites.get(b) and bsites[b]['calls'] and len(bsites[b]['calls']) <= 2 and
                                     all(bc[3] and (HOOK_MACRO.match(bc[3]) or bc[3] in hooks.midasm) for bc in bsites[b]['calls']) for b in binders)
                        if direct:
                            notes.append('%s: entry %s is reached through binder(s) %s called directly from hook code: accepted' % (u, e, ', '.join(sorted(binders))))
                if not direct and all(hooks.site(c) for c in s['calls']):
                    why = 'an inner function: its only src call site(s) are hook code under %s (%s:%d)' % (s['calls'][0][3] or '?', s['calls'][0][0], s['calls'][0][1])
            if why:
                fails.append('%s: entry %s is %s: drive the hook or the public function that calls it' % (u, e, why))
    if fails:
        return Result('e.units', 'FAIL', fails[0][:300], notes=notes, details=fails)
    return Result('e.units', 'PASS', '%d unit(s) / rig(s) checked' % len(units), notes=notes)



# ----------------------------------------------------------------------------------------------------------------------------- check f

def check_compile(ch, a):
    gs_own = os.path.join(ch.root, 'tools', 'gate', 'game_syntax.py')
    script = gs_own if os.path.isfile(gs_own) and '--changed' in (_read(ch.root, 'tools/gate/game_syntax.py') or '') else os.path.join(REPO, 'tools', 'gate', 'game_syntax.py')
    if not os.path.isfile(script):
        return Result('f.compile', 'BLOCKED', 'tools\\gate\\game_syntax.py not found')
    try:
        r = subprocess.run([sys.executable, '-I', script, '--changed', ch.base, '--build', 'both', '--root', ch.root], capture_output=True, timeout=3600, cwd=ch.root)
    except subprocess.TimeoutExpired:
        return Result('f.compile', 'BLOCKED', 'game_syntax timed out after 3600 s')
    text = (r.stdout + r.stderr).decode('utf-8', 'replace')
    lines = text.splitlines()
    notes = [ln[6:].strip()[:240] for ln in lines if ln.startswith('note: ')]
    notes += [ln.strip()[:240] for ln in lines if ln.startswith(('skipped', 'not compiled by any game build'))]
    blocked_rx = re.compile(r'not found|no build\.ninja|ninja -t commands failed|main checkout with generated|no base for --changed|git diff / ls-files failed')
    if r.returncode == 0 and any(ln.startswith('NOTHING CHECKED') for ln in lines):
        return Result('f.compile', 'SKIP', 'no game unit reaches the change', notes=notes)
    if r.returncode == 0:
        return Result('f.compile', 'PASS', next((ln for ln in lines if ln.startswith('PASS')), 'PASS')[:200], notes=notes)
    if r.returncode == 4:
        return Result('f.compile', 'PASS', 'units compile (NEEDS-MAIN: the main checkout\'s configured builds do not describe this tree; the main session re-runs after merge)', notes=notes + [ln[:240] for ln in lines if 'NEEDS-MAIN' in ln])
    fl = [ln for ln in lines if ln.startswith('FAIL')]
    if CRASH.search(text) and not any(': error:' in ln for ln in lines):
        return Result('f.compile', 'BLOCKED', 'game_syntax crashed: %s' % (lint_lines(text) or ['?'])[-1][:200], notes=notes)
    if fl and all(blocked_rx.search(ln) for ln in fl):
        return Result('f.compile', 'BLOCKED', fl[0][:240], notes=notes)
    errs = [ln.strip() for ln in lines if ': error:' in ln][:8]
    units = [ln[5:].strip() for ln in lines if ln.startswith('FAIL ') and '[' in ln]
    return Result('f.compile', 'FAIL', '%s%s' % (', '.join(units[:4]) or (fl[0] if fl else 'game_syntax exit %d' % r.returncode), ('; ' + errs[0][:200]) if errs else ''), notes=notes, details=units + errs)


# ----------------------------------------------------------------------------------------------------------------------------- driver

ORDER = ['a.deleted', 'a.stale_base', 'a.control_chars', 'a.game_text', 'a.build_lists', 'b.cvar_builds', 'b.lints', 'c.defaults', 'd.log_readers', 'e.units', 'f.compile']


def selected(only):
    if not only:
        return list(ORDER)
    want = [w.strip() for w in only.split(',') if w.strip()]
    match = lambda n, w: n == w or n.split('.')[0] == w or n.split('.')[1] == w
    unknown = [w for w in want if not any(match(n, w) for n in ORDER)]
    if unknown:
        raise SystemExit('unknown check name(s): %s (names: %s)' % (', '.join(unknown), ', '.join(ORDER)))
    return [n for n in ORDER if any(match(n, w) for w in want)]


def run_checks(root, base, a, names, out=print, budget_s=50):
    t_all = time.time()
    try:
        ch = Change(root, base)
    except GitError as e:
        r = Result('a.deleted', 'BLOCKED', 'the diff could not be read: %s' % e)
        out('%-16s %-7s %6.1fs  %s' % (r.name, r.verdict, 0, r.reason))
        return [r], {'exit': 1, 'counts': {'BLOCKED': 1}, 'checks': {r.name: r.verdict}, 'fail': [], 'blocked': [r.name], 'secs_a_e': 0, 'secs_f': 0, 'partial': True, 'notes': 0}
    deadline = Deadline(ch, budget_s)
    results = []
    try:
        for n in ORDER:
            t0 = time.time()
            if n not in names or (n == 'f.compile' and a.skip_compile):
                r = Result(n, 'SKIP', '(by flag) %s' % ('--skip-compile' if n == 'f.compile' and a.skip_compile and n in names else '--only'), by_flag=True)
            else:
                try:
                    if n == 'a.deleted':
                        r = check_deleted(ch, a)
                    elif n == 'a.stale_base':
                        r = check_stale_base(ch, a, deadline)
                    elif n == 'a.control_chars':
                        r = check_control_chars(ch, a)
                    elif n == 'a.game_text':
                        r = check_game_text(ch, a)
                    elif n == 'a.build_lists':
                        r = check_build_lists(ch, a)
                    elif n == 'b.cvar_builds':
                        r = check_cvar_builds(ch, a)
                    elif n == 'b.lints':
                        r = check_lints(ch, a)
                    elif n == 'c.defaults':
                        r = check_defaults(ch, a)
                    elif n == 'd.log_readers':
                        r = check_log_readers(ch, a, deadline)
                    elif n == 'e.units':
                        r = check_units(ch, a)
                    else:
                        r = check_compile(ch, a)
                except Blocked as e:
                    r = Result(n, 'BLOCKED', str(e))
                except GitError as e:
                    r = Result(n, 'BLOCKED', 'git failed: %s' % e)
                except Exception as e:   # a bug in a check is "could not run", never a silent pass
                    r = Result(n, 'BLOCKED', 'the check crashed: %s: %s' % (type(e).__name__, e))
            r.secs = time.time() - t0
            results.append(r)
            out('%-16s %-7s %6.1fs  %s' % (r.name, r.verdict, r.secs, r.reason))
            for nt in r.notes:
                out('    NOTE  %s' % nt)
            for d in r.details[1:12]:
                out('    ..    %s' % d)
    finally:
        why = ch.basetree.close()
        if why:
            out('note: the base tree was not removed: %s' % why)
    ae = sum(r.secs for r in results if not r.name.startswith('f.'))
    f = sum(r.secs for r in results if r.name.startswith('f.'))
    summ = {
        'base': ch.base, 'root': root, 'files': len(ch.status), 'check_version': CHECK_VERSION,
        'exit': 1 if any(r.verdict in ('FAIL', 'BLOCKED') for r in results) else 0,
        'partial': any(r.by_flag for r in results),
        'skipped_by_flag': [r.name for r in results if r.by_flag],
        'counts': {v: sum(1 for r in results if r.verdict == v) for v in VERDICTS},
        'secs_a_e': round(ae, 1), 'secs_f': round(f, 1), 'secs_total': round(time.time() - t_all, 1),
        'checks': {r.name: r.verdict for r in results},
        'fail': [r.name for r in results if r.verdict == 'FAIL'], 'blocked': [r.name for r in results if r.verdict == 'BLOCKED'],
        'notes': sum(len(r.notes) for r in results),
    }
    return results, summ


def main(argv=None):
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(errors='replace')
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--base', default=None)
    ap.add_argument('--json', default=None)
    ap.add_argument('--only', default=None)
    ap.add_argument('--skip-compile', action='store_true')
    ap.add_argument('--allow-delete', nargs='+', default=[])
    ap.add_argument('--root', default='.')
    a = ap.parse_args(argv)
    try:   # round 3 item 8: a folder below the top would read untracked paths relative to itself and walk nothing
        root = os.path.normpath(git(os.path.abspath(a.root), 'rev-parse', '--show-toplevel').strip())
    except GitError as e:
        print('a.deleted        BLOCKED    0.0s  %s is not inside a git tree: %s' % (a.root, e))
        print('HANDBACK ' + json.dumps({'exit': 1, 'blocked': ['root'], 'fail': [], 'partial': True}))
        return 1
    names = selected(a.only)
    for ln in _tree.sweep(root, BASE_DIR):   # base trees of dead runs (238 MB each otherwise)
        print(ln)
    why = ''
    try:
        base = a.base or default_base(root)
    except Exception as e:
        base, why = None, ' (%s)' % e
    if not base:
        print('a.deleted        BLOCKED    0.0s  no base: --base not given and the merge-base with the main checkout\'s branch could not be found%s' % why)
        print('HANDBACK ' + json.dumps({'exit': 1, 'blocked': ['base'], 'fail': [], 'partial': True}))
        return 1
    print('hand-back check %s: %s, base %s' % (CHECK_VERSION, root, base))
    results, summ = run_checks(root, base, a, names)
    c = summ['counts']
    print('total: %d PASS, %d FAIL, %d BLOCKED, %d SKIP (%d by flag), %d NOTE line(s); checks a-e %.1f s, f %.1f s, %s files in the diff -> exit %d%s'
          % (c.get('PASS', 0), c.get('FAIL', 0), c.get('BLOCKED', 0), c.get('SKIP', 0), len(summ.get('skipped_by_flag', [])), summ.get('notes', 0), summ['secs_a_e'],
             summ['secs_f'], summ.get('files', '?'), summ['exit'], ' (PARTIAL: %s left out by flag)' % ', '.join(summ['skipped_by_flag']) if summ.get('partial') else ''))
    if a.json:
        with open(a.json, 'w', encoding='utf-8') as f:
            json.dump({'summary': summ, 'checks': [r.as_dict() for r in results]}, f, indent=1)
    print('HANDBACK ' + json.dumps(summ, sort_keys=True))
    return summ['exit']


if __name__ == '__main__':
    sys.exit(main())
