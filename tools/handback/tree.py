#!/usr/bin/env python3
"""Temporary detached worktrees for the hand-back check (the base tree) and its re-find (old slip trees). Lane HBK, review blocker 3 + round 3 item 6.

The danger: a folder that EXISTS but is no longer a registered worktree. `git -C <folder> checkout --detach SHA` then falls through to the enclosing
repository and moves ITS head (a builder's branch, the main checkout). So, before every checkout:
  1. `git worktree prune` (forget worktrees whose folder is gone);
  2. the folder must be listed as an exact `worktree <path>` line of `git worktree list --porcelain` (or not exist / be empty, and is then added);
  3. `git -C <folder> rev-parse --show-toplevel` must be the folder itself;
  4. a reused tree must be CLEAN (`git status --porcelain --untracked-files=all` empty) before the checkout: a leftover of a dead run keeps changed and
     untracked files, so its HEAD would match while its files are not that commit;
  5. after the checkout, HEAD must be the wanted commit and the tree clean.
Anything else raises TreeError and nothing is checked out. Removal is `git worktree remove` (never --force) + prune.
sweep(): the tool's own trees (<dir>/wt_<pid>_<random>) whose pid is dead are removed the same guarded way; a registration still "locked initializing"
(a run killed inside `worktree add`) and a dirty one are reported, never forced."""
import ctypes
import os
import re
import secrets
import subprocess


class TreeError(RuntimeError):
    pass


def _git(cwd, *args, timeout=300):
    r = subprocess.run(['git', '-C', cwd] + list(args), capture_output=True, text=True, timeout=timeout)
    if r.returncode:
        raise TreeError('git %s (in %s): exit %d: %s' % (' '.join(args[:3]), cwd, r.returncode, r.stderr.strip()[:300]))
    return r.stdout.strip()


def norm(p):
    return os.path.normcase(os.path.realpath(os.path.abspath(p))).replace('\\', '/').rstrip('/')


def worktrees(repo):
    """[{'path':..., 'locked': reason or None, ...}] from `git worktree list --porcelain`."""
    out, cur = [], None
    for ln in _git(repo, 'worktree', 'list', '--porcelain').splitlines():
        if ln.startswith('worktree '):
            cur = {'path': ln[len('worktree '):], 'locked': None}
            out.append(cur)
        elif cur is not None and ln.startswith('locked'):
            cur['locked'] = ln[len('locked'):].strip() or 'locked'
    return out


def listed(repo, path):
    want = norm(path)
    return any(norm(w['path']) == want for w in worktrees(repo))


def dirty(path):
    """The `git status --porcelain --untracked-files=all` lines of a tree (empty = clean)."""
    return _git(path, 'status', '--porcelain', '--untracked-files=all').splitlines()


def new_name(folder):
    return os.path.join(folder, 'wt_%d_%s' % (os.getpid(), secrets.token_hex(4)))


def checkout(repo, path, rev):
    """Make `path` a clean detached worktree of `repo` at `rev` (added when missing). Returns the full sha. Raises TreeError on any doubt."""
    sha = _git(repo, 'rev-parse', '--verify', rev + '^{commit}')
    _git(repo, 'worktree', 'prune')
    if not listed(repo, path):
        if os.path.exists(path) and (not os.path.isdir(path) or os.listdir(path)):
            raise TreeError('%s exists but is not a registered worktree of %s: refusing to check anything out there (remove the folder by hand)' % (path, repo))
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        _git(repo, 'worktree', 'add', '-q', '--detach', path, sha)
    top = _git(path, 'rev-parse', '--show-toplevel')
    if norm(top) != norm(path):
        raise TreeError('%s is not its own worktree (its top level is %s): refusing the checkout' % (path, top))
    d = dirty(path)
    if d:
        raise TreeError('%s is a reused tree with %d changed / untracked file(s) (%s): refusing it (its files would not be the commit)' % (path, len(d), d[0][:80]))
    _git(path, 'checkout', '-q', '--detach', sha)
    head = _git(path, 'rev-parse', 'HEAD')
    if head != sha:
        raise TreeError('%s HEAD is %s after the checkout, wanted %s' % (path, head[:9], sha[:9]))
    d = dirty(path)
    if d:
        raise TreeError('%s is not clean after the checkout (%s)' % (path, d[0][:80]))
    return sha


def remove(repo, path):
    """`git worktree remove` (never --force), then prune. Returns None, or the reason it was left."""
    try:
        if listed(repo, path):
            _git(repo, 'worktree', 'remove', path)
        _git(repo, 'worktree', 'prune')
    except TreeError as e:
        return str(e)
    return None


def pid_alive(pid):
    """True when a process with this pid runs (Windows: OpenProcess + GetExitCodeProcess; never a signal: os.kill would TERMINATE it there)."""
    if os.name == 'nt':
        k = ctypes.windll.kernel32
        h = k.OpenProcess(0x1000, False, int(pid))   # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return ctypes.GetLastError() == 5   # access denied = it exists
        try:
            code = ctypes.c_ulong()
            return bool(k.GetExitCodeProcess(h, ctypes.byref(code))) and code.value == 259   # STILL_ACTIVE
        finally:
            k.CloseHandle(h)
    try:
        os.kill(int(pid), 0)
        return True
    except OSError:
        return False


NAME = re.compile(r'^wt_(\d+)_[0-9a-f]+$')


def sweep(repo, folder):
    """Remove this tool's trees under `folder` whose owning pid is dead. Returns report lines (what was removed, what was left and why)."""
    out = []
    try:
        wts = worktrees(repo)
    except TreeError as e:
        return ['sweep: worktree list failed: %s' % e]
    mine = norm(folder) + '/'
    for w in wts:
        p = norm(w['path'])
        if not p.startswith(mine):
            continue
        m = NAME.match(os.path.basename(p))
        if not m or pid_alive(int(m.group(1))):
            continue
        if w['locked']:
            out.append('sweep: %s is still registered "locked %s" (a run killed inside `worktree add`?): left alone, nothing forced; `git worktree unlock` + remove it by hand' % (w['path'], w['locked']))
            continue
        try:
            d = dirty(w['path']) if os.path.isdir(w['path']) else []
        except TreeError as e:
            out.append('sweep: %s left (%s)' % (w['path'], e))
            continue
        if d:
            out.append('sweep: %s left: a dead run\'s tree with %d changed / untracked file(s) (never forced)' % (w['path'], len(d)))
            continue
        why = remove(repo, w['path'])
        out.append('sweep: removed %s (pid %s dead)' % (w['path'], m.group(1)) if not why else 'sweep: %s left (%s)' % (w['path'], why))
    return out
