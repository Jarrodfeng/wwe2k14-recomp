"""Selftest for tools\\harness\\tidy.py (lane 154, Session 101). 0 game runs; builds fake trees under TIDY_TEST_ROOT.

  py tools\\harness\\tidy_selftest.py             the plants (every case below), against the tidy next to this file
  py tools\\harness\\tidy_selftest.py --mutants   every rule removed in turn (a mutated copy of tidy.py): each must make the suite fail
  py tools\\harness\\tidy_selftest.py --refind    the 2026-11-12 case (a live session started 23 h ago, temporary output 10 h old,
                                                an --until hold made before the session, gate inputs without KEEP): OLD tidy
                                                (git rev OLD_REV) vs the new one, with numbers

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
    """One fake checkout: a git repo on branch main with out\\, tools\\gate\\lanes, a projects folder for transcripts."""

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


GATE_LANE = {'owner': 'selftest', 'checks': [{'name': 'g.x', 'cmd': ['py', 'tools/x.py', 'out/gate/brock/capture'],
                                              'needs': ['out/gate/wm30/capture/manifest.json', 'game_data/extracted']}]}


# --------------------------------------------------------------------------------------------- the plants
def suite_agents(script, base, R):
    print('agent worktrees, rolling expiry, gate inputs (live session since 23 h ago, hourly pass)')
    w = World(base, 'agents')
    now = time.time()
    w.live = [['sess1', now - 23 * H, now - 60]]
    write(w.p('tools', 'gate', 'lanes', 'g.json'), json.dumps(GATE_LANE))
    ext = os.path.join(base, 'agents_ext')       # a junction target OUTSIDE the tree: must survive
    write(os.path.join(ext, 'precious.txt'), 'keep me')
    write(os.path.join(ext, 'sub', 'precious2.txt'), 'keep me too')
    ext_t = time.time() - 50 * H
    age_tree(ext, ext_t)

    live1 = w.wt('agent-live1')            # fresh transcript, old files
    live2 = w.wt('agent-live2')            # old transcript, one fresh file
    done = w.wt('agent-done')
    scratch = w.wt('agent-scratch')
    junc = w.wt('agent-junc')
    det = w.wt('agent-detached', 'detached')
    unm = w.wt('agent-unmerged', 'unmerged')
    unm_old = w.wt('agent-unmergedold', 'unmerged')
    mc = w.wt('agent-mergedcommit', 'mergedcommit')
    keep = w.wt('agent-keep')
    held = w.wt('agent-held')
    write(os.path.join(scratch, 'scratch', 'notes.txt'), 'scratch work')
    write(os.path.join(keep, 'KEEP'), 'x')
    junction(os.path.join(junc, 'link'), ext)
    # ages: everything 3 h old unless said otherwise
    for path, hrs in [(live1, 5), (live2, 5), (done, 3), (scratch, 3), (junc, 3), (det, 30), (unm, 3), (unm_old, 30), (mc, 3), (keep, 3), (held, 3)]:
        age_tree(path, time.time() - hrs * H)
    set_times(os.path.join(live2, 'pad.bin'), time.time() - 0.5 * H)     # git-ignored, so only the 2 h rule can keep it
    w.transcript('agent-live1', 10 / 60)
    w.transcript('agent-live2', 10)
    w.transcript('agent-done', 3)
    # not worktrees: a plain folder named like one, a file
    write(w.p('.claude', 'worktrees', 'agent-notregistered', 'f.txt'), 'x')
    age_tree(w.p('.claude', 'worktrees', 'agent-notregistered'), time.time() - 90 * H)
    write(w.p('.claude', 'worktrees', 'notes.txt'), 'x')
    set_times(w.p('.claude', 'worktrees', 'notes.txt'), time.time() - 90 * H)
    with open(w.p('out', 'tidy', 'carry.json'), 'w') as f:
        json.dump([{'path': '.claude/worktrees/agent-held', 'why': 'x', 'added': now - 5 * H, 'expires': now + 5 * 86400}], f)
    # temp output
    w.temp('tmp7h', 7)
    w.temp('tmp10h', 10)
    w.temp('tmp1h', 1)
    w.temp('gate/brock/capture', 10)
    w.temp('gate/wm30/capture', 10)
    w.temp('gate/other', 10)
    age_tree(w.p('out', 'gate'), time.time() - 10 * H)

    plan_out = w.run(script, '--plan')
    plans = sorted(f for f in os.listdir(w.p('out', 'tidy')) if f.startswith('plan_') and f.endswith('.json'))
    plan = json.load(open(w.p('out', 'tidy', plans[-1]))) if plans else {'worktrees': [], 'items': []}
    planned = {os.path.basename(x['path']) for x in plan['worktrees']}
    R.check('plan removes exactly the finished clean agent worktrees',
            planned == {'agent-done', 'agent-detached', 'agent-unmergedold', 'agent-mergedcommit'}, str(sorted(planned)))
    R.check('plan summary reports the merged / unmerged split', 'not merged' in plan_out and 'agent worktrees' in plan_out, plan_out[-400:])
    w.run(script, '--auto', 'hourly')

    R.check('live agent worktree (fresh transcript) kept', w.exists('.claude', 'worktrees', 'agent-live1', 'pad.bin'))
    R.check('live agent worktree (fresh file, old transcript) kept', w.exists('.claude', 'worktrees', 'agent-live2', 'pad.bin'))
    R.check('finished clean merged agent worktree removed', not w.exists('.claude', 'worktrees', 'agent-done'))
    R.check('finished one whose transcript is 3 h old (past the 2 h rule) removed', not w.exists('.claude', 'worktrees', 'agent-done'))
    R.check('finished one with an untracked scratch folder kept and reported',
            w.exists('.claude', 'worktrees', 'agent-scratch', 'scratch', 'notes.txt'))
    R.check('worktree with a junction left whole', w.exists('.claude', 'worktrees', 'agent-junc', 'pad.bin')
            and w.exists('.claude', 'worktrees', 'agent-junc', 'link'))
    R.check('junction target untouched (files and folders survive)',
            os.path.exists(os.path.join(ext, 'precious.txt')) and os.path.exists(os.path.join(ext, 'sub', 'precious2.txt')))
    refs = git(w.root, 'for-each-ref', 'refs/tidy').strip()
    R.check('worktree whose HEAD is on no branch: saved under refs/tidy, then removed',
            'refs/tidy/agent-detached' in refs and not w.exists('.claude', 'worktrees', 'agent-detached'), refs)
    R.check('unmerged idle worktree, 3 h old, kept', w.exists('.claude', 'worktrees', 'agent-unmerged', 'pad.bin'))
    R.check('unmerged worktree 30 h idle removed, its branch stays',
            not w.exists('.claude', 'worktrees', 'agent-unmergedold') and 'br_unmergedold' in git(w.root, 'branch', '--list'))
    R.check('merged worktree (own commit merged into the session branch), 3 h old, removed',
            not w.exists('.claude', 'worktrees', 'agent-mergedcommit'))
    R.check('worktree with a KEEP file kept', w.exists('.claude', 'worktrees', 'agent-keep', 'pad.bin'))
    R.check('held worktree kept', w.exists('.claude', 'worktrees', 'agent-held', 'pad.bin'))
    R.check('unregistered agent-* folder and loose files in .claude\\worktrees untouched',
            w.exists('.claude', 'worktrees', 'agent-notregistered', 'f.txt') and w.exists('.claude', 'worktrees', 'notes.txt'))
    R.check('gate input folder (command argument) kept', w.exists('out', 'gate', 'brock', 'capture', 'data.bin'))
    R.check('gate input folder (needs a file in it) kept', w.exists('out', 'gate', 'wm30', 'capture', 'data.bin'))
    R.check('7 h old temp folder under a live session expired', not w.exists('out', 'tmp7h'))
    R.check('10 h old temp folder under a live session expired', not w.exists('out', 'tmp10h'))
    R.check('1 h old temp folder kept', w.exists('out', 'tmp1h', 'data.bin'))
    R.check('unrelated old folder next to the gate inputs expired', not w.exists('out', 'gate', 'other'))
    rm_world(w)
    shutil.rmtree(ext, ignore_errors=True)


def suite_holds(script, base, R):
    print('holds: --until / --days survive --auto end, plain holds are released, legacy "Jarrod" entries migrate')
    w = World(base, 'holds')
    now = time.time()
    w.live = [['sess1', now - 23 * H, now - 60]]
    for n in ('held_until', 'held_plain', 'held_legacy', 'cli_until', 'cli_plain'):
        w.temp(n, 10)
    carry = [
        {'path': 'out/held_until', 'why': 'x', 'added': now - 30 * H, 'expires': now + 4 * 86400, 'fixed': True},
        {'path': 'out/held_plain', 'why': 'x', 'added': now - 30 * H, 'expires': now + 4 * 86400, 'fixed': False},
        {'path': 'out/held_legacy', 'why': 'Session 99 builder work, Jarrod: keep', 'added': now - 30 * H, 'expires': now + 4 * 86400},
    ]
    with open(w.p('out', 'tidy', 'carry.json'), 'w') as f:
        json.dump(carry, f)
    out = w.run(script, '--hold', 'out/cli_until', 'by cli', '--until', '2099-01-01')
    R.check('--hold --until is accepted', 'held until' in out and 'never --auto end' in out, out)
    c = {x['path']: x for x in w.carry()}
    R.check('--until entry is fixed with its own expiry', c.get('out/cli_until', {}).get('fixed') is True
            and c['out/cli_until']['expires'] > now + 365 * 86400)
    w.run(script, '--hold', 'out/cli_days', 'by cli', '--days', '3')
    c = {x['path']: x for x in w.carry()}
    R.check('--days entry is fixed, expires in 3 days', c.get('out/cli_days', {}).get('fixed') is True
            and abs(c['out/cli_days']['expires'] - (time.time() + 3 * 86400)) < 120)
    w.run(script, '--hold', 'out/cli_plain', 'by cli')
    c = {x['path']: x for x in w.carry()}
    R.check('plain --hold stays a plain hold', c.get('out/cli_plain', {}).get('fixed') is False)
    # make the plain CLI hold older than the session so --auto end releases it
    rows = w.carry()
    for r in rows:
        if r['path'] == 'out/cli_plain':
            r['added'] = now - 30 * H
    with open(w.p('out', 'tidy', 'carry.json'), 'w') as f:
        json.dump(rows, f)
    w.run(script, '--auto', 'end', '--session', 'sess1')
    paths = {x['path'] for x in w.carry()}
    R.check('--until hold (made before the session) survives --auto end', w.exists('out', 'held_until', 'data.bin') and 'out/held_until' in paths)
    R.check('--until hold made by the CLI survives --auto end', 'out/cli_until' in paths and w.exists('out', 'cli_until', 'data.bin'))
    R.check('legacy "Jarrod" hold migrated to its own expiry and survives', w.exists('out', 'held_legacy', 'data.bin') and 'out/held_legacy' in paths)
    R.check('plain hold made before the session is released by --auto end', not w.exists('out', 'held_plain') and 'out/held_plain' not in paths)
    R.check('plain CLI hold made before the session is released by --auto end', not w.exists('out', 'cli_plain') and 'out/cli_plain' not in paths)
    rm_world(w)


def suite_low(script, base, R):
    print('low-space pass reaches finished agent worktrees, biggest first, stops at the target')
    w = World(base, 'low')
    big = w.wt('agent-big', size=3_000_000)
    small = w.wt('agent-small', size=2_000)
    unm = w.wt('agent-unm', 'unmerged', size=5_000_000)
    for path in (big, small, unm):
        age_tree(path, time.time() - 3 * H)
    runner = os.path.join(base, 'low_runner.py')
    write(runner, '''
import importlib.util, os, sys
spec = importlib.util.spec_from_file_location('tidy_under_test', sys.argv[1])
T = importlib.util.module_from_spec(spec); spec.loader.exec_module(T)
big = os.path.join(T.REPO, '.claude', 'worktrees', 'agent-big')
T.GB = 1e6      # so the batch sizing (deficit in GB) is in MB here
T.free_gb = lambda: 74.999 if os.path.exists(big) else 80.0
print('free after', T.low_pass([], set()))
''')
    r = subprocess.run([sys.executable, runner, script], env=w.env(), capture_output=True, text=True, errors='replace', creationflags=NOWIN)
    R.check('biggest finished agent worktree removed first', not w.exists('.claude', 'worktrees', 'agent-big'), r.stdout + r.stderr)
    R.check('stops at the target: the smaller one stays', w.exists('.claude', 'worktrees', 'agent-small', 'pad.bin'))
    R.check('unmerged 3 h worktree not touched by the low pass', w.exists('.claude', 'worktrees', 'agent-unm', 'pad.bin'))
    rm_world(w)



def suite_morning(script, base, R):
    print('S1: last night\'s output survives the morning session; output made during the session still expires at 6 h')
    w = World(base, 'morning')
    now = time.time()
    w.live = [['sess1', now - 8 * H, now - 60]]          # this session began 8 h ago
    w.temp('night10h', 10)        # made before the session began
    w.temp('night30h', 30)        # older than 24 h: goes
    w.temp('mine7h', 7)           # made during the session, older than 6 h: goes
    w.temp('mine1h', 1)
    w.run(script, '--auto', 'hourly')
    R.check('output made BEFORE the live session (10 h old) survives the morning pass', w.exists('out', 'night10h', 'data.bin'))
    R.check('30 h old output expires', not w.exists('out', 'night30h'))
    R.check('output made during the session and older than 6 h expires', not w.exists('out', 'mine7h'))
    R.check('output made during the session and 1 h old stays', w.exists('out', 'mine1h', 'data.bin'))
    rm_world(w)


def suite_inputs(script, base, R):
    print('S3: capture folders, runs, tools-py names, depth-2 gate names; never delete part of a folder')
    w = World(base, 'inputs')
    now = time.time()
    w.live = [['sess1', now - 23 * H, now - 60]]
    write(w.p('tools', 'gate', 'lanes', 'g.json'), json.dumps({'checks': [{'name': 'x', 'cmd': ['py', 'a.py'], 'needs': ['out/post_plan_cache']}]}))
    write(w.p('tools', 'screens', 'screens_check.py'),
          "import os\nP = os.path.join(ROOT, 'out', 'pieces', 'ours')\nQ = r'out\\viaquote\\inner'\nlayout = 'layout/notme'\nS = 'out/stale_tool'\n")
    w.temp('cap7h', 7)
    write(w.p('out', 'cap7h', 'images.bin'), b'i')
    w.temp('s101/runs/run7h', 7)
    w.temp('post_plan_cache', 7)
    w.temp('pieces/ours', 7)
    w.temp('viaquote/inner', 7)
    w.temp('layout/notme', 7)
    w.temp('plain7h', 7)
    w.temp('stale_tool', 24 * 10)
    age_tree(w.p('out', 'stale_tool'), time.time() - 24 * 10 * H)
    age_tree(w.p('out', 'cap7h'), time.time() - 7 * H)
    for d in ('pieces', 'viaquote', 'layout'):
        age_tree(w.p('out', d), time.time() - 7 * H)
    # a capture whose images.bin is open elsewhere (a tool reads it): nothing else in the folder may go
    for n in ('a.bin', 'z.bin'):
        write(w.p('out', 'lockcap', n), b'x')
    write(w.p('out', 'lockcap', 'images.bin'), b'i')
    age_tree(w.p('out', 'lockcap'), time.time() - 30 * H)
    # a capture folder with a fresh images.bin next to old frames: it does not go whole, so no file in it may go one by one
    write(w.p('out', 'capmix', 'frame1.bin'), b'x')
    write(w.p('out', 'capmix', 'images.bin'), b'i')
    age_tree(w.p('out', 'capmix'), time.time() - 30 * H)
    set_times(w.p('out', 'capmix', 'images.bin'), time.time() - 1 * H)
    held = open(w.p('out', 'lockcap', 'images.bin'), 'rb')
    try:
        w.run(script, '--auto', 'hourly')
    finally:
        held.close()
    R.check('lane 294: capture folder (images.bin) 7 h old goes (6 h rule for unpacked capture copies)', not w.exists('out', 'cap7h', 'data.bin'))
    R.check('everything under out\\s101\\runs\\* kept at 7 h', w.exists('out', 's101', 'runs', 'run7h', 'data.bin'))
    R.check('depth-2 gate name (out/post_plan_cache) kept', w.exists('out', 'post_plan_cache', 'data.bin'))
    R.check('folder named by os.path.join(..., "out", "pieces", "ours") in tools/*.py kept', w.exists('out', 'pieces', 'ours', 'data.bin'))
    R.check("folder named by r'out\\viaquote\\inner' in tools/*.py kept", w.exists('out', 'viaquote', 'inner', 'data.bin'))
    R.check('"layout/notme" is not read as out/notme: the folder expires', not w.exists('out', 'layout', 'notme'))
    R.check('control: a plain 7 h folder expires', not w.exists('out', 'plain7h'))
    R.check('capture folder with a fresh images.bin: old frames next to it are kept (not deleted one by one)',
            w.exists('out', 'capmix', 'frame1.bin') and w.exists('out', 'capmix', 'images.bin'))
    R.check('a folder only a tool names, untouched for 10 days, is not protected', not w.exists('out', 'stale_tool'))
    R.check('capture with an open images.bin: the rest of the folder stays (never part of a folder)',
            w.exists('out', 'lockcap', 'a.bin') and w.exists('out', 'lockcap', 'z.bin') and w.exists('out', 'lockcap', 'images.bin'))
    rm_world(w)


def suite_liveness(script, base, R):
    print('S2 / S4 / N2 / N3: builder edits, Claude Code meta signal, worktree locks, out\\wt unmerged')
    w = World(base, 'liveness')
    now = time.time()
    w.live = [['sess1', now - 23 * H, now - 60]]
    names = ['agent-run1', 'agent-fin1', 'agent-resumed', 'agent-mapped', 'agent-oldsess', 'agent-manifest', 'agent-locked', 'agent-orphan']
    paths = {n: w.wt(n) for n in names}
    phantom = w.wt('agent-phantom')
    git(w.root, 'worktree', 'lock', paths['agent-locked'])
    git(w.root, 'worktree', 'lock', phantom)
    write(os.path.join(paths['agent-manifest'], 'wwe2k14_manifest.toml'), 'a = 2\n')       # uncommitted builder edit
    um = w.wt('um', 'unmerged', under=('out', 'wt'))
    mg = w.wt('mg', 'merged', under=('out', 'wt'))
    for path in list(paths.values()) + [um, mg]:
        age_tree(path, time.time() - 7 * H)
    # liveness signals: transcripts 5 h old everywhere, so only the meta can keep them
    w.transcript('agent-run1', 5)
    w.meta('agent-run1', False, hours=5)                                                  # running: no finished key
    w.transcript('agent-fin1', 5)
    w.meta('agent-fin1', True, hours=5)                                                   # ended
    w.transcript('agent-resumed', 5)
    w.meta('agent-resumed', True, hours=5)
    w.meta('agent-resumed', False, hours=5)                                               # resumed: meta rewritten without finished
    w.transcript('agent-other', 5)
    w.meta('agent-other', False, wt=paths['agent-mapped'], hours=5)                       # mapped by worktreePath, not by name
    w.transcript('agent-oldsess', 5, parent='deadsess')
    w.meta('agent-oldsess', False, parent='deadsess', hours=5)                            # parent session is not live
    w.transcript('agent-manifest', 5)
    w.meta('agent-manifest', True, hours=5)
    w.transcript('agent-locked', 5)
    w.meta('agent-locked', True, hours=5)
    age_tree(paths['agent-orphan'], time.time() - 30 * H)
    w.transcript('agent-orphan', 30)
    w.meta('agent-orphan', False, hours=30)                                               # killed 30 h ago: no finished flag, ever
    shutil.rmtree(phantom, onerror=lambda f, p_, e: (os.chmod(p_, stat.S_IWRITE), f(p_)))
    w.run(script, '--plan', '--ahead', '12')                                              # forecast: the live session goes on
    fc = sorted(f for f in os.listdir(w.p('out', 'tidy')) if f.endswith('_ahead12.json'))
    planned = {os.path.basename(x['path']) for x in json.load(open(w.p('out', 'tidy', fc[-1])))['worktrees']} if fc else None
    R.check('--plan --ahead: running agents (live session ids from real time) are not listed as removed',
            planned is not None and 'agent-fin1' in planned and not ({'agent-run1', 'agent-resumed', 'agent-mapped'} & planned), str(planned))
    w.run(script, '--auto', 'hourly')
    R.check('Q3: a meta without finished, 30 h old (killed agent), no longer keeps its worktree', not w.exists('.claude', 'worktrees', 'agent-orphan'))
    ex = lambda n: w.exists('.claude', 'worktrees', n)
    R.check('S4: meta without "finished" + 5 h idle is kept (agent still running)', ex('agent-run1'))
    R.check('S4: a finished agent (meta finished:true) 5 h idle is removed', not ex('agent-fin1'))
    R.check('S4: a resumed agent (meta rewritten without finished) is kept', ex('agent-resumed'))
    R.check('S4: mapped by worktreePath (agent id differs from the folder name)', ex('agent-mapped'))
    R.check('S4: a meta without finished whose parent session is not live does not keep it', not ex('agent-oldsess'))
    R.check('S2: an uncommitted wwe2k14_manifest.toml edit in an agent worktree is a real edit (kept)',
            ex('agent-manifest') and open(os.path.join(paths['agent-manifest'], 'wwe2k14_manifest.toml')).read() == 'a = 2\n')
    listing = git(w.root, 'worktree', 'list', '--porcelain')
    R.check('N2: a locked finished agent worktree is removed and unregistered', not ex('agent-locked') and 'agent-locked' not in listing, listing)
    R.check('N2: a phantom entry (locked, folder gone) is unlocked and pruned', 'agent-phantom' not in listing, listing)
    R.check('N3: out\\wt worktree whose branch is not merged, 7 h old, kept', w.exists('out', 'wt', 'um', 'pad.bin'))
    R.check('N3: out\\wt worktree already merged, 7 h old, removed', not w.exists('out', 'wt', 'mg'))
    rm_world(w)


def suite_inproc(script, base, R):
    print('in-process: the agent re-read right before deleting (S4), a junction that appears under a folder (N1)')
    w = World(base, 'inproc')
    now = time.time()
    w.live = [['sess1', now - 23 * H, now - 60]]
    path = w.wt('agent-wr')
    age_tree(path, time.time() - 5 * H)
    w.transcript('agent-wr', 5)
    w.meta('agent-wr', True, hours=5)
    ext = os.path.join(base, 'inproc_ext')
    write(os.path.join(ext, 'precious.txt'), 'keep me')
    age_tree(ext, time.time() - 50 * H)
    jdir = w.p('out', 'jdir')
    junction(jdir, ext)
    runner = os.path.join(base, 'inproc_runner.py')
    write(runner, r"""
import importlib.util, json, os, sys, time
spec = importlib.util.spec_from_file_location('tidy_under_test', sys.argv[1])
T = importlib.util.module_from_spec(spec); spec.loader.exec_module(T)
now = time.time()
age, strict = T.make_ages(now, T.sessions(now))
wts = T.check_worktrees(age, set(), now)
w = [x for x in wts if x['path'].endswith('agent-wr')][0]
state_before = w['state']
meta = sys.argv[2]
with open(meta, 'w') as f:
    json.dump({'worktreePath': w['path']}, f)            # the agent is resumed after the plan was made
d = T.Deleter(now)
went = d.worktree(w)
subs = d.clear_dir(sys.argv[3])
d.clear_dir(sys.argv[4])                                  # a folder with .git, a.txt and an open z.bin
T.Deleter.unlock_prune([{'repo': T.REPO, 'path': sys.argv[5]}])      # a live, existing worktree: keeps its lock
print(json.dumps({'state_before': state_before, 'went': went, 'subs': subs}))
""")
    gd = w.p('out', 'gitdir')
    for n in ('.git', 'a.txt', 'z.bin'):
        write(os.path.join(gd, n), b'x')
    age_tree(gd, time.time() - 30 * H)
    live_wt = w.wt('agent-lockedlive')
    git(w.root, 'worktree', 'lock', live_wt)
    age_tree(live_wt, time.time() - 5 * H)
    meta = os.path.join(w.proj, 'proj1', 'sess1', 'subagents', 'agent-wr.meta.json')
    held = open(os.path.join(gd, 'z.bin'), 'rb')
    try:
        r = subprocess.run([sys.executable, runner, script, meta, jdir, gd, live_wt], env=w.env(), capture_output=True, text=True, errors='replace', creationflags=NOWIN)
    finally:
        held.close()
    try:
        out = json.loads(r.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        out = {'state_before': '?', 'went': None, 'subs': None}
    R.check('S4 setup: the finished agent worktree was removable at plan time', out['state_before'] == 'remove', r.stdout + r.stderr)
    R.check('S4: an agent resumed after the plan (meta rewritten) is not deleted', out['went'] is False and os.path.exists(os.path.join(path, 'pad.bin')))
    R.check('N1: clear_dir on a junction that appeared stops there (target untouched)',
            out['subs'] == [] and os.path.exists(os.path.join(ext, 'precious.txt')))
    R.check('.git is deleted LAST: with an open file in the folder, .git and the open file stay',
            os.path.exists(os.path.join(gd, '.git')) and os.path.exists(os.path.join(gd, 'z.bin')) and not os.path.exists(os.path.join(gd, 'a.txt')))
    listing = git(w.root, 'worktree', 'list', '--porcelain')
    blk = next((b for b in listing.split('\n\n') if b.replace('\\', '/').split('\n')[0].endswith('agent-lockedlive')), '')
    R.check('A: unlock_prune leaves the lock on a worktree whose folder exists', any(l.strip() == 'locked' or l.startswith('locked ') for l in blk.splitlines()), blk)
    os.rmdir(jdir)
    rm_world(w)
    shutil.rmtree(ext, ignore_errors=True)


def suite_cli(script, base, R):
    print('CLI: holds on agent worktrees (N4), --days 0, --until alone, --release and --all (N6), TIDY_REPO (N7)')
    w = World(base, 'cli')
    w.wt('agent-held')
    out = w.run(script, '--hold', '.claude/worktrees/agent-held', 'builder work', '--days', '2')
    R.check('N4: --hold accepts an agent worktree', 'held until' in out and any(c['path'] == '.claude/worktrees/agent-held' for c in w.carry()), out)
    out = w.run(script, '--hold', 'out/zero', 'x', '--days', '0')
    R.check('N6: --days 0 is an error', 'above 0' in out and not any(c['path'] == 'out/zero' for c in w.carry()), out)
    out = w.run(script, '--until', '2099-01-01')
    R.check('N6: --until without --hold is an error', 'only go with --hold' in out, out)
    w.run(script, '--hold', 'out/plain', 'p')
    w.run(script, '--release')
    paths = {c['path'] for c in w.carry()}
    R.check('N6: --release with no path drops plain holds and keeps --days holds', 'out/plain' not in paths and '.claude/worktrees/agent-held' in paths, str(paths))
    w.run(script, '--release', '--all')
    R.check('N6: --release --all drops the rest', not w.carry())
    env = dict(os.environ)
    env['TIDY_REPO'] = w.root
    env.pop('TIDY_TEST_ROOT', None)
    r = subprocess.run([sys.executable, script, '--auto', 'hourly', '--dry'], env=env, capture_output=True, text=True, creationflags=NOWIN)
    R.check('N7: TIDY_REPO is refused for anything but --plan', r.returncode == 2 and 'for --plan only' in r.stdout, r.stdout + r.stderr)
    rm_world(w)


def suite_s294(script, base, R):
    print('lane 294: run folders (newest per route), KEEP expiry, capture copies, budget line')
    w = World(base, 's294')
    now = time.time()
    write(w.p('tools', 'x.py'), '# a tool\n')
    write(w.p('tools', 'x_check.py'), '# a check\n')
    write(w.p('tools', 'pfx_check.py'), "G = 'out/s9/runs/20261110_05*/case_*.zip'\nH = 'out/s9/runs/zzz_*/case_*.zip'\n")     # S4: glob prefixes
    write(w.p('tools', 'gate', 'lanes', 'case.json'), '{"checks": []}')                                                          # a lane called "case"
    write(w.p('STATUS.md'), 'the runs are in out/s9/runs and out\\s9\\runs\n')                                                    # S1: a doc naming a namespace
    write(w.p('out', 'bench', 'userdata_frozen', 'a.txt'), 'x')
    write(w.p('tools', 'ring_check.py'), "A = 'out/oldcase/case_a/capture'\nB = 'out/gone/case_b/capture'\n")      # a check naming a capture 10 days old and one that is gone
    w.temp('oldcase/case_a/capture', 240)
    ln = {'owner': 'selftest', 'checks': [{'name': 'g.i', 'cmd': ['py', 'tools/x.py', 'out/s9/runs/20261110_060000_routeI']}]}
    write(w.p('tools', 'gate', 'lanes', 'g.json'), json.dumps(ln))
    runs = 's9/runs/'
    names = ['20261110_100000_routeA', '20261110_110000_routeA',          # old + newest of A
             '20261110_090000_routeB',                                    # only one of B
             '20261110_060000_routeD', '20261110_120000_routeD',          # old D carries a tool-citing KEEP
             '20261110_060000_routeE', '20261110_120000_routeE',          # old E: fresh bare KEEP
             '20261110_060000_routeF', '20261110_120000_routeF',          # old F: bare KEEP 9 days old
             '20261110_060000_routeG', '20261110_120000_routeG',          # old G: KEEP until 2099
             '20261110_060000_routeH', '20261110_120000_routeH',          # old H: KEEP until 2020
             '20261110_060000_routeI', '20261110_120000_routeI',          # old I: named by a gate lane
             '20261110_060000_routeJ', '20261110_120000_routeJ',          # old J: empty KEEP, 9 days old
             '20261110_100000_routeK', '20261110_130000_routeK', '20261110_130001_routeK',     # B1: case dir + record dir one second apart; one 3 h older
             '20261110_060000_routeL', '20261110_120000_routeL',          # old L: a KEEP whose words are lane-name substrings
             '20261110_050000_routeM', '20261110_120000_routeM']          # old M: named by a check's glob prefix
    for n in names:
        w.temp(runs + n, 5)

    def kt(n, text, days=0):
        f = w.p('out', 's9', 'runs', n, 'KEEP')
        write(f, text)
        set_times(f, now - days * 86400 - 5 * H)
        set_times(os.path.dirname(f), now - 5 * H)
    kt('20261110_060000_routeD', 'baseline for tools/x_check.py', 9)
    kt('20261110_060000_routeL', 'judgement post case scene core wide', 9)
    kt('20261110_060000_routeE', 'misc')
    kt('20261110_060000_routeF', 'misc', 9)
    kt('20261110_060000_routeG', 'the clip, until 2099-01-01')
    kt('20261110_060000_routeH', 'the clip, until 2020-01-01')
    kt('20261110_060000_routeJ', '', 9)
    # an unpacked capture next to the runs (a copy) and one inside a kept run
    for n, hrs in (('cap7', 7), ('cap3', 3)):
        write(w.p('out', 'cap_' + n, 'images.bin'), b'x' * 1000)
        write(w.p('out', 'cap_' + n, 'manifest.json'), b'{}')
        age_tree(w.p('out', 'cap_' + n), now - hrs * H)
    write(w.p('out', 's9', 'runs', '20261110_060000_routeD', 'cap', 'images.bin'), b'x' * 1000)
    age_tree(w.p('out', 's9', 'runs', '20261110_060000_routeD', 'cap'), now - 30 * H)
    set_times(w.p('out', 's9', 'runs', '20261110_060000_routeD'), now - 5 * H)
    plan_out = w.run(script, '--plan')
    R.check('s294: --plan prints the size budget line and top 5', 'size budget:' in plan_out and 'top 5:' in plan_out, plan_out[:600])
    pt = sorted(f for f in os.listdir(w.p('out', 'tidy')) if f.startswith('plan_') and f.endswith('.txt'))
    full = open(w.p('out', 'tidy', pt[-1]), encoding='utf-8').read() if pt else ''
    R.check('s294: --plan lists every KEEP folder with its state',
            'KEEP folders (' in full and 'NO HOLD' in full and 'HOLDS' in full and 'bare: 7-day hold' in full, full[:1500])
    R.check('s294: --plan prints MISSING GATE INPUT for a capture a check names that is gone',
            'MISSING GATE INPUT: out/gone/case_b/capture' in plan_out and 'MISSING GATE INPUT: out/oldcase' not in plan_out, plan_out[:900])
    g = w.run(script, '--guard', w.p('out', 'oldcase'), w.p('out', 'oldcase', 'case_a', 'capture'), w.p('out', 'zzz_free'))
    R.check('s294: --guard refuses a gate-named capture and its parent, and passes an unrelated path',
            ('REFUSE ' + w.p('out', 'oldcase') + ' ') in g and ('REFUSE ' + w.p('out', 'oldcase', 'case_a', 'capture')) in g and 'zzz_free' not in g.replace('2 of 3', '') and '2 of 3' in g, g)
    w.run(script, '--auto', 'hourly')
    R.check('s294: a capture a check script names survives although untouched for 10 days', w.exists('out', 'oldcase', 'case_a', 'capture', 'data.bin'))

    def ex(n):
        return w.exists('out', 's9', 'runs', n, 'data.bin')
    R.check('s294: old run of a route removed after 2 h', not ex('20261110_100000_routeA'))
    R.check('s294: newest run of a route kept', ex('20261110_110000_routeA') and ex('20261110_120000_routeD'))
    R.check('s294: the only run of a route kept', ex('20261110_090000_routeB'))
    R.check('s294: KEEP that cites an existing tool kept', ex('20261110_060000_routeD'))
    R.check('s294: bare KEEP younger than 7 days still holds', ex('20261110_060000_routeE'))
    R.check('s294: bare KEEP aged out (9 days): the run goes', not ex('20261110_060000_routeF'))
    R.check('s294: KEEP with a future expiry kept', ex('20261110_060000_routeG'))
    R.check('s294: KEEP with a past expiry: the run goes', not ex('20261110_060000_routeH'))
    R.check('s294: run named by a gate lane kept', ex('20261110_060000_routeI'))
    R.check('s294: empty old KEEP is no hold', not ex('20261110_060000_routeJ'))
    R.check('s294 B1: a run\'s two folders one second apart both kept (newest within 30 min)', ex('20261110_130000_routeK') and ex('20261110_130001_routeK'))
    R.check('s294 B1: ... but the run 3 h older goes', not ex('20261110_100000_routeK'))
    R.check('s294 S2: a KEEP made of lane-name substrings (judgement post case scene) is no hold', not ex('20261110_060000_routeL'))
    R.check('s294 S4: a run folder matched by a check\'s glob prefix (20261110_05*) kept', ex('20261110_050000_routeM'))
    R.check('s294 S1: a state doc naming out/s9/runs does not hold every run', not ex('20261110_100000_routeA'))
    R.check('s294 S4: --plan reports a glob prefix that matches nothing as MISSING and one that matches as present',
            'MISSING GATE INPUT: out/s9/runs/zzz_*' in plan_out and 'MISSING GATE INPUT: out/s9/runs/20261110_05' not in plan_out, plan_out[:900])
    P = lambda *a: w.p('out', *a)
    g2 = w.run(script, '--guard', P('bench', 'userdata_frozen'), P('bench', 'userdata_frozen', 'a.txt'), w.p('tools'), P('s9', 'runs', '20261110_060000_routeG', 'data.bin'),
               P('s9', 'runs', '20261110_050000_routeM'), P('s9', 'runs'), P('zzz_free'))
    R.check('s294 S5: --guard refuses NO_TOUCH, outside paths, an active KEEP, a glob-prefix input and the parent of inputs; passes an unrelated path',
            all(('REFUSE ' + x) in g2 for x in (P('bench', 'userdata_frozen') + ' ', P('bench', 'userdata_frozen', 'a.txt'), w.p('tools') + ' ',
                                                 P('s9', 'runs', '20261110_060000_routeG', 'data.bin'), P('s9', 'runs', '20261110_050000_routeM'), P('s9', 'runs') + ' '))
            and 'REFUSE ' + P('zzz_free') not in g2 and '6 of 7' in g2, g2)
    # follow-up round: keep trees, newest runs, relative paths, doc-named paths under a keep tree
    ka = [P('build'), P('build', 'win-amd64-release'), P('q'), P('harness', 'run_list'), P('live'), P('harness')]
    for dd in ka:
        os.makedirs(dd, exist_ok=True)
    gk = w.run(script, '--guard', *ka)
    R.check('s294 F1: --guard refuses out/build, out/build/win-amd64-release, out/q, out/harness/run_list, out/live and out/harness (at / under / above a keep tree)',
            '%d of %d paths refused' % (len(ka), len(ka)) in gk, gk)
    gr = w.run(script, '--guard', P('s9', 'runs', '20261110_090000_routeB'))
    R.check('s294 F1: --guard refuses the newest run folder of a route (a run in progress)', 'newest run folder' in gr, gr)
    r1 = subprocess.run([sys.executable, script, '--guard', os.path.join('out', 'oldcase')], env=w.env(), cwd=w.root, capture_output=True, text=True, creationflags=NOWIN).stdout
    r2 = subprocess.run([sys.executable, script, '--guard', os.path.join('out', 'oldcase')], env=w.env(), cwd=base, capture_output=True, text=True, creationflags=NOWIN).stdout
    R.check('s294 F1: a relative path means the CURRENT directory (inside the checkout: protected; elsewhere: outside, refused)',
            'protected' in r1 and 'outside the folders' in r2 and os.path.join(base, 'out', 'oldcase') in r2, r1 + r2)
    write(w.p('STATUS.md'), 'the runs are in out/s9/runs and out/build/special_doc\n')
    nr = os.path.join(base, 's294_named.py')
    write(nr, "import importlib.util, json, sys\nspec = importlib.util.spec_from_file_location('t', sys.argv[1])\nT = importlib.util.module_from_spec(spec); spec.loader.exec_module(T)\nprint(json.dumps(sorted(T.named_paths())))\n")
    nk = subprocess.run([sys.executable, nr, script], env=w.env(), capture_output=True, text=True, creationflags=NOWIN).stdout
    R.check('s294 F: a state doc naming a path under a keep tree (out/build/special_doc) stays named; out/s9/runs does not', 'out/build/special_doc' in nk and 'out/s9/runs"' not in nk, nk)
    reg = w.wt('lane288', under=('thirdparty', 'sdk-wt'))
    gw = w.run(script, '--guard', reg, os.path.join(reg, 'sub'), w.p('thirdparty', 'sdk-wt'))
    R.check('s294 F1: --guard refuses a registered sdk-wt worktree and the sdk-wt folder above it', gw.count('REFUSE') >= 2 and 'at, inside or above the registered worktree' in gw, gw)
    R.check('s294: capture copy 7 h old removed (6 h rule)', not w.exists('out', 'cap_cap7'))
    R.check('s294: capture copy 3 h old kept', w.exists('out', 'cap_cap3', 'images.bin'))
    R.check('s294: capture inside a kept run kept', w.exists('out', 's9', 'runs', '20261110_060000_routeD', 'cap', 'images.bin'))
    rm_world(w)

    # ---- agent worktree out\ scratch, in-process (the 10 GB limit scaled to 0.5 MB: no huge files)
    print('lane 294: agent worktree out scratch (scaled: limit 0.5 MB): idle rule, holds, KEEP, half captures, builds, reparse points')
    w = World(base, 's294b')
    w.live = [['sess1', now - 2 * H, now - 60]]
    names = ['fin', 'fin2', 'finu', 'live', 'small', 'held', 'keepw', 'dirty']
    wts = {n: w.wt('agent-' + n, 'unmerged' if n == 'finu' else 'merged') for n in names}
    ext = os.path.join(base, 's294_ext')
    write(os.path.join(ext, 'precious.txt'), 'keep me')
    age_tree(ext, now - 50 * H)
    for n in names:
        write(os.path.join(wts[n], 'out', 'cases', 'old.bin'), b'x' * (1000 if n == 'small' else 2_000_000))
    write(os.path.join(wts['live'], 'out', 'cap', 'images.bin'), b'x' * 100_000)         # half capture: images.bin old, a frame fresh
    write(os.path.join(wts['live'], 'out', 'cap', 'frame1.bin'), b'x' * 1000)
    write(os.path.join(wts['live'], 'out', 'build', 'CMakeCache.txt'), b'x' * 1000)       # a live build tree
    write(os.path.join(wts['live'], 'out', 'build', 'obj.o'), b'x' * 100_000)
    write(os.path.join(wts['live'], 'out', 'plain', 'a.bin'), b'x' * 100_000)             # a plain old folder
    write(os.path.join(wts['fin'], 'out', 'tracked.txt'), 'tracked')
    git(wts['fin'], 'add', '-f', 'out/tracked.txt')
    git(wts['fin'], 'commit', '-q', '-m', 'tracked out file')
    git(w.root, 'merge', '-q', 'br_fin')                                                   # fin is merged
    write(os.path.join(wts['keepw'], 'KEEP'), 'x')
    write(os.path.join(wts['dirty'], 'README.md'), 'edited and never committed')      # HEAD == the session branch, but uncommitted tracked edits
    for n in names:
        age_tree(wts[n], now - 3 * H)
    set_times(os.path.join(wts['live'], 'out', 'cap', 'frame1.bin'), now - 60)
    write(os.path.join(wts['fin2'], 'out', 'fresh.bin'), b'y' * 1000)                      # a reviewer is using it right now
    write(os.path.join(wts['live'], 'out', 'fresh.bin'), b'y' * 1000)
    junction(os.path.join(wts['live'], 'out', 'jl'), ext)
    for n in ('fin', 'fin2', 'finu', 'held', 'keepw', 'dirty'):
        w.meta('agent-' + n, True, hours=1)
        w.transcript('agent-' + n, 2)
    for n in ('live', 'small'):
        w.meta('agent-' + n, False, hours=0.1)
        w.transcript('agent-' + n, 0.1)
    with open(w.p('out', 'tidy', 'carry.json'), 'w') as f:
        json.dump([{'path': '.claude/worktrees/agent-held', 'why': 'x', 'added': now - 5 * H, 'expires': now + 5 * 86400, 'fixed': True}], f)
    runner = os.path.join(base, 's294_runner.py')
    write(runner, r"""
import importlib.util, json, sys, time
spec = importlib.util.spec_from_file_location('tidy_under_test', sys.argv[1])
T = importlib.util.module_from_spec(spec); spec.loader.exec_module(T)
T.SCRATCH_REPORT_GB = 0.0005     # GB is 1e9, so 5e-4 GB = 0.5 MB
total, lines = T.scratch_pass(time.time(), False, float(sys.argv[2]))
print(json.dumps({'total': total, 'lines': lines}))
""")

    def go(free):
        r = subprocess.run([sys.executable, runner, script, str(free)], env=w.env(), capture_output=True, text=True, errors='replace', creationflags=NOWIN)
        try:
            return json.loads(r.stdout.strip().splitlines()[-1]), r.stdout + r.stderr
        except (ValueError, IndexError):
            return {'total': -1, 'lines': []}, r.stdout + r.stderr

    def ex2(n, *rel):
        return w.exists('.claude', 'worktrees', 'agent-' + n, 'out', *rel)
    o1, raw = go(500)       # no pressure
    R.check('s294: finished merged agent idle > 30 min: out scratch cleared, the tracked file stays', not ex2('fin', 'cases', 'old.bin') and ex2('fin', 'tracked.txt'), raw)
    R.check('s294 S3: a finished agent whose out has a file from the last minutes (a reviewer) is untouched', ex2('fin2', 'cases', 'old.bin') and ex2('fin2', 'fresh.bin'))
    R.check('s294 S3: a finished agent on an UNMERGED branch waits (24 h)', ex2('finu', 'cases', 'old.bin'))
    R.check('s294 F3: a finished builder that never committed (HEAD == session branch, uncommitted edits) waits 24 h like an unmerged one', ex2('dirty', 'cases', 'old.bin'))
    R.check('s294 B2: a held (--hold) worktree is skipped', ex2('held', 'cases', 'old.bin'))
    R.check('s294 B2: a worktree with a KEEP at its root is skipped', ex2('keepw', 'cases', 'old.bin'))
    R.check('s294: the scratch pass does not remove the worktree itself', w.exists('.claude', 'worktrees', 'agent-fin', 'pad.bin'))
    R.check('s294: live agent not under pressure: untouched, but reported loudly',
            ex2('live', 'cases', 'old.bin') and any('LOUD' in x and 'agent-live' in x for x in o1['lines']), str(o1))
    o2, raw = go(50)        # under 100 GB free
    R.check('s294: live agent over the limit under pressure: an old folder is cleared whole', not ex2('live', 'cases', 'old.bin') and not ex2('live', 'plain', 'a.bin'), raw)
    R.check('s294 B3: ... a half capture (old images.bin beside a fresh frame) is left whole', ex2('live', 'cap', 'images.bin') and ex2('live', 'cap', 'frame1.bin'))
    R.check('s294 B3: ... a live build tree (CMakeCache.txt) is left whole', ex2('live', 'build', 'CMakeCache.txt') and ex2('live', 'build', 'obj.o'))
    R.check('s294: ... a file modified in the last 15 min stays', ex2('live', 'fresh.bin'))
    R.check('s294: ... and a junction inside out is never followed (target intact)', os.path.exists(os.path.join(ext, 'precious.txt')))
    R.check('s294: a live agent under the limit is untouched under pressure', ex2('small', 'cases', 'old.bin'))
    R.check('s294 B2: pressure still skips held and KEEP worktrees', ex2('held', 'cases', 'old.bin') and ex2('keepw', 'cases', 'old.bin'))
    of = w.wt('agent-oldfin')
    write(os.path.join(of, 'out', 'x.bin'), b'x' * 100)
    age_tree(of, now - 9 * H)
    w.meta('agent-oldfin', True, hours=5)
    w.transcript('agent-oldfin', 6)
    ga = w.run(script, '--guard', w.p('.claude', 'worktrees', 'agent-live', 'out', 'cap'), w.p('.claude', 'worktrees', 'agent-fin', 'out', 'tracked.txt'),
               os.path.join(of, 'out', 'x.bin'))
    R.check('s294 F1: --guard refuses inside the out of a running agent and of a recently finished one, and passes an old finished one',
            'running or was active' in ga and ga.count('REFUSE') == 2 and '2 of 3' in ga, ga)
    os.rmdir(os.path.join(wts['live'], 'out', 'jl'))
    rm_world(w)
    shutil.rmtree(ext, ignore_errors=True)

    # ---- S6: the 10-minute watch with real out folders and nothing to clear: no scratch pass, quick, nothing touched
    print('lane 294: --auto watch with real agent out folders and nothing to clear')
    w = World(base, 's294c')
    w.live = [['sess1', now - 2 * H, now - 60]]
    p1 = w.wt('agent-busy')
    write(os.path.join(p1, 'out', 'a.bin'), b'x' * 1000)
    age_tree(p1, now - 3 * H)
    w.meta('agent-busy', False, hours=0.1)
    w.transcript('agent-busy', 0.1)
    t0 = time.time()
    out = w.run(script, '--auto', 'watch')
    R.check('s294 S6: watch with nothing to clear exits quietly (no scratch pass, no lock), under 20 s',
            'scratch' not in out and 'skipped' not in out and time.time() - t0 < 20 and w.exists('.claude', 'worktrees', 'agent-busy', 'out', 'a.bin'), out)
    rm_world(w)


def run_suite(script, base):
    R = Results()
    suite_agents(script, base, R)
    suite_holds(script, base, R)
    suite_low(script, base, R)
    suite_morning(script, base, R)
    suite_inputs(script, base, R)
    suite_liveness(script, base, R)
    suite_inproc(script, base, R)
    suite_cli(script, base, R)
    suite_s294(script, base, R)
    return R


# --------------------------------------------------------------------------------------------- mutants
MUTANTS = [
    ('transcript no longer keeps a worktree', "elif w['state'] in ('remove', 'report') and last >= cutoff:", "elif False:"),
    ('fresh files no longer keep a worktree', "    if not age.expired(w['t']):\n        w.update(state='kept', why='recent activity')",
     "    if False:\n        w.update(state='kept', why='recent activity')"),
    ('everything counts as unmerged', "merged = is_merged(REPO, w.get('head'), branch)", "merged = False"),
    ('untracked scratch files are not real edits', "    if real:\n        w['why'] = '%d real edit(s)", "    if False:\n        w['why'] = '%d real edit(s)"),
    ('junction refusals gone (check and the remover re-check)',
     [("    if links:\n        w['why'] = 'holds a junction", "    if False:\n        w['why'] = 'holds a junction"),
      ("if links or keeps or t >= self.guard:", "if keeps or t >= self.guard:")]),
    ('HEAD on no branch is not saved', "        w['save_ref'] = True\n    w['state'] = 'remove'", "        w['save_ref'] = False\n    w['state'] = 'remove'"),
    ('gate inputs unprotected', "    for k, v in gate_inputs().items():\n        named.setdefault(k, v)\n", ""),
    ('no rolling expiry (24 h under a live session)', "own_since, (start, now - LIVE_KEEP_H * HOUR))", "own_since, (start, now - EXPIRE_H * HOUR))"),
    ('1 h old output expires', "own_since, (start, now - LIVE_KEEP_H * HOUR))", "own_since, (start, now - 0.0001 * HOUR))"),
    ('S1: the 6 h rule also hits output made before the session', "own_since, (start, now - LIVE_KEEP_H * HOUR))", "own_since, (0.0, now - LIVE_KEEP_H * HOUR))"),
    ('S2: BUILD_SETUP edits are benign in agent worktrees', "benign = (set() if w.get('agent') else", "benign = (BUILD_SETUP if w.get('agent') else"),
    ('S3b: depth-2 names dropped', "if len(parts) < 2 or parts[0] != 'out':", "if len(parts) < 3 or parts[0] != 'out':"),
    ('S3c: names from tools stay protected for ever', "touched_within(os.path.join(REPO, k.replace('/', os.sep)), TOOL_STALE_DAYS)", "True"),
    ('S3c: tools/*.py not scanned', "for dp, dn, fn in os.walk(troot):", "for dp, dn, fn in []:"),
    ('S3d: a failed unlink does not stop the folder', "stay')\n                return []", "stay')\n                continue"),
    ('S3d: capture markers not deleted first', "lambda e: (e.name.lower() not in CAPTURE_MARKERS and not (", "lambda e: (False and not ("),
    ('A: unlock also a worktree whose folder exists', "if not os.path.isdir(lp(w['path'])):", "if True:"),
    ('Q3: orphan metas keep a worktree for ever', "and max(i['tr'], i['meta_t']) >= now - ORPHAN_META_H * HOUR for i in mine)", "and True for i in mine)"),
    ('Q2: files of a capture folder that does not go whole are deleted one by one', "child_rows = [r for r in child_rows if r['dir']]", "pass"),
    ('.git is not deleted last', "e.name == '.git', e.name.lower()))", "False, e.name.lower()))"),
    ('--ahead forecast ignores the real live session ids', "wts = check_worktrees(age, protect, now, {s_[0] for s_ in live})", "wts = check_worktrees(age, protect, now)"),
    ('S4: worktrees mapped by folder name only', "mine = [i for i in infos if i['wt'] == key or i['id'] == name]", "mine = [i for i in infos if i['id'] == name]"),
    ('S4: no re-read right before deleting', "        if w.get('agent'):\n            why = self.agent_active_now(w)", "        if False:\n            why = self.agent_active_now(w)"),
    ('N1: no junction check at the top of clear_dir', "& REPARSE:        # N1", "& 0:        # N1"),
    ('N2: removed worktrees are not unlocked before prune', "            git(w['repo'], 'worktree', 'unlock', w['path'])", "            pass"),
    ('N3: sdk-wt / out\\wt worktrees ignore the merged / unmerged split', "check_worktree(w, age if merged else Age(now - AGENT_UNMERGED_H * HOUR), protect, carried, subs)", "check_worktree(w, age, protect, carried, subs)"),
    ('--until holds released by --auto end', "released_before is not None and c['added'] < released_before and not c['fixed']",
     "released_before is not None and c['added'] < released_before"),
    ('plain holds not released by --auto end', "released_before is not None and c['added'] < released_before and not c['fixed']", "False"),
    ('legacy Jarrod entries not migrated', "c['fixed'] = bool(FIXED_WHY.search(c.get('why', '')))", "c['fixed'] = False"),
    ('low pass skips agent worktrees', "    arows = agent_rows()\n", "    arows = []\n"),
    ('s294: capture folders a check names are protected only while fresh', "(required_input(k) or touched_within(", "(touched_within("),
    ('unmerged branches get the 2 h rule', "hours = merged_h if merged else unmerged_h", "hours = merged_h"),
    ('S4: running agents (no finished flag) do not keep a worktree', "running = any(i['has_meta'] and i['finished'] is not True and (i['parent'] in live_ids or ANY_PARENT)", "running = False and any(i['has_meta'] and i['finished'] is not True and (i['parent'] in live_ids or ANY_PARENT)"),
    ('s294: a KEEP that cites a tool is no hold', "named = named or any(os.path.isfile(os.path.join(REPO, t.rstrip", "named = named or False and any(os.path.isfile(os.path.join(REPO, t.rstrip"),
    ('s294: scratch pass ignores the pressure rule', "if free < SCRATCH_PRESSURE_GB and over:", "if False:"),
    ('s294: tracked files in scratch are deleted', "or any(t == crel or t.startswith(crel + '/') for t in tracked):", "or any(t == crel or t.startswith(crel + '/') for t in set()):"),
    ('s294 B2: held worktrees are deleted from (scratch skip and Deleter.safe both)', [("    if any(key == k or key.startswith(k + '/') for k in d.carried):\n        return 'held (--hold)'", "    if False:\n        return 'held (--hold)'"), ("        if any(key == k or key.startswith(k + '/') for k in self.carried) or keep_above(p):", "        if keep_above(p):")]),
    ('s294 F1: --guard lets keep trees through', "        return 'at or under the keep-list / never-touch folder out\\\\' + k.replace('/', '\\\\')", "        return ''"),
    ('s294 F1: --guard ignores trees ABOVE a keep tree', "        if k.startswith(r + '/'):\n            return 'above the keep-list", "        if False:\n            return 'above the keep-list"),
    ('s294 F1: --guard lets a running agent\'s out through', "    if running or last >= now - AGENT_LIVE_H * HOUR:\n        return 'inside the out", "    if False:\n        return 'inside the out"),
    ('s294 F1: --guard lets the newest run through', "        if m.group(2) in newest_run_names(names):", "        if False:"),
    ('s294 F1: relative guard paths mean the checkout, not the current directory', "        a.guard = [os.path.abspath(x) for x in a.guard]", "        a.guard = [os.path.join(REPO, x) for x in a.guard]"),
    ('s294 F3: an uncommitted builder counts as merged', "idle = NOW_IDLE_H if (clean and is_merged(", "idle = NOW_IDLE_H if (is_merged("),
    ('s294 F: doc-named paths under keep trees are dropped', "            if k.startswith('out/') and _doc_namespace(k[4:]):", "            if k.startswith('out/') and _namespace(k[4:]):"),
    ('s294: finished agents keep their out scratch', "            elif finished:\n", "            elif False:\n"),
    ('s294 F1: --guard lets registered worktrees through (both rules)', [("        if is_allowed(w['path']) and (n == wp or wp.startswith(n + os.sep) or n.startswith(wp + os.sep)):", "        if False:"), ("        if n.startswith(rn + os.sep) and os.path.exists(os.path.join(ap, '.git')):", "        if False:")]),
    ('s294 S3: unmerged finished agents are cleared at 30 min', "idle = NOW_IDLE_H if (clean and is_merged(REPO, w.get('head'), branch)) else AGENT_UNMERGED_H", "idle = NOW_IDLE_H"),
    ('s294 S1: a doc naming a namespace holds everything under it', "            if k.startswith('out/') and _doc_namespace(k[4:]):\n                continue", "            if False:\n                continue"),
    ('s294 S5: --guard ignores never-touch folders (both rules)', [("            elif d.protected_zone(ap):", "            elif False:"), ("    for k in sorted(KEEP_TREES | NO_TOUCH):", "    for k in sorted(KEEP_TREES):")]),
    ('s294 S5: --guard ignores active KEEP files', "                elif not why and (keep_above(ap) or keep_active(ap)):", "                elif False:"),
    ('s294 B1: only the exact newest folder of a route counts', "newest.update(n for t, n in lst if t >= top - RUN_PAIR_MIN * 60)", "newest.update(n for t, n in lst if t >= top)"),
    ('s294 B2: a KEEP in the worktree root is ignored', "    if keep_active(p) or walk_tree(p)[4]:", "    if False:"),
    ('s294 B3: a folder with a fresh file goes anyway (half captures)', "if links or keeps or newest >= guard_t or (skip_build and build):", "if links or keeps or (skip_build and build):"),
    ('s294 B3: a live build tree is cleared', "if links or keeps or newest >= guard_t or (skip_build and build):", "if links or keeps or newest >= guard_t:"),
    ('s294 S3: a finished agent is cleared although a file is fresh', "                if walk_tree(o)[2] >= guard:", "                if False:"),
    ('s294 S2: lane-name substrings make a KEEP valid', "        named = named or any(os.path.isfile(", "        named = named or any(n in low for n in lanes) or any(os.path.isfile("),
    ('s294 S4: glob prefixes protect nothing', "        return key in self.protect or (bool(self.prefixes) and key.startswith(self.prefixes))", "        return key in self.protect"),
    ('s294 S4: a prefix is "present" without a match', "            present = bool(glob.glob(glob.escape(base) + '*'))", "            present = True"),
    ('s294 S5: --guard accepts paths outside the tidy\'s folders', "            if not (is_allowed(ap) or in_agent_out(ap)):", "            if False:"),
    ('s294 S6: the watch always takes the scratch pass', "                if not safe_scratch(time.time(), True)[0]:", "                if False:"),
    ('s294: every run folder keeps the old rule (no 2 h limit)', "else self.run_age", "else self.strict"),
    ('s294: the newest run of a route is not kept', "mrun.group(2) in self.run_newest.get(mrun.group(1), ())", "False"),
    ('s294: a bare KEEP never ages out', "        if now < left:", "        if True:"),
    ('s294: KEEP expiry date ignored', "info['valid'] = until > now", "info['valid'] = True"),
    ('s294: capture copies follow the 24 h rule', "                ao = self.cap", "                ao = None"),
    ('low pass smallest first', "-i['b'] if top == 'worktree' else i['t'])", "i['b'] if top == 'worktree' else i['t'])"),
]


def old_script(base):
    p = os.path.join(base, 'tidy_old.py')
    src = sh('git', 'show', '%s:tools/harness/tidy.py' % OLD_REV, cwd=REPO_REAL)
    with open(p, 'w', encoding='utf-8', newline='') as f:
        f.write(src)
    return p


def refind(base):
    print('re-find, the 2026-11-12 case: live session since 23 h ago, temp output 10 h old, --until hold made before it, gate inputs without KEEP')
    old = old_script(base)
    mb = 1_000_000
    rows = {}

    def build(tag):
        w = World(base, 'rf_' + tag)
        now = time.time()
        w.live = [['sess1', now - 23 * H, now - 60]]
        write(w.p('tools', 'gate', 'lanes', 'g.json'), json.dumps(GATE_LANE))
        w.temp('finished_a', 10, 20 * mb)
        w.temp('finished_b', 10, 30 * mb)
        w.temp('held_until', 10, 10 * mb)
        w.temp('gate/brock/capture', 10, 15 * mb)
        w.temp('gate/wm30/capture', 10, 15 * mb)
        with open(w.p('out', 'tidy', 'carry.json'), 'w') as f:
            json.dump([{'path': 'out/held_until', 'why': 'Session 99 builder work, Jarrod: keep, until 2026-11-16', 'added': now - 30 * H,
                        'expires': now + 4 * 86400}], f)
        return w

    def size(w, *rel):
        t = 0
        for dp, _, fn in os.walk(w.p('out', *rel)):
            t += sum(os.path.getsize(os.path.join(dp, f)) for f in fn)
        return t / mb

    for name, script in (('OLD', old), ('NEW', NEW)):
        w = build(name.lower())
        out = w.run(script, '--plan')
        plans = sorted(f for f in os.listdir(w.p('out', 'tidy')) if f.startswith('plan_') and f.endswith('.json'))
        plan = json.load(open(w.p('out', 'tidy', plans[-1])))
        planned = sum(i['b'] for i in plan['items']) / mb
        w.run(script, '--auto', 'end', '--session', 'sess1')
        rows[name] = dict(plan_mb=planned, finished_left=size(w, 'finished_a') + size(w, 'finished_b'),
                          hold_left=size(w, 'held_until'), hold_listed=any(c['path'] == 'out/held_until' for c in w.carry()),
                          gate_left=size(w, 'gate'))
        rm_world(w)
    print('                         OLD        NEW')
    for key, label in (('plan_mb', '--plan would delete (MB)'), ('finished_left', 'finished output left after --auto end (MB, was 50)'),
                       ('hold_left', 'held_until left after --auto end (MB, was 10)'), ('hold_listed', 'hold still in carry.json'),
                       ('gate_left', 'gate inputs left after --auto end (MB, was 30)')):
        a, b = rows['OLD'][key], rows['NEW'][key]
        print('  %-55s %-10s %s' % (label, a if isinstance(a, bool) else '%.0f' % a, b if isinstance(b, bool) else '%.0f' % b))
    good = (rows['OLD']['plan_mb'] == 0 and not rows['OLD']['hold_listed'] and rows['OLD']['hold_left'] == 0
            and rows['NEW']['plan_mb'] >= 49 and rows['NEW']['hold_listed'] and rows['NEW']['hold_left'] == 10
            and rows['NEW']['gate_left'] == 30 and rows['NEW']['finished_left'] == 0)
    print('  re-find %s' % ('OK: old deletes 0 under --plan and releases the --until hold; new frees the output, keeps hold and gate inputs'
                            if good else 'NOT AS EXPECTED'))
    return 0 if good else 1


def main():
    base = tempfile.mkdtemp(prefix='tidy_selftest_', dir=os.environ.get('TIDY_SELFTEST_DIR') or None)
    try:
        if '--refind' in sys.argv:
            return refind(base)
        if '--mutant' in sys.argv:          # one mutant, quiet (the parallel runner starts these)
            i = int(sys.argv[sys.argv.index('--mutant') + 1])
            m = MUTANTS[i]
            src = open(NEW, encoding='utf-8', newline='').read().replace('\r\n', '\n')
            pairs = m[1] if isinstance(m[1], list) else [(m[1], m[2])]
            if any(a not in src for a, _ in pairs):
                print('RESULT BAD %s :: text moved, mutant not applicable' % m[0])
                return 1
            mut = src
            for a, b in pairs:
                mut = mut.replace(a, b, 1)
            mp = os.path.join(base, 'tidy_mut.py')
            with open(mp, 'w', encoding='utf-8', newline='') as f:
                f.write(mut)
            import contextlib
            with contextlib.redirect_stdout(open(os.devnull, 'w')):
                R = run_suite(mp, base)
            print('RESULT %s %s :: %s' % ('OK' if R.failed() else 'BAD', m[0], ('caught by ' + '; '.join(R.failed())) if R.failed() else 'NOT CAUGHT'))
            return 0
        if '--mutants' in sys.argv:         # every mutant, 5 at a time
            todo, running, bad = list(range(len(MUTANTS))), [], 0
            while todo or running:
                while todo and len(running) < 5:
                    i = todo.pop(0)
                    running.append(subprocess.Popen([sys.executable, os.path.abspath(__file__), '--mutant', str(i)], stdout=subprocess.PIPE,
                                                    stderr=subprocess.STDOUT, text=True, errors='replace', creationflags=NOWIN))
                for pr in list(running):
                    if pr.poll() is not None:
                        out = pr.stdout.read()
                        line = next((x for x in out.splitlines() if x.startswith('RESULT')), 'RESULT BAD (no result) :: ' + out[-300:])
                        print(line[:260], flush=True)
                        bad += line.startswith('RESULT BAD')
                        running.remove(pr)
                time.sleep(1)
            print('mutants not caught: %d of %d' % (bad, len(MUTANTS)))
            return 1 if bad else 0
        R = run_suite(NEW, base)
        print('%d checks, %d failed' % (len(R.rows), len(R.failed())))
        return 1 if R.failed() else 0
    finally:
        shutil.rmtree(base, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
