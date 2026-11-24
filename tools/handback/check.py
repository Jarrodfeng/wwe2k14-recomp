#!/usr/bin/env python3
r"""Hand-back check (S108 rewrite of the lane HBK tool; verdict: docs\research\s108\P2_check_py.md): a thin lint runner. A builder runs it before it hands back.

  py -I tools\handback\check.py [--base REV] [--root DIR] [--only NAMES] [--json FILE] [--list]

The change = BASE -> the working tree of --root (committed work, dirty tree and untracked files). BASE default = the rule of tools\compile\game_syntax.py --changed
(default_base): in a worktree the merge-base of HEAD with the main checkout's branch (the session branch). The changed-file list is `git diff --name-status --no-renames
BASE`: a `git mv` is a deletion of the old path plus an addition of the new one (the diff-reading rows see both paths and the moved text), and neither a move nor an intended
deletion is a finding.

EVERY row runs on every invocation (S108: no per-row path triggers; a trigger narrower than what its lint reads was a false PASS). The rows run side by side (<= 8 at
once), so the whole check costs about the slowest lint. The rows that judge the diff itself (control_chars, game_text, new_src, log_readers) read it as their input and
PASS with a line saying what they looked at when it holds nothing for them. A lint that is red at head is the lane's own: there is no base tree and no "new against base".
  py -I tools\handback\check.py --list      prints the rows and what each reads

Exit 0 = every row passed (--only: the named rows; the output is marked partial); 1 = a FAIL; 2 = no FAIL but a row could not run (BLOCKED, with the reason). One line per row (name, verdict, seconds, reason),
NOTE lines under it, a total, and a last machine-readable line `HANDBACK {json}`. Compile is NOT here: run `py -I tools\compile\game_syntax.py --changed --build both`.
To add a lint: one `lint_row(...)` line in LINT_ROWS (docs\agents\handback_checks.md). 0 game runs."""
import argparse
import json
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tools', 'compile'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'lint'))

T_GIT = 120
MAX_JOBS = 8
VERDICTS = ('PASS', 'FAIL', 'BLOCKED', 'SKIP')


# ----------------------------------------------------------------------------------------------------------------------------- git / diff

class GitError(RuntimeError):
    pass


def git(root, *args, ok_codes=(0,), timeout=T_GIT):
    """stdout of one git command; raises GitError on another exit code (a failed git call is never "no output")."""
    try:
        r = subprocess.run(['git', '-C', root] + list(args), capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise GitError('git %s: timed out after %d s' % (' '.join(args[:3]), timeout))
    if r.returncode not in ok_codes:
        raise GitError('git %s: exit %d: %s' % (' '.join(args[:3]), r.returncode, r.stderr.decode('utf-8', 'replace').strip()[:300]))
    return r.stdout.decode('utf-8', 'replace')


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
    """The change BASE -> working tree of root. status {path: 'A' | 'M' | 'D' | 'T'}. `--no-renames`: a `git mv` is a 'D' of the old path and an 'A' of the new one with
    every line, because the lints read the tree: with -M (S108 review) a moved file hid its old path and its text from every trigger (a getenv baseline keyed by path, a
    lint that reads src\\renderer_adapter\\binder.h by name) and those rows SKIPped on a diff their lint fails."""

    def __init__(self, root, base):
        self.root = root
        self.base = git(root, 'rev-parse', '--verify', base + '^{commit}').strip()
        self.status = {}
        for ln in git(root, 'diff', '--name-status', '--no-renames', self.base).splitlines():
            parts = ln.split('\t')
            if len(parts) >= 2:
                self.status[parts[-1].replace('\\', '/')] = parts[0][0]
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


def default_base(root):
    """game_syntax --changed's rule (tools\\compile\\game_syntax.py default_base), imported so the two cannot drift."""
    import game_syntax as gs
    out = git(root, 'rev-parse', '--git-common-dir').strip()
    return gs.default_base(root, os.path.dirname(os.path.abspath(os.path.join(root, out))))


# ----------------------------------------------------------------------------------------------------------------------------- results

class Blocked(RuntimeError):
    """The row could not run: its verdict is BLOCKED."""


class Result:
    def __init__(self, name, verdict, reason, notes=None, details=None):
        assert verdict in VERDICTS, verdict
        self.name, self.verdict, self.reason = name, verdict, reason
        self.notes, self.details = list(notes or []), list(details or [])
        self.secs = 0.0

    def as_dict(self):
        return {'name': self.name, 'verdict': self.verdict, 'secs': round(self.secs, 2), 'reason': self.reason, 'notes': self.notes, 'details': self.details}


CRASH = re.compile(r'Traceback \(most recent call last\)|^\w*Error: |Exception:', re.M)
COULD_NOT_RUN = re.compile(r'^usage: |unrecognized arguments', re.M)   # (a `BLOCKED` line is not here: cvar_link_lint prints one per unit it could not preprocess
# next to its real findings and then exits 1; its "only blocked" case is exit 2. A finding is never downgraded to BLOCKED by a line beside it.)


def run_script(script, args, cwd, timeout=120):
    """(rc, text). rc None = could not run (timeout / missing)."""
    if not os.path.isfile(script):
        return None, 'missing %s' % script
    try:
        r = subprocess.run([sys.executable, '-I', script] + list(args), capture_output=True, timeout=timeout, cwd=cwd)
    except subprocess.TimeoutExpired:
        return None, '%s timed out after %d s' % (os.path.basename(script), timeout)
    return r.returncode, (r.stdout + r.stderr).decode('utf-8', 'replace')


def lines_of(text):
    return [ln.rstrip() for ln in text.splitlines() if ln.strip()]


def tree_script(root, rel):
    """The tree's own copy of a script, else this checkout's."""
    for base in (root, REPO):
        p = os.path.join(base, rel)
        if os.path.isfile(p):
            return p
    return None


# ----------------------------------------------------------------------------------------------------------------------------- the in-process rows

def check_control_chars(ch):
    """A control character on an ADDED line (tools\\lint\\no_control_chars.py's scanner), or a CR not at the line's end (the keep_list K44 break): both are invisible in a diff."""
    import no_control_chars as ncc
    hits, n = [], 0
    for p in ch.files('A', 'M', 'R'):
        if os.path.splitext(p)[1].lower() not in ncc.EXT:
            continue
        data = ch.read_bytes(p)
        if data is None:
            continue
        n += 1
        added = set(ln for ln, _t in ch.added(p))
        for line, byte, ctx in ncc.scan_bytes(data):
            if line in added:
                hits.append('%s:%d: 0x%02X near "%s"' % (p, line, byte, ctx))
        for i, raw in enumerate(data.split(b'\n'), 1):   # no_control_chars allows CR (CRLF files); a CR that does not end its line splits it for every reader
            if i in added and b'\r' in raw.rstrip(b'\r'):
                hits.append('%s:%d: a lone CR inside the line near "%s"' % (p, i, raw[:raw.index(b'\r')][-20:].decode('utf-8', 'replace')))
    if hits:
        return Result('control_chars', 'FAIL', '%d control character(s) on added lines: %s' % (len(hits), hits[0]), details=hits)
    return Result('control_chars', 'PASS', '%d changed text file(s), no control character on an added line' % n)


def check_game_text(ch):
    """Rule 1: game text (the player's string.pac / CSDT) on an added line of a published file."""
    try:
        if 'game_text_lint' not in sys.modules:
            sys.path.insert(0, os.path.join(REPO, 'tools', 'harness'))
        import game_text_lint as gtl
        idx = gtl.Index(gtl.load_set())
    except Exception as e:
        raise Blocked('tools/harness/game_text_lint.py cannot run (game data missing?): %s' % e)
    allow = gtl.load_allow()
    hits, n = [], 0
    for p in ch.files('A', 'M', 'R'):
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
    if hits:
        return Result('game_text', 'FAIL', "%d line(s) of the game's own text (rule 1): %s" % (len(hits), hits[0]), details=hits)
    return Result('game_text', 'PASS', '%d changed published file(s), no game text on an added line' % n)


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


def check_new_src(ch):
    """A NEW src\\ .cpp that no build list names (nothing compiles it: game_syntax only prints "not compiled by any game build"), or that CMakeLists.txt lacks while its
    siblings are game units."""
    lists = _real_lists(ch)
    cm = lists.get('CMakeLists.txt', '')
    off = lists.get('tools/scene/keep_stream_off_sources.txt', '')
    fails, notes = [], []
    new = ch.files('A', 'R')
    for p in new:
        if not p.endswith('.cpp') or not p.startswith('src/'):
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
    for p in new:
        if p.endswith('.cpp') and p.startswith('tools/') and not any(os.path.basename(p) in t for rel, t in lists.items() if rel.startswith('tools/')):
            notes.append('%s: no tools build script names it yet' % p)
    if fails:
        return Result('new_src', 'FAIL', fails[0][:240], notes=notes, details=fails)
    return Result('new_src', 'PASS', '%d added / renamed src .cpp checked against %d build list(s)' % (len([p for p in new if p.endswith('.cpp') and p.startswith('src/')]), len(lists)), notes=notes)


LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')
TAG = re.compile(r'\[[a-z0-9_]+\](?:\[[a-z0-9_.]+\])*')


def changed_tags(ch):
    """{tag: 'file:line'} of the `[tag]` log / census literals whose format string changed (an existing line whose text moved)."""
    out = {}
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
            if tag in rem and rem.get(tag, set()) != add.get(tag, set()):
                out.setdefault(tag, '%s:%s' % (p, at[tag]))
    return out


def check_log_readers(ch):
    """A log / census line whose format string changed: every reader under tools\\ that names its tag and has --selftest runs it; a failing one is a FAIL (the reader is not
    in the diff and its proof is a run, e.g. feed_census_check.py lints the src literals)."""
    tags = changed_tags(ch)
    notes, fails, ran = [], [], 0
    for tag, where in sorted(tags.items()):
        readers = [x for x in git(ch.root, 'grep', '--untracked', '-l', '-F', tag, '--', 'tools', ok_codes=(0, 1)).splitlines() if x and not x.startswith('tools/handback/')]
        if not readers:
            notes.append('%s changed at %s: no tool under tools\\ names the tag' % (tag, where))
            continue
        tested = []
        for r in readers:
            if not r.endswith('.py') or '--selftest' not in (ch.read(r) or ''):
                continue
            ran += 1
            rc, out = run_script(os.path.join(ch.root, r), ['--selftest'], ch.root, timeout=120)
            if rc is None:
                raise Blocked('%s --selftest could not run (%s)' % (r, out))
            if rc:
                fails.append('%s changed at %s; its reader %s --selftest FAILS: %s' % (tag, where, r, (lines_of(out) or ['?'])[-1][:160]))
            else:
                tested.append(r)
        notes.append('%s changed at %s: read by %s%s' % (tag, where, ', '.join(readers[:6]) + (' ...' if len(readers) > 6 else ''),
                                                         ('; selftest passed: ' + ', '.join(tested)) if tested else '; no reader has a selftest'))
    if fails:
        return Result('log_readers', 'FAIL', fails[0][:260], notes=notes, details=fails)
    return Result('log_readers', 'PASS', '%d changed [tag] literal(s) in src\\; %d reader selftest(s) ran' % (len(tags), ran), notes=notes)


# ----------------------------------------------------------------------------------------------------------------------------- the rows

class Row:
    def __init__(self, name, reads, run):
        self.name, self.reads, self.run = name, reads, run
        self.script = None   # a lint row: the script's path in the repo


def lint_row(name, rel, reads, rooted=True, timeout=120):
    """A lint script run as `py -I SCRIPT --root ROOT` (rooted=False: no arguments, cwd = the tree), on every invocation. Exit 0 = PASS; exit 2, a crash, a usage message,
    a timeout or a missing script = BLOCKED; any other exit = FAIL (its lines). `reads` is the one line `--list` prints (what the lint looks at)."""
    def run(ch):
        script = tree_script(ch.root, rel) if rooted else os.path.join(ch.root, rel)
        if not script or not os.path.isfile(script):
            raise Blocked('%s not found' % rel)
        rc, text = run_script(script, ['--root', ch.root] if rooted else [], ch.root, timeout)
        if rc is None:
            raise Blocked(text)
        if rc == 75:   # S109 GATE: the compile queue refused the lint's pool (tools\admit: not admitted within its bound): BLOCKED with the reason, not a finding
            why = next((x for x in lines_of(text) if 'REFUSED' in x), (lines_of(text) or ['exit 75'])[-1])
            raise Blocked('compile queue refused: %s' % why.strip()[:300])
        if rc != 0 and CRASH.search(text):
            raise Blocked('crashed: %s' % (lines_of(text) or ['?'])[-1][:200])
        if rc != 0 and (rc == 2 or COULD_NOT_RUN.search(text)):
            raise Blocked('could not run (exit %d): %s' % (rc, next((x for x in lines_of(text) if COULD_NOT_RUN.search(x)), (lines_of(text) or ['?'])[0])[:200]))
        if rc:
            shown = [x for x in lines_of(text) if not re.match(r'^\s*(?:PASS|ok)\b', x)] or lines_of(text)
            return Result(name, 'FAIL', (shown or ['exit %d' % rc])[0].strip()[:240], details=[x.strip()[:240] for x in shown[:12]])
        return Result(name, 'PASS', 'exit 0')
    row = Row(name, reads, run)
    row.script = rel
    return row


LINT_ROWS = [
    lint_row('dev_cvar', 'tools/scene/dev_cvar_lint.py', 'every src\\ file: dev cvar definitions'),
    lint_row('getenv', 'tools/scene/getenv_lint.py', 'every src\\ file: getenv calls against getenv_baseline.txt'),
    lint_row('rule_register', 'tools/renderer/rule_register_lint.py', 'src\\ SHADER-INPUT lines, src\\renderer\\shaders\\fx\\*.fx.json'),
    lint_row('switch_defined', 'tools/scene/switch_defined_lint.py', 'every src\\ file, keep_stream_sources.txt'),
    lint_row('version_define', 'tools/lint/version_define_lint.py', 'every src\\ file (WWE2K14_VERSION uses, includes), CMakeLists.txt'),
    lint_row('render_clock', 'tools/lint/render_clock_lint.py', 'the render folders of src\\ (clock use)'),
    lint_row('start_parity', 'tools/scene/start_parity_lint.py', 'every src\\ file, start_paths.txt and its rig globs, the keep_stream lists, CMakeLists.txt'),
    lint_row('register_switch', 'tools/scene/register_switch_lint.py', 'every src .h / .cpp, register_switch_allow.txt'),
    lint_row('census_flush', 'tools/renderer/census_flush_lint.py', 'the census / log lines of src\\'),
    lint_row('census_reach', 'tools/scene/census_reach_lint.py', 'the census lines of src\\, scene_host.cpp, host_start.cpp'),
    lint_row('off_census', 'tools/scene/off_census_lint.py', 'live.cpp, scene_host.cpp, live_scene.cpp, scene_skip.h'),
    lint_row('nrbf_hole', 'tools/scene/nrbf_hole_lint.py', 'src\\renderer_scene\\rf_*.cpp'),
    lint_row('fallback_census_binder', 'tools/scene/fallback_census_binder_lint.py', 'src\\renderer_adapter\\binder.h', rooted=False),
    lint_row('build_list', 'tools/lint/build_list_lint.py', 'src\\ and tools\\ sources and scripts against the build lists, CMakeLists.txt'),
    lint_row('cvar_link', 'tools/scene/cvar_link_lint.py', 'every game unit of both builds through clang -E (cache: main checkout out\\cvar_pp), the build lists, tools build*.ps1 / .bat', timeout=600),
    lint_row('one_definition', 'tools/lint/one_definition_lint.py', 'every game unit of both builds through clang -E (the cvar_pp cache): each hooked guest function defined by one unit; src\\ headers: no namespace-scope mutable static / static function / anonymous namespace', timeout=600),
    # the former orphans (no runner before S108)
    lint_row('aniso', 'tools/lint/aniso_lint.py', 'src\\renderer_scene\\'),
    lint_row('mask', 'tools/lint/mask_lint.py', 'src\\renderer_scene\\'),
    lint_row('t19', 'tools/lint/t19_lint.py', 'tools\\ .py, case_scene.cpp, record_io.*, renderer_adapter\\live.cpp, repo-root *.tmp'),
    lint_row('junction', 'tools/lint/junction_lint.py', 'the tools\\ tree'),
    lint_row('cache_state', 'tools/lint/cache_state_lint.py', 'src\\renderer_scene\\'),
    lint_row('fview_csv', 'tools/lint/fview_csv_lint.py', 'tools\\ .py'),
    lint_row('light_stamp', 'tools/lint/light_stamp_lint.py', 'src\\renderer_scene\\'),
    lint_row('own_catalog', 'tools/renderer/own_catalog_lint.py', 'src\\ program blobs (vertex_spirv / fragment_spirv, ProgramDesc brace lists), fx_spv_data.cpp kOwnShaders, pipeline_catalog.h and its three callers'),   # S110 RDRPREP
]

ROWS = [
    Row('control_chars', 'added lines of added / modified text files (the diff)', check_control_chars),
    Row('game_text', 'added lines of published files (the diff)', check_game_text),
    Row('new_src', 'added / renamed .cpp files (the diff) against the build lists', check_new_src),
    Row('log_readers', "changed [tag] literals of src\\ (the diff); runs the readers' --selftest", check_log_readers),
] + LINT_ROWS


# ----------------------------------------------------------------------------------------------------------------------------- driver

def _run_row(ch, row):
    t0 = time.time()
    try:
        r = row.run(ch)
    except Blocked as e:
        r = Result(row.name, 'BLOCKED', str(e))
    except GitError as e:
        r = Result(row.name, 'BLOCKED', 'git failed: %s' % e)
    except (Exception, SystemExit) as e:   # a bug in a row is "could not run", never a silent pass; SystemExit too (game_text_lint.load_allow raises it on a bad allow
        r = Result(row.name, 'BLOCKED', 'the row crashed: %s: %s' % (type(e).__name__, e))   # row): uncaught, it left the pool and ended the run with no summary
    r.secs = time.time() - t0
    return r


def run_checks(root, base, only=None, out=print):
    """(results in ROWS order, summary dict). Every row (or the --only rows) runs, in a pool of at most MAX_JOBS."""
    t_all = time.time()
    ch = Change(root, base)
    want = [w.strip() for w in (only or '').split(',') if w.strip()]
    unknown = [w for w in want if not any(r.name == w for r in ROWS)]
    if unknown:
        raise SystemExit('unknown row name(s): %s (rows: %s)' % (', '.join(unknown), ', '.join(r.name for r in ROWS)))
    todo, skipped = [], {}
    for row in ROWS:
        if want and row.name not in want:
            skipped[row.name] = Result(row.name, 'SKIP', '(by --only)')
        else:
            todo.append(row)
    with ThreadPoolExecutor(max_workers=max(1, min(MAX_JOBS, len(todo)))) as ex:
        ran = dict(zip([r.name for r in todo], ex.map(lambda r: _run_row(ch, r), todo)))
    results = []
    for row in ROWS:
        r = ran.get(row.name) or skipped[row.name]
        results.append(r)
        out('%-22s %-7s %6.1fs  %s' % (r.name, r.verdict, r.secs, r.reason))
        for nt in r.notes:
            out('    NOTE  %s' % nt)
        for d in r.details[1:12]:
            out('    ..    %s' % d)
    counts = {v: sum(1 for r in results if r.verdict == v) for v in VERDICTS}
    summ = {'base': ch.base, 'root': root, 'files': len(ch.status), 'counts': counts, 'secs_total': round(time.time() - t_all, 1), 'partial': bool(want),   # --only: a quoted HANDBACK line says so
            'exit': 1 if counts['FAIL'] else (2 if counts['BLOCKED'] else 0),
            'fail': [r.name for r in results if r.verdict == 'FAIL'], 'blocked': [r.name for r in results if r.verdict == 'BLOCKED'],
            'rows': {r.name: r.verdict for r in results}}
    return results, summ


def print_list(out=print):
    out('%-24s %s' % ('row', 'reads (every row runs on every invocation)'))
    for row in ROWS:
        out('%-24s %s' % (row.name, row.reads))


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
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--root', default='.')
    a = ap.parse_args(argv)
    if a.list:
        print_list()
        return 0
    try:
        root = os.path.normpath(git(os.path.abspath(a.root), 'rev-parse', '--show-toplevel').strip())
        base = a.base or default_base(root)
        if not base:
            raise GitError('--base not given and the merge-base with the main checkout\'s branch could not be found')
        print('hand-back check: %s, base %s%s' % (root, base, ('  [PARTIAL: only %s]' % a.only) if a.only else ''))
        results, summ = run_checks(root, base, a.only)
    except GitError as e:
        print('BLOCKED: the diff could not be read: %s' % e)
        print('HANDBACK ' + json.dumps({'exit': 2, 'blocked': ['diff'], 'fail': []}))
        return 2
    c = summ['counts']
    print('total: %d PASS, %d FAIL, %d BLOCKED, %d SKIP, %s files in the diff, %.1f s -> exit %d'
          % (c['PASS'], c['FAIL'], c['BLOCKED'], c['SKIP'], summ['files'], summ['secs_total'], summ['exit']))
    if a.json:
        with open(a.json, 'w', encoding='utf-8') as f:
            json.dump({'summary': summ, 'checks': [r.as_dict() for r in results]}, f, indent=1)
    print('HANDBACK ' + json.dumps(summ, sort_keys=True))
    return summ['exit']


if __name__ == '__main__':
    sys.exit(main())
