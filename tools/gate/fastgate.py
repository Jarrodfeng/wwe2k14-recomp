# The fast gate's library (S102 lane M4): which checks a change can affect, what a result is cached by, and whether a program is stale.
# Used by tools\gate\gate.py (--changed [BASE], --jobs N, --no-cache, --build-stale); selftest: tools\gate\fastgate_selftest.py.
#
# A lane json (or one check in it) may declare  "affects": ["src/renderer/**", "tools/crowd/*.py"]  (globs, posix slashes, relative to the repo root;
# ** crosses folders, * does not). A change (git diff --name-only BASE, plus untracked files) selects a check when any changed path matches one of
# its globs, or is the lane's own json. A check with NO "affects" always runs and is never cached (nothing says what it reads).
# A check may also declare  "programs": [{"path": "out/caster/caster_rows_test.exe", "sources": ["tools/caster/**", "src/renderer/**"]}]:
# the program must exist and be at least as new as every file its globs match, else the check does not run: FAIL "stale build" (the S101 lesson:
# a --no-build check on an old exe gave a false gate result three times in one day). The exes the gate already knows (tools\gate\exes.py) are
# held to the same rule in --changed mode (the fast gate never rebuilds; --build-stale restores the old build-once behaviour).
# Cache key = sha256(lane json bytes, check name + cmd + timeout, content hash of every file the affects globs match, content hash of every
# program). Only a PASS is cached; a SKIP, FAIL or BUILD never is. times.csv gets one row per check and one for the whole gate.
import csv, hashlib, os, re, subprocess, time
from pathlib import Path


def glob_re(pat):
    pat = pat.replace('\\', '/')
    out, i = '', 0
    while i < len(pat):
        c = pat[i]
        if pat.startswith('**/', i):
            out += '(?:.*/)?'
            i += 3
        elif pat.startswith('**', i):
            out += '.*'
            i += 2
        elif c == '*':
            out += '[^/]*'
            i += 1
        elif c == '?':
            out += '[^/]'
            i += 1
        else:
            out += re.escape(c)
            i += 1
    return re.compile('^' + out + '$')


def matches(path, globs):
    p = path.replace('\\', '/')
    return any(glob_re(g).match(p) for g in globs)


def _git(root, *a):
    p = subprocess.run(['git', '-C', str(root)] + list(a), capture_output=True, text=True, timeout=120)
    if p.returncode != 0:
        raise RuntimeError(f'git {" ".join(a)} failed: {(p.stderr or p.stdout).strip()[:200]}')
    return p.stdout


def changed_files(root, base):
    """Paths (posix, relative to root) that differ from BASE in the working tree, plus untracked files. Raises when BASE is not a revision."""
    # --no-renames: a file moved OUT of a declared folder must list its OLD path too (a rename would show only the new one and the lane would not run)
    names = set(_git(root, 'diff', '--name-only', '--no-renames', base).split('\n'))
    names |= set(_git(root, 'ls-files', '--others', '--exclude-standard').split('\n'))
    return sorted(n.strip().replace('\\', '/') for n in names if n.strip())


def tree_files(root):
    """Every file in the checkout (tracked + untracked, not ignored), posix relative paths. Falls back to a walk without git."""
    try:
        names = set(_git(root, 'ls-files').split('\n')) | set(_git(root, 'ls-files', '--others', '--exclude-standard').split('\n'))
        return sorted(n.strip().replace('\\', '/') for n in names if n.strip())
    except Exception:
        out = []
        for dp, dn, fn in os.walk(root):
            dn[:] = [d for d in dn if d not in ('.git', 'out')]
            out += [os.path.relpath(os.path.join(dp, f), root).replace('\\', '/') for f in fn]
        return sorted(out)


def lane_affects(check):
    a = check.get('affects')
    return [a] if isinstance(a, str) else a


def selected(check, changed, lane_rel):
    """True when this check should run for the changed paths. No 'affects' -> always."""
    aff = lane_affects(check)
    if aff is None:
        return True, 'no affects declared: always runs'
    for p in changed:
        if p == lane_rel:
            return True, 'its lane json changed'
        if matches(p, aff):
            return True, f'{p} matches'
    return False, 'no changed path matches its affects'


_MEMO = {}


def file_hash(path):
    p = str(path)
    try:
        st = os.stat(p)
    except OSError:
        return 'missing'
    key = (p, st.st_mtime_ns, st.st_size)
    h = _MEMO.get(key)
    if h is None:
        d = hashlib.blake2b(digest_size=16)
        with open(p, 'rb') as f:
            for blk in iter(lambda: f.read(1 << 20), b''):
                d.update(blk)
        h = _MEMO[key] = d.hexdigest()
    return h


def input_hash(root, check, files=None):
    """Content hash over every file the check's affects globs match (sorted by path)."""
    aff = lane_affects(check) or []
    files = files if files is not None else tree_files(root)
    d = hashlib.sha256()
    n = 0
    for f in files:
        if matches(f, aff):
            d.update(f.encode() + b'\0' + file_hash(Path(root) / f).encode() + b'\n')
            n += 1
    return d.hexdigest(), n


def program_list(check):
    return [{'path': p['path'].replace('\\', '/'), 'sources': p.get('sources', [])} for p in check.get('programs', [])]


def stale_program(root, prog, files=None):
    """None when the program exists and is not older than any source its globs match; else the reason (a 'stale build' text)."""
    root = Path(root)
    exe = root / prog['path']
    if not exe.is_file():
        return f"{prog['path']}: missing (build it first)"
    t = exe.stat().st_mtime
    files = files if files is not None else tree_files(root)
    newest, who = 0.0, None
    for f in files:
        if matches(f, prog['sources']):
            try:
                m = os.stat(root / f).st_mtime
            except OSError:
                continue
            if m > newest:
                newest, who = m, f
    if newest > t:
        return f"{prog['path']}: older than its source {who} (rebuild it)"
    return None


ENV_ALLOW = ('GATE_EXE_WAIT_S', 'GATE_NO_RESTORE', 'WWE_FVIEW', 'WWE_FVIEW_ALLOW_STALE', 'PYTHONHASHSEED')   # plus any RDR_* / WWE2K14_*


def env_part(env=None):
    env = os.environ if env is None else env
    keys = sorted(k for k in env if k in ENV_ALLOW or k.startswith(('RDR_', 'WWE2K14_')))
    return repr([(k, env[k]) for k in keys])


def path_state(p):
    """A needs path or cmd argument: a file by content; a folder by its files' relative path + size + mtime (a capture folder is too big to read);
    missing = 'missing'."""
    p = Path(p)
    if p.is_file():
        return 'f:' + file_hash(p)
    if p.is_dir():
        d = hashlib.sha256()
        for dp, dn, fn in os.walk(p):
            dn.sort()
            for f in sorted(fn):
                try:
                    st = os.stat(os.path.join(dp, f))
                except OSError:
                    continue
                d.update(f'{os.path.relpath(os.path.join(dp, f), p)}\0{st.st_size}\0{st.st_mtime_ns}\n'.encode())
        return 'd:' + d.hexdigest()
    return 'missing'


def arg_files(root, main, cmd):
    """Existing files named by a command's arguments (a trailing :N / :name suffix is allowed), resolved under root, else under the main checkout."""
    out = []
    for a in cmd:
        if not isinstance(a, str) or a.startswith('-'):
            continue
        base = a.split(':')[0] if not re.match(r'^[A-Za-z]:[\\/]', a) else a
        for cand in (Path(root) / base, Path(main) / base, Path(base)):
            try:
                if cand.is_file():
                    out.append(str(cand))
                    break
            except OSError:
                pass
    return out


def cache_key(root, check, lane_bytes, prog_paths, files=None, main=None):
    """-> key, or None when the check cannot be cached (no affects declared, or "cache": false). Besides the lane json, the cmd and the files its
    affects globs match, the key holds: every `needs` path (under the main checkout; a file by content, a folder by file sizes and times), every cmd
    argument that is an existing file (by content: a script a check runs is an input), the Python version and an allow-list of environment variables."""
    import sys
    if lane_affects(check) is None or check.get('cache') is False:
        return None
    main = main or root
    ih, _ = input_hash(root, check, files)
    d = hashlib.sha256()
    d.update(lane_bytes)
    d.update(repr((check.get('name'), check.get('cmd'), check.get('timeout_s'), check.get('needs'))).encode())
    d.update(ih.encode())
    for n in sorted(check.get('needs', [])):
        d.update(b'N' + n.encode() + b'\0' + path_state(Path(main) / n).encode())
    for f in sorted(arg_files(root, main, check.get('cmd', []))):
        d.update(b'A' + f.encode() + b'\0' + file_hash(f).encode())
    d.update(b'PY' + sys.version.encode())
    d.update(b'ENV' + env_part().encode())
    for p in sorted(prog_paths):
        d.update(p.encode() + b'\0' + file_hash(Path(root) / p).encode())
    return d.hexdigest()


def validate_affects(root, checks, files=None):
    """-> list of problems: an `affects` that is not a non-empty list of strings, or a glob that matches no file in the tree. A silent never-selected
    (or always-selected) lane is worse than a loud one."""
    files = files if files is not None else tree_files(root)
    seen, bad = set(), []
    for c in checks:
        aff = lane_affects(c)
        if aff is None or (c['lane'], repr(aff)) in seen:
            continue
        seen.add((c['lane'], repr(aff)))
        if not isinstance(aff, list) or not aff or not all(isinstance(g, str) and g for g in aff):
            bad.append(f"{c['lane']}: 'affects' must be a non-empty list of globs, got {aff!r}")
            continue
        for g in aff:
            if not any(matches(f, [g]) for f in files):
                bad.append(f"{c['lane']}: affects glob {g!r} matches no file")
    return bad


def serial_needed(check):
    """Checks that must not run beside others: no `affects` (nothing says what they touch) or a PowerShell build (.ps1 in the cmd), unless the check or
    its lane says "parallel_safe": true."""
    if check.get('parallel_safe') is True:
        return False
    cmd = check.get('cmd', [])
    if any(isinstance(a, str) and a.lower().endswith('.ps1') for a in cmd):
        return True
    return lane_affects(check) is None


def default_base(root, branch='s102-arc-pt2'):
    """merge-base of HEAD with the session branch; raises with a clear message when the branch does not exist."""
    try:
        return _git(root, 'merge-base', 'HEAD', branch).strip()
    except RuntimeError as e:
        raise RuntimeError(f'no BASE given and the session branch {branch} is not here ({e}); pass --changed BASE')


def load_cache(path):
    import json
    try:
        return json.loads(Path(path).read_text(encoding='utf-8'))
    except Exception:
        return {}


def save_cache(path, cache):
    """Merge into whatever another gate run saved meanwhile, then replace the file atomically (write a temp file, os.replace)."""
    import json
    cur = load_cache(path)
    cur.update(cache)
    tmp = Path(str(path) + f'.{os.getpid()}.tmp')
    tmp.write_text(json.dumps(cur, indent=0), encoding='utf-8')
    os.replace(tmp, path)


TIMES_HEAD = ['when', 'root', 'mode', 'lane', 'check', 'verdict', 'seconds', 'cache']


def append_times(path, rows):
    p = Path(path)
    new = not p.exists() or p.stat().st_size == 0
    with open(p, 'a', newline='', encoding='utf-8') as f:
        w = csv.writer(f, lineterminator='\n')
        if new:
            w.writerow(TIMES_HEAD)
        w.writerows(rows)
