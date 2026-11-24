# entry: main
r"""Selftest for wait_commit.py in a throwaway repo under %TEMP% (never the real repo's refs).  py -I tools\harness\wait_commit_selftest.py

S108 lane F review: the repo lives under tempfile.mkdtemp() with GIT_CEILING_DIRECTORIES at its parent and every GIT_* variable
dropped, and the toplevel is checked before any commit, so a failed `git init` can never fall through to the enclosing repository
(the first version used out\laneF inside the worktree; a half-deleted .git there resolved to the builder's worktree).
"""
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, 'wait_commit.py')
BASE = os.path.realpath(tempfile.mkdtemp(prefix='wc_selftest_'))
ROOT = os.path.join(BASE, 'repo')
WT = os.path.join(BASE, 'wt')
ENV = {k: v for k, v in os.environ.items() if not k.upper().startswith('GIT_')}
ENV['GIT_CEILING_DIRECTORIES'] = BASE


def git_in(where, *a, check=True):
    r = subprocess.run(['git', '-C', where, '-c', 'user.name=t', '-c', 'user.email=t@t', *a], capture_output=True, text=True, env=ENV)
    if check and r.returncode:
        raise RuntimeError(f'git {a} failed: {r.stderr.strip()}')
    return r.stdout.strip()


def git(*a):
    return git_in(ROOT, *a)


def commit(msg, where=None):
    where = where or ROOT
    with open(os.path.join(where, 'f.txt'), 'a') as f:
        f.write(msg + '\n')
    git_in(where, 'add', 'f.txt')
    git_in(where, 'commit', '-q', '-m', msg)
    return git_in(where, 'rev-parse', 'HEAD')


def wait(*a, cwd=None, repo=True):
    args = [sys.executable, '-I', TOOL] + (['--repo', ROOT] if repo else []) + list(a)
    t0 = time.monotonic()
    r = subprocess.run(args, capture_output=True, text=True, cwd=cwd, env=ENV)
    return r.returncode, r.stdout.strip(), time.monotonic() - t0


def rmtree(p):
    def fix(func, path, _exc):
        os.chmod(path, stat.S_IWRITE)   # git object files are read-only on Windows
        func(path)
    kw = {'onexc': fix} if sys.version_info >= (3, 12) else {'onerror': fix}
    shutil.rmtree(p, **kw)


def main():
    try:
        return run()
    finally:
        rmtree(BASE)


def run():
    os.makedirs(ROOT)
    git('init', '-q', '-b', 'br')
    top = os.path.normcase(os.path.realpath(git('rev-parse', '--show-toplevel')))
    if top != os.path.normcase(ROOT):
        print('FAIL temp repo toplevel is', top)   # never commit anywhere else
        return 1
    base = commit('base')
    bad = []

    def check(name, got, code, prefix, max_s=None, min_s=None):
        ok = got[0] == code and got[1].startswith(prefix)
        ok = ok and (max_s is None or got[2] <= max_s) and (min_s is None or got[2] >= min_s)
        print('ok  ' if ok else 'FAIL', name, got[:2], f'{got[2]:.1f} s')
        if not ok:
            bad.append(name)

    check('timeout', wait('--branch', 'br', '--after', base, '--timeout', '2'), 3, 'TIMEOUT ' + base, max_s=8, min_s=1.9)
    th = threading.Timer(2.0, lambda: commit('hand-back'))
    th.start()
    got = wait('--branch', 'br', '--after', base, '--timeout', '15')
    th.join()
    head = git('rev-parse', 'HEAD')
    check('commit', got, 0, f'COMMIT {head} hand-back')
    # an abbreviated --after resolves to the full sha; a --after that is not a commit is refused, not reported as MOVED
    check('abbreviated after', wait('--branch', 'br', '--after', base[:9], '--timeout', '5'), 0, f'COMMIT {head} ')
    check('tree after', wait('--branch', 'br', '--after', git('rev-parse', 'HEAD^{tree}'), '--timeout', '5'), 4, 'BAD AFTER', max_s=4)
    check('missing after', wait('--branch', 'br', '--after', 'nosuchrev', '--timeout', '5'), 4, 'BAD AFTER', max_s=4)
    # marker: a wip commit is ignored, a marker commit returns; a marker commit BEFORE --after never counts
    git_in(ROOT, 'commit', '-q', '--allow-empty', '-m', 'HAND-BACK old')
    cur = commit('plan')
    wip = commit('wip: stuff')
    check('marker ignores wip and older marker', wait('--branch', 'br', '--after', cur, '--timeout', '2', '--marker', 'HAND-BACK'), 3, 'TIMEOUT ')
    hb = commit('HAND-BACK done')
    check('marker match', wait('--branch', 'br', '--after', cur, '--timeout', '5', '--marker', 'HAND-BACK'), 0, f'COMMIT {hb} HAND-BACK done')
    # marker: a commit merged in from a side branch that does not descend from --after never counts (e.g. the session branch merged in)
    plan2 = git('rev-parse', 'HEAD')
    git('checkout', '-q', '-b', 'side', base)
    git('commit', '-q', '--allow-empty', '-m', 'HAND-BACK other lane')
    git('checkout', '-q', 'br')
    git('merge', '-q', '--no-ff', '-m', 'merge side', 'side')
    check('marker ignores merged-in side commit', wait('--branch', 'br', '--after', plan2, '--timeout', '2', '--marker', 'HAND-BACK'), 3, 'TIMEOUT ')
    # the branch checked out in another worktree; the tool run from that worktree's cwd (default --repo .) and from the main repo
    git('worktree', 'add', '-q', '-b', 'wtb', WT, base)
    th = threading.Timer(2.0, lambda: commit('wt hand-back', WT))
    th.start()
    got = wait('--branch', 'wtb', '--after', base, '--timeout', '15', cwd=WT, repo=False)
    th.join()
    check('other worktree cwd', got, 0, f'COMMIT {git_in(WT, "rev-parse", "HEAD")} wt hand-back')
    wt_head = git_in(WT, 'rev-parse', 'HEAD')
    th = threading.Timer(2.0, lambda: commit('wt second', WT))
    th.start()
    got = wait('--branch', 'wtb', '--after', wt_head, '--timeout', '15', cwd=ROOT, repo=False)
    th.join()
    check('branch checked out elsewhere', got, 0, 'COMMIT ')
    # rewound: branch reset to an ANCESTOR of --after -> MOVED at once (not at the timeout)
    tip = git('rev-parse', 'br')
    git('checkout', '-q', '--detach')
    git('branch', '-f', 'br', base)
    check('rewound to ancestor', wait('--branch', 'br', '--after', tip, '--timeout', '15'), 4, 'MOVED ' + base, max_s=5)
    # reset to an unrelated commit
    git('checkout', '-q', '--orphan', 'other')
    with open(os.path.join(ROOT, 'g.txt'), 'w') as f:
        f.write('x')
    git('add', 'g.txt')
    git('commit', '-q', '-m', 'unrelated')
    unrelated = git('rev-parse', 'HEAD')
    git('branch', '-f', 'br', unrelated)
    check('moved', wait('--branch', 'br', '--after', wip, '--timeout', '5'), 4, 'MOVED ' + unrelated, max_s=5)
    check('no branch', wait('--branch', 'nope', '--after', base, '--timeout', '5'), 4, 'NO BRANCH', max_s=5)
    print('PASS wait_commit selftest' if not bad else f'FAIL {bad}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
