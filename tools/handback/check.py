#!/usr/bin/env python3
r"""Hand-back check (S108 rewrite of the lane HBK tool; verdict: docs\research\s108\P2_check_py.md): a thin lint runner. A builder runs it before it hands back.

  py -I tools\handback\check.py [--base REV] [--root DIR] [--only NAMES] [--all-rows] [--json FILE] [--list]

The change = BASE -> the working tree of --root (committed work, dirty tree and untracked files). BASE default = the rule of tools\compile\game_syntax.py --changed
(default_base): in a worktree the merge-base of HEAD with the main checkout's branch (the session branch). The changed-file list is `git diff --name-status --no-renames
BASE`: a `git mv` is a deletion of the old path plus an addition of the new one (the diff-reading rows see both paths and the moved text), and neither a move nor an intended
deletion is a finding.

Every row runs on every invocation EXCEPT the gated rows (S111 CHECKFAST, tools\handback\check_gates.py: frame_tick, kind_last, cvar_link, one_definition, cvar_shard,
build_list): a gated row whose own input list holds no file of the diff is SKIP with that reason (the gate is the lint's whole input list, never narrower: S108's false
PASS was a trigger narrower than the lint; `--all-rows` ignores every gate). The rows run side by side (<= 8 at
once), so the whole check costs about the slowest lint. The rows that judge the diff itself (control_chars, game_text, new_src, log_readers) read it as their input and
PASS with a line saying what they looked at when it holds nothing for them. A lint that is red at head is the lane's own: there is no base tree and no "new against base".
  py -I tools\handback\check.py --list      prints the rows and what each reads

Exit 0 = every row passed (--only: the named rows; the output is marked partial); 1 = a FAIL; 2 = no FAIL but a row could not run (BLOCKED, with the reason). One line per row (name, verdict, seconds, reason),
NOTE lines under it, a total, and a last machine-readable line `HANDBACK {json}`. Compile is NOT here: run `py -I tools\compile\game_syntax.py --changed` (the off build, the default since S111 COMPILEOFF).
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
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_gates   # noqa: E402  S111 CHECKFAST: the input list of each gated row
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
    per = {}
    added_anywhere = {}   # tag -> every literal any src file of the diff adds: a literal removed in one file and added unchanged in another MOVED (S110 CVARSHARD: cvar help text to src\cvars), it did not change
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
        per[p] = (rem, add, at)
        for tag, lits in add.items():
            added_anywhere.setdefault(tag, set()).update(lits)
    for p, (rem, add, at) in per.items():
        for tag in set(rem) | set(add):
            if tag in rem and rem.get(tag, set()) != add.get(tag, set()) and rem[tag] - added_anywhere.get(tag, set()):
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


def check_seam(ch):
    """S111 SEAMDIFF: the seam index (db\\seams) rows whose file is in the diff must still hold their anchor (a deleted / rewritten line the index points at is fixed in
    the same change); a row that stores a `line`, or whose anchor is on several lines without an `occurrence`, fails too; rows of files outside the
    diff are not judged. A direct read of the entries: no index build."""
    qdir = os.path.join(REPO, 'tools', 'q')
    if qdir not in sys.path:
        sys.path.append(qdir)
    import qseam_diff
    fails, judged = qseam_diff.judge(ch.root, ch.status)
    if fails:
        return Result('seam', 'FAIL', fails[0][:260], details=fails)
    return Result('seam', 'PASS', '%d seam row(s) point into the diff: none STALE, none AMBIGUOUS' % judged)


def check_codediff(ch):
    """S111 CODEDIFF: the code diff (tools\\q\\qcodediff.py) of the pairs whose OUR function has a fact in a src file of the diff: a DIFF / GAME-ONLY
    that no `// DEPARTS:` marker declares fails, so does a DEPARTS marker the row files refuse. Reads files (q ostate, q pairs, the q gstate index)."""
    qdir = os.path.join(REPO, 'tools', 'q')
    if qdir not in sys.path:
        sys.path.append(qdir)
    import qcodediff
    fails, judged, what = qcodediff.check(ch.root, [p for p, s in ch.status.items() if s != 'D'], ch.base)
    if fails:
        return Result('codediff', 'FAIL', fails[0][:260], details=fails)
    return Result('codediff', 'PASS', '%s: %d row(s), no DIFF / GAME-ONLY without a DEPARTS marker' % (what, judged))


def check_cdf(name):
    """S111 CDCHECKALL: the row of one code-diff extractor (tools\\handback\\check_cdfields.py): a NEW DIFF / GAME-ONLY (draworder: DIFF) the base tree does not hold fails;
    one the base already holds does not. Reads files (the q index for the game side)."""
    def run(ch):
        import check_cdfields
        fails, judged, what = check_cdfields.check(name, ch.base)
        if fails:
            return Result('cd_' + name, 'FAIL', fails[0][:260], details=fails)
        return Result('cd_' + name, 'PASS', what)
    return run


def check_marks(ch):
    """S111 QMARKS: the trend of the marks (claims, seam game rows, open items) of THIS tree's db\\; never fails (a read error is a PASS that says so)."""
    qdir = os.path.join(REPO, 'tools', 'q')
    if qdir not in sys.path:
        sys.path.append(qdir)
    try:
        import qmarks
        return Result('marks', 'PASS', qmarks.totals_line(qmarks.totals(qmarks.collect(ch.root))).replace('marks: ', '', 1))
    except Exception as e:   # a trend row: never a FAIL
        return Result('marks', 'PASS', 'not counted: %s: %s' % (type(e).__name__, e))


# ----------------------------------------------------------------------------------------------------------------------------- the rows

class Row:
    def __init__(self, name, reads, run):
        self.name, self.reads, self.run = name, reads, run
        self.script = None   # a lint row: the script's path in the repo


def lint_row(name, rel, reads, rooted=True, timeout=120, with_base=False):
    """A lint script run as `py -I SCRIPT --root ROOT` (rooted=False: no arguments, cwd = the tree), on every invocation. Exit 0 = PASS; exit 2, a crash, a usage message,
    a timeout or a missing script = BLOCKED; any other exit = FAIL (its lines). `reads` is the one line `--list` prints (what the lint looks at)."""
    def run(ch):
        script = tree_script(ch.root, rel) if rooted else os.path.join(ch.root, rel)
        if not script or not os.path.isfile(script):
            raise Blocked('%s not found' % rel)
        rc, text = run_script(script, (['--root', ch.root] if rooted else []) + (['--base', ch.base] if with_base else []), ch.root, timeout)   # with_base: S111 WRITERLINT, the lint reads the diff
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
    lint_row('fallback_census_binder', 'tools/scene/fallback_census_binder_lint.py', 'src\\renderer_adapter\\binder.h (read with its parts, src\\renderer_adapter\\binder\\*.inc)', rooted=False),
    lint_row('build_list', 'tools/lint/build_list_lint.py', 'src\\ and tools\\ sources and scripts against the build lists, CMakeLists.txt'),
    lint_row('cvar_link', 'tools/scene/cvar_link_lint.py', 'every game unit of both builds through clang -E (cache: main checkout out\\cvar_pp), the build lists, tools build*.ps1 / .bat', timeout=600),
    lint_row('one_definition', 'tools/lint/one_definition_lint.py', 'every game unit of both builds through clang -E (the cvar_pp cache): each hooked guest function defined by one unit; src\\ headers: no namespace-scope mutable static / static function / anonymous namespace', timeout=600),
    lint_row('cvar_shard', 'tools/lint/cvar_shard_lint.py', 'every src\\ header (no REXCVAR_DEFINE_*: a new cvar is moved by tools\\compile\\cvar_shard.py), src\\cvars\\cvars_<n>.cpp against what the tool writes'),   # S110 CVARSHARD
    # the former orphans (no runner before S108)
    lint_row('aniso', 'tools/lint/aniso_lint.py', 'src\\renderer_scene\\'),
    lint_row('mask', 'tools/lint/mask_lint.py', 'src\\renderer_scene\\'),
    lint_row('t19', 'tools/lint/t19_lint.py', 'tools\\ .py, case_scene.cpp, record_io.*, renderer_adapter\\live.cpp, repo-root *.tmp'),
    lint_row('junction', 'tools/lint/junction_lint.py', 'the tools\\ tree'),
    lint_row('cache_state', 'tools/lint/cache_state_lint.py', 'src\\renderer_scene\\'),
    lint_row('fview_csv', 'tools/lint/fview_csv_lint.py', 'tools\\ .py'),
    lint_row('light_stamp', 'tools/lint/light_stamp_lint.py', 'src\\renderer_scene\\'),
    lint_row('plant_number', 'tools/lint/plant_number_lint.py', 'tools\\scene\\guest_model_unit.cpp plant dispatch and guest_model_unit.py PLANTS: each twin number and each product plant value used once'),   # S111 LAYERS review
    lint_row('flag_bit', 'tools/lint/flag_bit_lint.py', 'src\\ every enum { kX = N } body and every constexpr kXyz = N prefix group: no two members share a power-of-two bit'),   # S111 FLAGLINT
    lint_row('kind_last', 'tools/lint/kind_last_lint.py', 'src\\ every constexpr kXxLast = kName (a record row family\'s last known kind) against the enum that holds kName: it must be the highest value'),   # S111 FXSKIP review
    lint_row('shader_flip', 'tools/lint/shader_flip_lint.py', 'src\\renderer\\shaders\\*.vert (our own vertex shaders, compile_plain\'s set): each gl_Position write applies ndc_scale (the renderer flip) or the file carries `// FLIP-EXEMPT: <why>` (planted twins run each time)'),   # S111 SHADERFLIP
    lint_row('hook_original', 'tools/lint/hook_original_lint.py', 'src\\ every REX_HOOK_RAW(sub_X) body: __imp__sub_X once on each path, or a return before it with `// SKIP-ORIGINAL: <why>`, two calls with `// TWICE-ORIGINAL: <why>`, the call inside a helper with `// ORIGINAL-IN: <Helper>` (a brace-depth path walk; no baseline; planted twins run each time)', timeout=300),   # S111 CDLINTS
    lint_row('alpha_source', 'tools/lint/alpha_source_lint.py', 'src\\renderer_scene\\ every ProgramFor( call / `x->alpha_test =` that reads Material::alpha_test alone: a pass_state row, InstancedDraw::alpha_test, a game constant or `// ALPHA-SOURCE: <why>` is needed (known rows in tools\\lint\\alpha_source_known.txt; planted twins run each time)'),   # S111 CDLINTS
    lint_row('frame_tick', 'tools/renderer/frame_tick_unit.py', 'src\\renderer_game\\frame_tick.h (built and run with its planted twins), the wiring of reader.h / half2_hooks.h / tail_release_hook.h; its planted-twin runs print FAIL lines by design', timeout=300),   # S111 FRAMETICK
    lint_row('own_catalog','tools/renderer/own_catalog_lint.py', 'src\\ program blobs (vertex_spirv / fragment_spirv, ProgramDesc brace lists), fx_own_shaders.cpp kOwnShaders, pipeline_catalog.h and its three callers'),   # S110 RDRPREP
    lint_row('pipeline_seed', 'tools/renderer/pseed_stale_check.py', 'db\\shaders\\shader_list.txt R lines against the catalog of this tree\'s compiled SPIR-V (tools\\renderer\\pseed_catalog.cpp + src\\renderer\\shaders\\spv\\fx_spv_data.cpp + fx_own_shaders.cpp, built with clang): a record whose shader changed since the seed was made FAILS (fix: tools\\renderer\\pseed_rekey.py, 0 game runs)', timeout=300),   # S111 PSEEDSTALE
    lint_row('spv_split', 'tools/renderer/spv_split_lint.py', 'src\\renderer\\shaders\\spv: no header holds a word array, each array defined once, fx_spv_data.cpp includes every effect with a current sha and no own shader, fx_own_shaders.cpp holds kOwnShaders with current sha lines, fx_registry.h names no kFx_ outside sizeof( (the words only through rdr::fx::Words), no tool build script lists fx_spv_data.cpp for sources that never call Words (spv_readers.py; its planted twins run each time)', timeout=300),   # S111 SPVSPLIT, S111 TOOLLINK, S111 TOOLLINK-D
    lint_row('cite_line','tools/lint/cite_line_lint.py', 'the live docs (docs\\systems, formats, renderer, ours, STATUS, PLAYBOOK, LESSONS, IDEAS, .claude\\agents) and every src\\ / tools\\ comment: no `<our file>:N` line citation (cite the file and the NAME); exceptions in tools\\lint\\cite_line_allow.txt', timeout=300),   # S111 CITENAME
    lint_row('caster_loop', 'tools/lint/caster_loop_lint.py', 'src\\ every CasterMeshCasts( / SkinChunkAbsent( call: the wrestler caster loops are the known set and each applies the piece rule (planted twins run each time)'),   # S111 TOOLFIX3
    lint_row('char_class_key', 'tools/lint/char_class_key_lint.py', 'src\\renderer_scene\\front_end.cpp FrontEnd::Record: every IsCharEffect( call that feeds an order / rank / turn key (SeeKey::character, SeeCharacter, BodyObject / TurnOf, one hop through a local or member) is a test plant\'s input only and the key goes through the class (CharSlotOf / Item::char_slot); shader / variant uses allowed (planted twins run each time)'),   # S111 SEECHAR review
    lint_row('w2b', 'tools/scene/w2b_lint.py', 'the BodyRowCount( and SkinChunkAbsent( uses of the palette readers and the caster loops (counts)', rooted=False),   # S111 TOOLFIX3
    lint_row('lodge','tools/handback/lodge_lint.py', 'git-tracked docs\\research\\ notes with a ```claims block against their db\\claims\\ files (a missing file, or one edited by hand: the note is the only source; prints both sides\' last commit and the commands)'),   # S111 INDEX_PLAN lane B
    lint_row('writers_claim', 'tools/lint/writers_claim_lint.py', 'the docs\\research\\ notes of the diff with a ```claims block: a row that says only writer / flips / alternates about a +N field must name every writer `q field N --global ADDR --writes` lists, or be marked [L] (planted twins run each time; --all sweeps)', with_base=True),   # S111 WRITERLINT
]

ROWS = [
    Row('control_chars', 'added lines of added / modified text files (the diff)', check_control_chars),
    Row('game_text', 'added lines of published files (the diff)', check_game_text),
    Row('new_src', 'added / renamed .cpp files (the diff) against the build lists', check_new_src),
    Row('log_readers', "changed [tag] literals of src\\ (the diff); runs the readers' --selftest", check_log_readers),
    Row('seam', 'db\\seams rows whose file is in the diff: STALE / AMBIGUOUS anchor / a stored line = FAIL (the diff)', check_seam),
    Row('codediff', 'src files of the diff: q codediff of their functions\' pairs (a DIFF / GAME-ONLY without `// DEPARTS:` = FAIL), their DEPARTS markers; a db\\claims / db\\seams edit: the whole tree\'s new DIFF / GAME-ONLY against the base (the diff)', check_codediff),   # S111 CODEDIFF
    Row('cd_levels', 'q codediff --field levels (qlevels.py): a NEW DIFF / GAME-ONLY against the base fails; gate: any src code file, the q tools it uses (the diff)', check_cdf('levels')),   # S111 CDCHECKALL
    Row('cd_targetfmt', 'q targetfmt (qtargetfmt.py): a NEW DIFF / GAME-ONLY against the base fails; gate: the role files of ours, src\\renderer\\shaders\\fx, the q tools it uses (the diff)', check_cdf('targetfmt')),
    Row('cd_draworder', 'q draworder (qdraworder.py): a NEW DIFF against the base fails; gate: front_end.cpp, body_order.h, see_through_order.h, the record classes, frame_hooks, render_categories.md (the diff)', check_cdf('draworder')),
    Row('marks', 'db\\claims + db\\seams: the C / L / U totals and open items (a trend, never fails)', check_marks),
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


def run_checks(root, base, only=None, out=print, all_rows=False):
    """(results in ROWS order, summary dict). Every row (or the --only rows) runs, in a pool of at most MAX_JOBS; a gated row with no input in the diff is SKIP
    (check_gates; not with --only naming it, nor all_rows)."""
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
            continue
        go, why = (True, '') if (want or all_rows) else check_gates.decide(ch, row.name)
        if go:
            todo.append(row)
        else:
            skipped[row.name] = Result(row.name, 'SKIP', why)
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
    out('%-24s %s' % ('row', 'reads (a row marked * is gated: SKIP when the diff holds none of its inputs, check_gates.py)'))
    for row in ROWS:
        out('%-22s%s %s' % (row.name, ' *' if row.name in check_gates.GATES else '  ', row.reads))


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
    ap.add_argument('--all-rows', action='store_true', help='run every row, gated ones too (the forced full run)')
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
        results, summ = run_checks(root, base, a.only, all_rows=a.all_rows)
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
