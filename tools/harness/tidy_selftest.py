"""Helper library for the tidy selftests (lane 154, Session 101): World, age_tree, write, junction, git, Results.
The old time-based suites that lived here (rolling ages, KEEP files, state-doc names) were removed in S107 (rule 6: dead since TIDY2).
The out\\ rules are tested by tidy_out_selftest.py, tidy_now_selftest.py and tidy_k1_selftest.py.

Fake trees only: nothing here touches the real out\\ or any real worktree. File ages are set with SetFileTime (creation and
write time), because the tidy goes by max(mtime, creation).
"""
import ctypes
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
NEW = os.path.join(HERE, 'tidy.py')
OLD_REV = 'ca08f298'      # tidy.py as it was before lane 154
REPO_REAL = os.path.dirname(os.path.dirname(HERE))
H = 3600.0
REPARSE = stat.FILE_ATTRIBUTE_REPARSE_POINT
NOWIN = 0x08000000

k32 = ctypes.windll.kernel32
k32.CreateFileW.restype = ctypes.c_void_p
k32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
k32.SetFileTime.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
k32.CloseHandle.argtypes = [ctypes.c_void_p]


def set_times(path, t):
    ft = ctypes.c_ulonglong(int((t + 11644473600) * 1e7))
    h = k32.CreateFileW('\\\\?\\' + os.path.abspath(path), 0x100, 7, None, 3, 0x02000000 | 0x00200000, None)
    if h in (None, ctypes.c_void_p(-1).value):
        raise OSError('SetFileTime open failed: ' + path)
    k32.SetFileTime(h, ctypes.byref(ft), ctypes.byref(ft), ctypes.byref(ft))
    k32.CloseHandle(h)


def age_tree(root, t):
    """Every file and folder under root (junctions not followed, not touched) gets the time t."""
    stack, dirs = [root], []
    while stack:
        cur = stack.pop()
        dirs.append(cur)
        for e in os.scandir(cur):
            st = e.stat(follow_symlinks=False)
            if st.st_file_attributes & REPARSE:
                continue
            if e.is_dir(follow_symlinks=False):
                stack.append(e.path)
            else:
                set_times(e.path, t)
    for d in reversed(dirs):
        set_times(d, t)


def sh(*cmd, cwd=None):
    r = subprocess.run(list(cmd), cwd=cwd, capture_output=True, text=True, errors='replace', creationflags=NOWIN)
    if r.returncode:
        raise RuntimeError('%s -> %s %s' % (cmd, r.stdout, r.stderr))
    return r.stdout


def git(cwd, *a):
    return sh('git', '-c', 'user.name=t', '-c', 'user.email=t@t', '-c', 'core.autocrlf=false', *a, cwd=cwd)


def write(path, data=b'x'):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data if isinstance(data, bytes) else data.encode())


class World:
    """One fake checkout: a git repo on branch main with out\\, tools\\, a projects folder for transcripts."""

    def __init__(self, base, name):
        self.root = os.path.join(base, name)
        self.proj = os.path.join(base, name + '_projects')
        os.makedirs(self.root)
        git(self.root, 'init', '-q', '-b', 'main')
        write(os.path.join(self.root, 'README.md'), 'r')
        write(os.path.join(self.root, 'wwe2k14_manifest.toml'), 'a = 1\n')       # a BUILD_SETUP file, tracked
        git(self.root, 'add', '.')
        git(self.root, 'commit', '-q', '-m', 'init')
        with open(os.path.join(self.root, '.git', 'info', 'exclude'), 'a') as f:      # not "real edits": pad files, the junction, .claude
            f.write('pad.bin\nlink\n.claude/\n')
        os.makedirs(os.path.join(self.root, 'out', 'tidy'))
        write(os.path.join(self.root, 'out', 'tidy', 'enabled'), 'x')
        self.live = []

    def p(self, *a):
        return os.path.join(self.root, *a)

    def temp(self, rel, hours, size=1000):
        write(self.p('out', *rel.split('/'), 'data.bin'), b'x' * size)
        age_tree(self.p('out', *rel.split('/')), time.time() - hours * H)

    def wt(self, name, kind='merged', hours=3.0, transcript_h=None, size=1000, under=('.claude', 'worktrees')):
        """An agent worktree. kind: merged (HEAD in main), unmerged (own commit on a branch), detached (commit on no branch)."""
        path = self.p(*under, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if kind == 'detached':
            git(self.root, 'worktree', 'add', '-q', '--detach', path)
        else:
            git(self.root, 'worktree', 'add', '-q', '-b', 'br_' + name.replace('agent-', ''), path)
        if kind in ('unmerged', 'detached', 'mergedcommit'):
            write(os.path.join(path, 'work_' + name + '.txt'), name)
            git(path, 'add', '.')
            git(path, 'commit', '-q', '-m', 'work')
            if kind == 'mergedcommit':
                git(self.root, 'merge', '-q', 'br_' + name.replace('agent-', ''))
        write(os.path.join(path, 'pad.bin'), b'x' * size)      # ignored by git (info/exclude): sizes the worktree
        return path

    def meta(self, name, finished, wt=None, parent='sess1', hours=0.0):
        """Claude Code's subagents\\agent-<id>.meta.json: worktreePath, and "finished": true once the agent ends."""
        f = os.path.join(self.proj, 'proj1', parent, 'subagents', name + '.meta.json')
        d = {'agentType': 'claude', 'worktreePath': wt or self.p('.claude', 'worktrees', name)}
        if finished:
            d['finished'] = True
        write(f, json.dumps(d))
        set_times(f, time.time() - hours * H)

    def transcript(self, name, hours, parent='sess1'):
        f = os.path.join(self.proj, 'proj1', parent, 'subagents', name + '.jsonl')
        write(f, '{}')
        set_times(f, time.time() - hours * H)

    def env(self):
        e = dict(os.environ)
        e.update(TIDY_TEST_ROOT=self.root, TIDY_TEST_LIVE=json.dumps(self.live), TIDY_TEST_PROJECTS=self.proj)
        return e

    def run(self, script, *args):
        r = subprocess.run([sys.executable, script, *args], env=self.env(), capture_output=True, text=True, errors='replace',
                           creationflags=NOWIN, timeout=300)
        return r.stdout + r.stderr

    def carry(self):
        try:
            return json.load(open(self.p('out', 'tidy', 'carry.json')))
        except OSError:
            return []

    def exists(self, *a):
        return os.path.exists(self.p(*a))


class Results:
    def __init__(self):
        self.rows = []

    def check(self, name, ok, detail=''):
        self.rows.append((name, bool(ok), detail))
        print('  %s  %s%s' % ('PASS' if ok else 'FAIL', name, ('   <- ' + detail) if (detail and not ok) else ''))

    def failed(self):
        return [n for n, ok, _ in self.rows if not ok]


def junction(link, target):
    sh('cmd', '/c', 'mklink', '/J', link, target)


def rm_world(w):
    for dp, dn, fn in os.walk(w.root):
        for d in list(dn):
            q = os.path.join(dp, d)
            if os.lstat(q).st_file_attributes & REPARSE:
                os.rmdir(q)
                dn.remove(d)

    def onerr(fn, path, exc):
        os.chmod(path, stat.S_IWRITE)
        fn(path)
    shutil.rmtree(w.root, onerror=onerr)
    shutil.rmtree(w.proj, ignore_errors=True)


def retired():
    # S103 TDS (docs\research\s103\TDS.md, 2026-11-18): this suite tests the pre-TIDY2 time-based rules (32eba5bd5 / 146f40dd9 removed them on
    # purpose: CLAUDE.md "out\ is scratch"); its 29 failures were all stale expectations, none a tidy regression. The live suites are
    # tools\harness\tidy_out_selftest.py, tidy_now_selftest.py and tidy_k1_selftest.py (run by path). This file stays as
    # the helper library tidy_now_selftest imports.
    print('tidy_selftest: retired (TIDY2): run tidy_out_selftest / tidy_now_selftest / tidy_k1_selftest')
    return 0


if __name__ == '__main__':
    sys.exit(retired())
