"""S102 TIDY2 (Jarrod, 2026-11-15 11:2x: "out should be scratch as it clearly seems to be ... be smart about it"): the pure rules of the
new out\\ clean-up. No import of tidy, nothing here deletes. tools\\harness\\tidy.py and tools\\gate\\needs_lint.py import this file by path.

THE MODEL. out\\ is scratch. At the end of a session (and when no session is live) everything in it goes EXCEPT:
  (i)   ALLOW below: build trees and tool infrastructure, each with the tool that needs it;
  (ii)  tools\\harness\\keep_list.txt: one committed line per exact path, `path | reason | optional until YYYY-MM-DD`;
  (iii) paths a gate lane names (tools\\gate\\lanes\\*.json: needs / programs / exes / command arguments), EXACTLY that path (a file, or a
        leaf folder that is itself a capture / case / run), never a container; and a capture / case / run folder a check script names;
  (iv)  a --hold carry-over (until the next session ends).
No ages, no KEEP files. An unpack / scratch copy (fview_unpack_*, *_unpack) is never held by a gate-lane NAME (an explicit keep_list / --hold entry does hold it).
A name that is a container (a session folder out\\sNNN, out\\cases, out\\captures, a runs folder, or a folder that holds other
capture / case / run folders) protects nothing and is a lint error (tools\\gate\\needs_lint.py).
"""
import datetime
import fnmatch
import os
import re

# (pattern under out\\, who needs it). A pattern is a tree: it and everything under it stay. fnmatch globs; {VERSION} = the VERSION file.
# kind: sacred (Jarrod plays from it), rebuild (long to rebuild), noregen (needs game runs / paid work to remake), tool (a tool cannot run without it), never (irreplaceable).
ALLOW = [
    # SACRED (Jarrod, 2026-11-15 11:3x: "out/build which is where I play my game from, other than that I can't imagine we need much else"):
    # out\build is where he plays the game from, with whatever he set up there (settings, logs). NEVER deleted by any pass, --now included.
    ('build', 'sacred', 'the game build Jarrod plays from (his words, 2026-11-15): never deleted by any pass, --now included; also tools\\build.ps1, run_list'),
    # Long to rebuild: only what a tool cannot run without.
    ('renderer/dev', 'rebuild', 'fview / rplay dev build, a long rebuild: tools\\gate\\exes.py, every renderer gate lane (programs / exes)'),
    ('renderer/release', 'rebuild', 'fview / rplay release build, a long rebuild: tools\\gate\\lanes\\caw_ours.json, tools\\c1\\c1_check.py'),
    ('toolcache', 'rebuild', 'S103 GX: content-addressed fview / rplay / rig exe cache every checkout copies from (a worktree starts without out\\; a miss is a ~220 s rebuild per gate run): tools\\gate\\toolcache.py keeps only the newest 6 keys per exe'),
    ('release/build', 'rebuild', 'the release (PGO) build tree, a long rebuild: tools\\release\\release.ps1 / pgo_build.ps1'),
    ('android/sdk-install-arm64', 'rebuild', 'Android SDK install tree, a long rebuild: the Android game build links against it'),
    ('glslang-build', 'rebuild', 'glslang build, a long rebuild: tools\\perf\\gen_spirv_header.py, tools\\native\\compute_test'),
    # Jarrod, 2026-11-19 23:0x ("Yeah preserve out tools"): every tool / unit / rig exe the lanes and probes use (~217 files, ~2.9 GB; 176 built in S103).
    ('tools', 'rebuild', 'out\\tools: every unit / rig / check exe the gate lanes and probes run (Jarrod 2026-11-19: "preserve out tools"); lanes still name exact exes'),
    # Needed for the run tools to run at all.
    ('bench/userdata_frozen', 'tool', 'the frozen test user data mirror every test run copies: tools\\bench\\run_list.ps1 (CLAUDE.md Safety)'),
    ('bench/userdata_frozen.sha256', 'tool', 'its checksum: tools\\bench\\run_list.ps1'),
    ('harness/runlock', 'tool', 'run-lock slots: tools\\runlock.ps1'),
    # Not regenerable without GAME RUNS or paid work (Opus review, 2026-11-15): held whole.
    ('harness/index', 'noregen', 'tools\\q / `q fact` indexes (index.db, fact.db, image.bin): image.bin needs a GAME RUN to remake'),
    ('harness/labelling', 'noregen', 'function-labelling results (labeller output = paid agent work): tools\\labelling'),
    ('harness/determinism', 'noregen', 'determinism reference streams: need game runs to remake'),
    ('pgo', 'noregen', 'PGO profiles (guest_match.profdata, ...): need profiling runs; tools\\release\\pgo_build.ps1, gate lane s102_w'),
    ('android/game-arm64', 'rebuild', 'Android game build tree, a long rebuild: tools\\android\\build_game_android.ps1 (the newest apk is in keep_list.txt)'),
    ('android/game-arm64-off', 'rebuild', 'Android stream-free (player build) game tree, a long rebuild: tools\\android\\build_game_android.ps1 -StreamFree'),
    ('android/symbols-release', 'noregen', 'unstripped libmain.so of the shipped release APK (ndk-stack of a player crash): build_game_android.ps1 -Release'),
    ('android/symbols-off-release', 'noregen', 'unstripped libmain.so of the shipped stream-free release APK (ndk-stack of a player crash): build_game_android.ps1 -StreamFree -Release'),
    ('android/symbols-off', 'rebuild', 'unstripped libmain.so of the stream-free Android build (ndk-stack): build_game_android.ps1 -StreamFree'),
    ('android/launcher-arm64', 'rebuild', 'Android launcher build tree, a long rebuild: tools\\android\\build_launcher.ps1'),
    ('wt', 'tool', 'out\\wt git worktrees: handled by the worktree pass (merged and finished go at once; real edits are reported only)'),
    ('tidy', 'tool', "the tidy's own plans, history, free-space log, carry list"),
    # Never touched: tiny, made by hand or by Jarrod, cannot be regenerated.
    ('replays', 'never', "Jarrod's recorded replays (not regenerable)"),
    ('scenarios', 'never', 'scenario files (not regenerable)'),
    ('recordings', 'never', "Jarrod's input recordings the routes are built from (not regenerable)"),
    ('backup', 'never', 'backups made on purpose (memory_removed_s86, ...)'),
]

CONTAINER_NAMES = {'cases', 'captures', 'renderer', 'harness', 'android', 'release', 'wt', 'bench', 'gate', 'archive', 'runs',
                   'replays', 'scenarios', 'recordings', 'backup', 'tidy', 'cleanup', 'census', 'runtime', 'native', 'shaders', 'tools'}   # S103: 'tools' = out/tools, every tool exe (5.7 GB): a lane names the exact exe, never the folder
INPUT_CONTAINERS = {'cases', 'captures', 'runs'}          # a check SCRIPT naming one of these is a lint error too (a script naming out\\gate as its output is not)
SCRATCH_COPY = re.compile(r'(^|/)(fview_unpack_[^/]*|[^/]*_unpack|unpack_[^/]*|unpacked)(/|$)', re.I)
CAPTURE_MARKERS = {'images.bin', 'manifest.json'}
RUN_NAME = re.compile(r'^\d{8}_\d{6}_')


def norm(p):
    return re.sub(r'/+', '/', str(p).replace('\\', '/')).strip().rstrip('/').lower()


def is_scratch_copy(rel):
    return SCRATCH_COPY.search(norm(rel)) is not None


def allow_patterns(version=''):
    out = []
    for pat, kind, why in ALLOW:
        if '{VERSION}' in pat and not version:
            continue
        out.append(('out/' + norm(pat.replace('{VERSION}', version)), kind, why))
    return out


def pat_match(pat, rel):
    """rel at or under the tree `pat`."""
    pp, rp = pat.split('/'), rel.split('/')
    return len(rp) >= len(pp) and all(fnmatch.fnmatchcase(r, p) for r, p in zip(rp, pp))


def pat_ancestor(pat, rel):
    """rel is a strict ancestor of a path the pattern can match."""
    pp, rp = pat.split('/'), rel.split('/')
    return len(rp) < len(pp) and all(fnmatch.fnmatchcase(r, p) for r, p in zip(rp, pp))


def lane_strings(data):
    """The strings of a lane json that are INPUTS: checks[].cmd / needs / exes / programs[].path. Not owner, note, claim or description text
    (prose that mentions a folder protects nothing and is not linted)."""
    if not isinstance(data, dict):
        return
    for c in (data.get('checks') or []) + [data]:      # (a lane json may also carry the keys at its top level)
        if not isinstance(c, dict):
            continue
        for key in ('cmd', 'needs', 'exes'):
            v = c.get(key)
            for x in (v if isinstance(v, list) else [v]):
                if isinstance(x, str):
                    yield x
        for pr in c.get('programs') or []:
            if isinstance(pr, dict) and isinstance(pr.get('path'), str):
                yield pr['path']


LANE_NAME_RE = re.compile(r'(?<![A-Za-z0-9_])(out[\\/]+[^\s`\'"<>|*?(),;:\[\]{}]+[*?]?)')


def names_in(text):
    for m in LANE_NAME_RE.finditer(text):
        yield re.sub(r'[\\/]+', '/', m.group(1)).rstrip('./').lower()


def lint_lane_data(data, name, disk=None):
    """-> [error strings] for one parsed lane json: a name that is a container, or an unpack / scratch copy."""
    errs, seen = [], set()
    for s in lane_strings(data):
        for k in names_in(s):
            if k in seen:
                continue
            seen.add(k)
            why = container_reason(k, disk)
            if why:
                errs.append('%s: names "%s" which is a container (%s): list the exact files / capture folders the check reads' % (name, k, why))
            elif is_scratch_copy(k):
                errs.append('%s: names "%s" which is an unpack / scratch copy: nothing ever holds those; name the source (a capture folder or a case zip)' % (name, k))
    return errs


# ---------------------------------------------------------------- the keep list
def parse_date(s):
    try:
        return datetime.date.fromisoformat(s.strip())
    except ValueError:
        return None


def load_keep_list(path, today=None):
    """-> (entries, problems). entry = {'path': 'out/...', 'reason', 'until': date or None, 'line': n}. A line: `path | reason | optional
    YYYY-MM-DD`. Problems (a line without a reason, a container, a scratch copy, a bad date, an expired entry) are strings; those entries do not hold."""
    today = today or datetime.date.today()
    entries, problems = [], []
    try:
        lines = open(path, encoding='utf-8').read().splitlines()
    except OSError:
        return [], ['keep list missing: ' + str(path)]
    for n, raw in enumerate(lines, 1):
        s = raw.strip()
        if not s or s.startswith('#'):
            continue
        parts = [x.strip() for x in s.split('|')]
        p = norm(parts[0])
        if not p.startswith('out/') or len(p.split('/')) < 2 or any(c in p for c in '*?'):
            problems.append('keep_list line %d: "%s" is not an exact path under out/' % (n, parts[0]))
            continue
        if len(parts) < 2 or not parts[1]:
            problems.append('keep_list line %d: %s has no reason' % (n, p))
            continue
        until = None
        if len(parts) > 2 and parts[2]:
            until = parse_date(parts[2])
            if until is None:
                problems.append('keep_list line %d: %s: bad date "%s" (want YYYY-MM-DD)' % (n, p, parts[2]))
                continue
        why = container_reason(p)
        if why:
            problems.append('keep_list line %d: %s is a container (%s): list the exact paths inside it' % (n, p, why))
            continue
        if until is not None and until < today:
            problems.append('keep_list line %d: %s expired %s (no longer holds)' % (n, p, until))
            continue
        entries.append({'path': p, 'reason': parts[1], 'until': until, 'line': n})
    return entries, problems


# ---------------------------------------------------------------- containers
def container_reason(rel, disk=None):
    """'' when `rel` (out/..., lower case) is an exact input; else why it is a container. disk: the real out folder (adds the on-disk test: a
    folder holding two or more capture / case / run folders). A path that the ALLOW list covers exactly is not judged here."""
    rel = norm(rel)
    star = rel.endswith('*')
    base = rel.rstrip('*').rstrip('/')
    parts = base.split('/')
    if parts[0] != 'out' or len(parts) < 2:
        return ''
    if star and base != rel.rstrip('*'):      # 'out/x/*' is a glob over children
        return 'a glob over the children of a folder'
    if star and len(parts) == 2:
        return 'a glob over everything under out/'
    if len(parts) == 2:
        top = parts[1]
        if re.fullmatch(r's\d+', top):
            return 'a session folder'
        if top in CONTAINER_NAMES:
            for pat, _k, _w in allow_patterns('x'):
                if pat == base:
                    return ''      # an allowlisted tree is named for what it is
            return 'a top-level folder that holds other runs / captures'
    if parts[-1] == 'runs' and len(parts) >= 2:
        return 'a runs folder (holds every run)'
    if disk and not star:
        p = os.path.join(disk, *parts[1:])
        n = _capture_children(p)
        if n >= 2:
            return 'holds %d capture / case / run folders' % n
    return ''


def is_capture_dir(p):
    try:
        names = {e.name.lower() for e in os.scandir(p)}
    except OSError:
        return False
    return bool(CAPTURE_MARKERS & names) or any(n.startswith('case_') and n.endswith('.zip') for n in names)


def _capture_children(p):
    try:
        kids = [e for e in os.scandir(p) if e.is_dir(follow_symlinks=False)]
    except OSError:
        return 0
    n = 0
    for e in kids:
        if RUN_NAME.match(e.name) or e.name.lower().startswith('case_') or is_capture_dir(e.path) or os.path.isdir(os.path.join(e.path, 'capture')):
            n += 1
    return n


# ---------------------------------------------------------------- what is held
class HeldIndex:
    """Everything the sweep must not delete: allowlist trees, keep-list paths, gate-named exact paths, carry-overs."""

    def __init__(self, version='', keep=(), gate=None, carried=()):
        self.pats = [(p, 'allow:' + k, w) for p, k, w in allow_patterns(version)]
        for e in keep:
            self.pats.append((e['path'], 'keep list', e['reason']))
        self.prefixes = []           # glob-prefix gate names ('out/a/b*'): a prefix match
        for k, src in (gate or {}).items():
            k = norm(k.replace('*', '\0')).replace('\0', '*') if '*' in k else norm(k)
            if is_scratch_copy(k.rstrip('*')):
                continue      # a gate-lane NAME of an unpack copy never holds it (explicit keep-list / --hold entries do: fix 7)
            if k.endswith('*'):
                self.prefixes.append((k[:-1], src))
            else:
                self.pats.append((k, 'gate lane', src))
        for c in carried:
            self.pats.append(('out/' + norm(c)[4:] if norm(c).startswith('out/') else norm(c), 'carry-over (--hold)', 'until the next session ends'))

    def why(self, rel):
        """'' or 'kind: reason' when rel is at or under a held path. A scratch copy is never held (unless an allowlist tree holds its parent)."""
        rel = norm(rel)
        for pat, kind, w in self.pats:
            if pat_match(pat, rel):
                if is_scratch_copy(rel) and kind == 'gate lane':
                    continue      # only a gate-lane name is overridden by the scratch pattern; the keep list and --hold override it
                return '%s: %s' % (kind, w)
        for pre, src in self.prefixes:
            if rel.startswith(pre) and not is_scratch_copy(rel):
                return 'gate lane: %s' % src
        return ''

    def ancestor(self, rel):
        """rel is a strict ancestor of something held (the sweep must look inside it, not take it whole)."""
        rel = norm(rel)
        for pat, kind, _w in self.pats:
            if pat_ancestor(pat, rel):
                return True
        return any(pre.startswith(rel + '/') for pre, _s in self.prefixes)

    def rows(self):
        """[(path pattern, kind, why)] for the report."""
        return list(self.pats) + [(p + '*', 'gate lane', s) for p, s in self.prefixes]


# ---------------------------------------------------------------- the sweep
REPARSE = 0x400


def _lp(p):
    return p if p.startswith('\\\\?\\') else '\\\\?\\' + os.path.abspath(p)


def stats(path):
    """(bytes, files, newest birth/mtime, links, gitfile, KEEP files): no link is followed; an unreadable folder counts as a link (unsafe)."""
    keeps = []
    b = f = links = 0
    newest = 0.0
    git = False
    stack = [path]
    while stack:
        cur = stack.pop()
        try:
            it = list(os.scandir(_lp(cur)))
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
                if e.name == '.git':
                    git = True      # a nested clone / worktree: never swept (fix 6)
                stack.append(os.path.join(cur, e.name))
            else:
                b += st.st_size
                f += 1
                newest = max(newest, st.st_mtime, getattr(st, 'st_birthtime', st.st_ctime))
                if e.name == '.git':
                    git = True
                elif e.name == 'KEEP' and len(keeps) < 20:
                    keeps.append(cur)
    return b, f, newest, links, git, keeps


def scratch_inside(path, depth=4):
    """Unpack / scratch-copy folders (fview_unpack_*, *_unpack) inside a held folder, up to `depth` levels down; never through a link."""
    found = []

    def walk(p, d):
        try:
            for e in os.scandir(_lp(p)):
                if not e.is_dir(follow_symlinks=False) or (e.stat(follow_symlinks=False).st_file_attributes & REPARSE):
                    continue
                if SCRATCH_COPY.search(e.name):
                    found.append(os.path.join(p, e.name))
                elif d < depth:
                    walk(os.path.join(p, e.name), d + 1)
        except OSError:
            pass
    walk(path, 0)
    return found


def sweep(out_root, held, skip_dirs=(), idle_cutoff=None, type_of=None):
    """Walk out\\ and return (items, held_rows, notes). items = [{'p','b','n','t','types','dir'}] the pass would delete: every child that is not held
    and does not lead to something held; a folder leading to something held is entered, never taken whole. skip_dirs: normcased absolute
    folders that are registered worktrees (the worktree pass owns them). idle_cutoff (emergency while a session is live): a folder or file
    changed after it stays. Held roots are returned unsized as (rel, why, path); the caller sizes them."""
    items, held_rows, notes = [], [], {'links': 0, 'worktrees': [], 'recent': 0, 'keeps': []}
    out_root = os.path.abspath(out_root)

    def visit(path, rel):
        try:
            entries = list(os.scandir(_lp(path)))
        except OSError:
            return
        for e in entries:
            child = os.path.join(path, e.name)
            crel = rel + '/' + e.name.lower()
            try:
                st = e.stat(follow_symlinks=False)
            except OSError:
                continue
            if st.st_file_attributes & REPARSE:
                notes['links'] += 1
                continue
            w = held.why(crel)
            if w:
                held_rows.append((crel, w, child))
                if e.is_dir(follow_symlinks=False) and not w.startswith('allow'):
                    for sp in scratch_inside(child):      # an unpack copy inside a held folder is still scratch, unless a keep-list / --hold entry holds it
                        if held.why(crel + '/' + os.path.relpath(sp, child).replace('\\', '/').lower()):
                            continue
                        b, n, newest, _l, _g, _k = stats(sp)
                        items.append({'p': sp, 'b': b, 'n': n, 't': newest, 'types': {'folders': b}, 'dir': True, 'links': 0})
                continue
            if e.is_dir(follow_symlinks=False):
                if os.path.normcase(child) in skip_dirs:
                    notes['worktrees'].append(child)
                    continue
                if held.ancestor(crel):
                    visit(child, crel)
                    continue
                b, n, newest, links, git, kp = stats(child)
                notes['keeps'] += kp
                if git:
                    notes['worktrees'].append(child)      # an unregistered git worktree: never swept
                    continue
                if idle_cutoff is not None and newest >= idle_cutoff:
                    notes['recent'] += 1
                    visit(child, crel)      # something in it is recent: only its idle children go
                    continue
                items.append({'p': child, 'b': b, 'n': n, 't': newest, 'types': {'folders': b}, 'dir': True, 'links': links})
            else:
                t = max(st.st_mtime, getattr(st, 'st_birthtime', st.st_ctime))
                if idle_cutoff is not None and t >= idle_cutoff:
                    notes['recent'] += 1
                    continue
                items.append({'p': child, 'b': st.st_size, 'n': 1, 't': t, 'types': {(type_of(e.name) if type_of else 'files'): st.st_size}, 'dir': False})

    visit(out_root, 'out')
    return items, held_rows, notes
