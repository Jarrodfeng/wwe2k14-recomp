"""S102 TIDY2 selftest for the new out\\ clean-up (tools\\harness\\tidy.py + tidy_out.py + tools\\gate\\needs_lint.py). 0 game runs; every tree is
built in a TEMP folder (nothing under the checkout's out\\ is touched, nothing real is deleted).

  py -I tools\\harness\\tidy_out_selftest.py

Every case has a planted twin that must come out the other way. The first cases re-find the 2026-11-15 bug: a lane naming out/cases held 265
fview_unpack_* copies (140 GB). Then the rules: a container-naming lane is a lint error and protects nothing; an unpack copy is never held (not even
inside a gate-named folder); out\\build (SACRED: Jarrod plays from it) survives session end AND --now; a build tree survives; a keep_list path survives and
prints its size; a KEEP file no longer protects; nothing is deleted while a session is live (except --now under the reserve, which also leaves recent
files); with no live session the hourly pass clears.
"""
import contextlib
import ctypes
import importlib.util
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FAILS, N = [], 0
k32 = ctypes.windll.kernel32
k32.CreateFileW.restype = ctypes.c_void_p
k32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
k32.SetFileTime.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
k32.CloseHandle.argtypes = [ctypes.c_void_p]


def set_times(path, t):
    ft = ctypes.c_ulonglong(int((t + 11644473600) * 1e7))
    h = k32.CreateFileW('\\\\?\\' + os.path.abspath(path), 0x100, 7, None, 3, 0x02000000, None)
    k32.SetFileTime(h, ctypes.byref(ft), ctypes.byref(ft), ctypes.byref(ft))
    k32.CloseHandle(h)


def age_tree(root, t):
    for dp, dn, fn in os.walk(root, topdown=False):
        for n in fn:
            set_times(os.path.join(dp, n), t)
        set_times(dp, t)


def check(name, cond, detail=''):
    global N
    N += 1
    if not cond:
        FAILS.append(name)
    print('%s  %s%s' % ('PASS' if cond else 'FAIL', name, ('  (' + detail + ')') if detail and not cond else ''))


def write(p, data=b'x' * 100):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, 'wb').write(data if isinstance(data, bytes) else data.encode())


def load_tidy(root, keep):
    os.environ['TIDY_TEST_ROOT'] = root
    os.environ['TIDY_TEST_PROJECTS'] = os.path.join(root, 'projects')
    os.environ['TIDY_TEST_KEEP'] = keep
    os.environ['TIDY_TEST_SCRATCH'] = os.path.join(root, 'scratchpads')
    for d in ('projects', 'scratchpads'):
        os.makedirs(os.path.join(root, d), exist_ok=True)
    spec = importlib.util.spec_from_file_location('tidy_under_test', os.path.join(HERE, 'tidy.py'))
    m = importlib.util.module_from_spec(spec)
    sys.modules['tidy_under_test'] = m
    spec.loader.exec_module(m)
    return m


def run(fn, *a, **k):
    time.sleep(0.3)      # files made a moment ago must be older than the pass's start (the Deleter spares newer ones)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        fn(*a, **k)
    return buf.getvalue()


def main():
    tmp = tempfile.mkdtemp(prefix='tidy_out_selftest_')
    try:
        return body(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def body(root):
    sys.path.insert(0, HERE)
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), 'tools', 'gate'))
    subprocess.run(['git', 'init', '-q', root], capture_output=True)
    keep = os.path.join(root, 'keep_list.txt')
    open(keep, 'w').write('# test keep list\n')
    out = os.path.join(root, 'out')
    os.makedirs(os.path.join(out, 'tidy'))
    open(os.path.join(out, 'tidy', 'enabled'), 'w').write('x\n')
    lanes = os.path.join(root, 'tools', 'gate', 'lanes')
    os.makedirs(lanes)
    m = load_tidy(root, keep)
    import needs_lint
    import tidy_out
    old = time.time() - 100 * 86400

    def reset():
        for d in os.listdir(out):
            if d != 'tidy':
                p = os.path.join(out, d)
                shutil.rmtree(p) if os.path.isdir(p) else os.unlink(p)
        for f in os.listdir(lanes):
            os.unlink(os.path.join(lanes, f))
        open(keep, 'w').write('# test keep list\n')
        m._held_cache.clear()
        os.environ['TIDY_TEST_LIVE'] = '[]'
        os.environ.pop('TIDY_TEST_FREE', None)

    def lane(needs, name='t.json'):
        json.dump({'checks': [{'name': 'x', 'needs': needs}]}, open(os.path.join(lanes, name), 'w'))

    def exists(rel):
        return os.path.exists(os.path.join(root, rel))

    # ---- 1. the 2026-11-15 re-find: a lane naming out/cases held every fview_unpack copy
    reset()
    for i in range(3):
        write(os.path.join(out, 'cases', 'fview_unpack_2026%d' % i, 'capture', 'images.bin'))
    write(os.path.join(out, 'cases', 'case_a', 'capture', 'images.bin'))
    lane(['out/cases'])
    errs = needs_lint.lint_lane_file(os.path.join(lanes, 't.json'), out)
    check('lint: a lane naming out/cases FAILS', bool(errs) and 'container' in errs[0], str(errs))
    lane(['out/cases/case_a/capture'])
    check('lint twin: the exact capture passes', not needs_lint.lint_lane_file(os.path.join(lanes, 't.json'), out))
    lane(['out/cases'])
    o = run(m.cmd_auto, 'end', False, None)
    check('fix 4: a container-naming lane is a LOUD problem and out\\ is NOT swept (fail closed)', 'NOT swept' in o and 'names "out/cases" which is a container' in o and exists('out/cases/case_a/capture/images.bin'), o[-400:])
    check('the container name protects nothing (held index has no out/cases entry)', not m.held_index(fresh=True)[0].why('out/cases/case_a/capture/images.bin'))
    lane(['out/cases/case_a/capture'])
    o = run(m.cmd_auto, 'end', False, None)
    check('twin: with the lane fixed the sweep runs: unpack copies gone, the named capture stays', not exists('out/cases/fview_unpack_20260') and exists('out/cases/case_a/capture/images.bin'), o[-300:])

    # ---- 2. an unpack copy is never held, even inside a folder a lane names exactly
    reset()
    write(os.path.join(out, 'leaf', 'keepme.bin'))
    write(os.path.join(out, 'leaf', 'fview_unpack_1', 'capture', 'images.bin'))
    write(os.path.join(out, 'cases', 'fview_unpack_9', 'capture', 'images.bin'))
    write(os.path.join(out, 'cases', 'case_real', 'capture', 'images.bin'))
    lane(['out/leaf', 'out/cases/case_real/capture'])
    o = run(m.cmd_auto, 'end', False, None)
    check('a fview_unpack under a gate-named folder is deleted at session end', not exists('out/leaf/fview_unpack_1'), o[-300:])
    check('twin: the files next to it in the named leaf stay', exists('out/leaf/keepme.bin'))
    check('an unpack copy no lane names is deleted', not exists('out/cases/fview_unpack_9'))
    lane(['out/cases/fview_unpack_9/capture/images.bin'])
    o = run(m.cmd_auto, 'end', False, None)
    check('a lane naming an unpack copy directly is a lint problem (no sweep)', 'NOT swept' in o and 'unpack' in o, o[-300:])
    check('twin: the exact case capture a lane names stays', exists('out/cases/case_real/capture/images.bin'))

    # ---- 3. builds: out\build is SACRED, renderer/dev stays, other renderer output goes
    reset()
    write(os.path.join(out, 'build', 'win-amd64-release', 'wwe2k14.exe'))
    write(os.path.join(out, 'build', 'win-amd64-release', 'jarrod_settings', 'config.toml'))
    write(os.path.join(out, 'renderer', 'dev', 'fview.exe'))
    write(os.path.join(out, 'renderer', 'sheets', 'a.png'))
    write(os.path.join(out, 'junk', 'big.bin'))
    for p in ('build', 'renderer', 'junk'):
        age_tree(os.path.join(out, p), old)
    o = run(m.cmd_auto, 'end', False, None)
    check('out\\build survives session end (SACRED, even 100 days old)', exists('out/build/win-amd64-release/wwe2k14.exe') and exists('out/build/win-amd64-release/jarrod_settings/config.toml'), o[-300:])
    check('the renderer dev build survives', exists('out/renderer/dev/fview.exe'))
    check('twin: other renderer output and junk go', not exists('out/renderer/sheets') and not exists('out/junk'))

    # ---- 4. a keep_list path survives and prints its size; a bad line is a problem and does not hold
    reset()
    write(os.path.join(out, 'sx', 'attires', 'a.bin'), b'x' * 5000)
    write(os.path.join(out, 'sx', 'other', 'b.bin'))
    write(os.path.join(out, 'cases', 'many1', 'images.bin'))
    open(keep, 'w').write('out/sx/attires | the attire census, tools\\assets\\attire_check.py\n')
    m._held_cache.clear()
    o = run(m.cmd_auto, 'end', False, None)
    check('a keep_list path survives, its sibling goes', exists('out/sx/attires/a.bin') and not exists('out/sx/other'), o[-300:])
    check('the keep list prints the held path with its size', 'out/sx/attires' in o and '0.0 GB' in o and 'keep list' in o, o[-600:])
    write(os.path.join(out, 'sx', 'other2', 'b.bin'))
    open(keep, 'w').write('out/sx/attires | the attire census\nout/cases | everything | \nout/sx/nothing\nout/sx/old | stale | 2020-01-01\n')
    m._held_cache.clear()
    o = run(m.cmd_auto, 'end', False, None)
    check('fix 4: a bad keep line (container / no reason / expired) is a problem and out\\ is NOT swept', 'NOT swept' in o and 'has no reason' in o and 'is a container' in o and exists('out/sx/other2/b.bin'), o[-600:])
    ents, probs = tidy_out.load_keep_list(keep, today=__import__('datetime').date(2026, 11, 15))
    open(keep, 'w').write('out/sx/attires | old | 2020-01-01\n')
    ents, probs = tidy_out.load_keep_list(keep)
    check('keep_list: an entry past its date does not hold (and is reported)', not ents and any('expired' in p for p in probs))

    # ---- 5. a KEEP file no longer protects
    reset()
    write(os.path.join(out, 's5', 'run1', 'a.bin'))
    write(os.path.join(out, 's5', 'run1', 'KEEP'), 'keep this until 2099-01-01, Jarrod said so')
    write(os.path.join(out, 's5', 'run2', 'a.bin'))
    o = run(m.cmd_auto, 'end', False, None)
    check('a KEEP file (even with a far date) no longer protects its folder', not exists('out/s5/run1'), o[-300:])
    check('the KEEP file is reported as ignored', 'KEEP file ignored' in o and 'keep_list.txt' in o, o[-500:])

    # ---- 6. nothing is deleted while a session is live
    reset()
    now = time.time()
    me = ['abc12345-0000', now - 3600, now - 5]
    os.environ['TIDY_TEST_LIVE'] = json.dumps([me])
    write(os.path.join(out, 'scratch1', 'a.bin'))
    age_tree(os.path.join(out, 'scratch1'), old)
    o = run(m.cmd_auto, 'hourly', False, None)
    check('hourly pass with a live session deletes nothing from out\\', exists('out/scratch1/a.bin'), o[-300:])
    o = run(m.cmd_auto, 'start', False, None)
    check('--auto start deletes nothing from out\\', exists('out/scratch1/a.bin'), o[-300:])
    other = ['def67890-0000', now - 7200, now - 5]
    os.environ['TIDY_TEST_LIVE'] = json.dumps([me, other])
    o = run(m.cmd_auto, 'end', False, 'abc12345')
    check('session end with ANOTHER live session leaves out\\ alone', exists('out/scratch1/a.bin') and 'another session is live' in o, o[-300:])
    os.environ['TIDY_TEST_LIVE'] = json.dumps([me])
    o = run(m.cmd_auto, 'end', False, 'abc12345')
    check('twin: session end with only this session live clears out\\', not exists('out/scratch1'), o[-300:])
    reset()
    write(os.path.join(out, 'scratch2', 'a.bin'))
    age_tree(os.path.join(out, 'scratch2'), old)
    o = run(m.cmd_auto, 'hourly', False, None)
    check('twin: hourly with no live session clears out\\', not exists('out/scratch2'), o[-300:])

    # ---- 7. --now (emergency, C: under the reserve): idle scratch goes while a session is live, recent files stay, build / keep list stay
    reset()
    os.environ['TIDY_TEST_LIVE'] = json.dumps([me])
    os.environ['TIDY_TEST_FREE'] = '10'
    m.LOCKED = True
    write(os.path.join(out, 'build', 'win-amd64-release', 'wwe2k14.exe'))
    write(os.path.join(out, 'build', 'win-amd64-release', 'logs', 'old.log'))
    write(os.path.join(out, 'old_scratch', 'a.bin'))
    write(os.path.join(out, 'new_scratch', 'a.bin'))
    write(os.path.join(out, 'kept', 'k.bin'))
    open(keep, 'w').write('out/kept | needed | \n')
    m._held_cache.clear()
    age_tree(os.path.join(out, 'build'), old)
    age_tree(os.path.join(out, 'old_scratch'), old)
    age_tree(os.path.join(out, 'kept'), old)
    o = run(m.cmd_now, False, 'now', 100)
    check('--now: out\\build (SACRED) intact, including an old log', exists('out/build/win-amd64-release/wwe2k14.exe') and exists('out/build/win-amd64-release/logs/old.log'), o[-400:])
    check('--now: idle scratch goes', not exists('out/old_scratch'), o[-400:])
    check('--now: a file made in the last 2 h stays while a session is live', exists('out/new_scratch/a.bin'), o[-400:])
    check('--now: the keep list path stays', exists('out/kept/k.bin'))
    m.LOCKED = False

    # ---- 8. the Deleter backstop: even a hand-made plan naming out\build is refused
    reset()
    write(os.path.join(out, 'build', 'x.exe'))
    d = m.Deleter(time.time() + 5)
    d.remove([{'p': os.path.join(out, 'build'), 'b': 100, 'n': 1, 't': 0, 'types': {'folders': 100}, 'dir': True}], [])
    check('--apply backstop: a plan naming out\\build deletes nothing', exists('out/build/x.exe'))


    # ---- 9. fix 1: indexes / profiles that need GAME RUNS to remake are held
    reset()
    for p_ in ('harness/index/image.bin', 'harness/index/index.db', 'harness/labelling/a.json', 'harness/determinism/det_1.csv', 'pgo/guest.profdata',
               'android/game-arm64/obj/x.o', 'android/launcher-arm64/y.o', 'harness/other/z.txt'):
        write(os.path.join(out, *p_.split('/')))
        age_tree(os.path.dirname(os.path.join(out, *p_.split('/'))), old)
    o = run(m.cmd_auto, 'end', False, None)
    check('fix 1 (planted: the index deleted): harness/index, labelling, determinism, pgo and the android arm64 trees survive session end',
          all(exists('out/' + x) for x in ('harness/index/image.bin', 'harness/index/index.db', 'harness/labelling/a.json', 'harness/determinism/det_1.csv',
                                           'pgo/guest.profdata', 'android/game-arm64/obj/x.o', 'android/launcher-arm64/y.o')), o[-300:])
    check('twin: another harness folder goes', not exists('out/harness/other'))

    # ---- 10. fix 2: FAIL CLOSED liveness; the hourly sweep has an idle cutoff
    reset()
    write(os.path.join(out, 'scratch3', 'old.bin'))
    write(os.path.join(out, 'scratch3b', 'fresh.bin'))
    age_tree(os.path.join(out, 'scratch3'), time.time() - 7 * 3600)
    age_tree(os.path.join(out, 'scratch3b'), time.time() - 3600)
    o = run(m.cmd_auto, 'hourly', False, None)
    check('fix 2: the hourly sweep (no session live) takes only items untouched >= 6 h', not exists('out/scratch3') and exists('out/scratch3b/fresh.bin'), o[-300:])
    real_test, real_proj = m.TEST, m.PROJECTS
    try:
        m.TEST = False
        m.PROJECTS = os.path.join(root, 'no_such_projects_folder')
        live_missing = m.sessions(time.time())
        empty = os.path.join(root, 'empty_projects')
        os.makedirs(empty, exist_ok=True)
        m.PROJECTS = empty
        live_empty = m.sessions(time.time())
        m.PROJECTS = os.path.join(root, 'projects')
        os.makedirs(os.path.join(root, 'projects', m.PROJECT_PREFIX + '-x'), exist_ok=True)
        live_ok = m.sessions(time.time())
    finally:
        m.TEST, m.PROJECTS = real_test, real_proj
    check('fix 2 (planted: PROJECTS missing): sessions() reports something live', bool(live_missing) and live_missing[0][0] == m.UNKNOWN_SESSION, str(live_missing))
    check('fix 2: a projects folder with no project folder is also "unknown = live"', bool(live_empty) and live_empty[0][0] == m.UNKNOWN_SESSION)
    check('twin: a readable projects folder with a project and no live transcript = nothing live', live_ok == [], str(live_ok))
    write(os.path.join(out, 'scratch4', 'old.bin'))
    age_tree(os.path.join(out, 'scratch4'), old)
    try:
        m.TEST = False
        m.PROJECTS = os.path.join(root, 'no_such_projects_folder')
        o = run(m.cmd_auto, 'hourly', False, None)
    finally:
        m.TEST, m.PROJECTS = real_test, real_proj
    check('fix 2: hourly with an unreadable projects folder deletes NOTHING', exists('out/scratch4/old.bin'), o[-300:])
    check('LIVE_IDLE_H is at least 12 h', m.LIVE_IDLE_H >= 12)

    # ---- 11. fix 3: this session's own running agent at session end
    reset()
    now = time.time()
    me = ['abc12345-0000', now - 3600, now - 5]
    os.environ['TIDY_TEST_LIVE'] = json.dumps([me])
    sub_dir = os.path.join(root, 'projects', 'proj', 'abc12345-0000', 'subagents')
    write(os.path.join(out, 'agentwork', 'half.bin'))
    write(os.path.join(out, 'agentold', 'a.bin'))
    age_tree(os.path.join(out, 'agentwork'), now - 600)      # idle 10 min: an agent may still be writing it
    age_tree(os.path.join(out, 'agentold'), now - 7200)
    write(os.path.join(sub_dir, 'agent-zz.meta.json'), json.dumps({'worktreePath': os.path.join(root, '.claude', 'worktrees', 'agent-zz')}))
    write(os.path.join(sub_dir, 'agent-zz.jsonl'), '{}')
    o = run(m.cmd_auto, 'end', False, 'abc12345')
    check('fix 3 (planted: a running agent at session end): a file idle 10 min stays, one idle 2 h goes', exists('out/agentwork/half.bin') and not exists('out/agentold'), o[-400:])
    write(os.path.join(sub_dir, 'agent-zz.meta.json'), json.dumps({'worktreePath': 'x', 'finished': True}))
    o = run(m.cmd_auto, 'end', False, 'abc12345')
    check('twin: the agent finished: the 10-minute-idle folder goes too', not exists('out/agentwork'), o[-300:])
    shutil.rmtree(os.path.join(root, 'projects', 'proj'))

    # ---- 12. fix 5: plain worktrees (out\wt, bisect, sdk-wt) wait 6 h, honour .owner and a running agent
    reset()
    os.environ['TIDY_TEST_LIVE'] = json.dumps([me])
    env = dict(os.environ, GIT_AUTHOR_NAME='t', GIT_AUTHOR_EMAIL='t@t', GIT_COMMITTER_NAME='t', GIT_COMMITTER_EMAIL='t@t')
    subprocess.run(['git', '-C', root, 'commit', '-q', '--allow-empty', '-m', 'c'], capture_output=True, env=env)
    os.makedirs(os.path.join(out, 'wt'), exist_ok=True)

    def mkwt(name):
        subprocess.run(['git', '-C', root, 'worktree', 'add', '-q', '-b', name, os.path.join(out, 'wt', name)], capture_output=True, env=env)
        write(os.path.join(out, 'wt', name, 'pad.bin'))
        subprocess.run(['git', '-C', os.path.join(out, 'wt', name), 'add', '-A'], capture_output=True, env=env)
        subprocess.run(['git', '-C', os.path.join(out, 'wt', name), 'commit', '-q', '-m', 'p'], capture_output=True, env=env)
        subprocess.run(['git', '-C', root, 'merge', '-q', name], capture_output=True, env=env)      # merged into the session branch
    for n_ in ('bis1h', 'bis7h', 'bisown'):
        mkwt(n_)
    check('test setup: three worktrees exist', all(exists('out/wt/' + n_ + '/pad.bin') for n_ in ('bis1h', 'bis7h', 'bisown')))
    age_tree(os.path.join(out, 'wt', 'bis1h'), time.time() - 3600)
    age_tree(os.path.join(out, 'wt', 'bis7h'), time.time() - 7 * 3600)
    age_tree(os.path.join(out, 'wt', 'bisown'), time.time() - 7 * 3600)
    open(os.path.join(out, 'wt', 'bisown') + '.owner', 'w').write('pid=%d\n' % os.getpid())
    o = run(m.cmd_auto, 'end', False, 'abc12345')
    check('fix 5 (planted: a 1 h idle bisect worktree): it stays', exists('out/wt/bis1h/pad.bin'), o[-400:])
    check('fix 5: a .owner pid that is alive keeps a 7 h idle worktree', exists('out/wt/bisown/pad.bin'))
    check('twin: a 7 h idle merged worktree goes', not exists('out/wt/bis7h'), o[-400:])

    # ---- 13. fix 6: a nested clone (a .git DIRECTORY) is never swept
    reset()
    write(os.path.join(out, 'nested', '.git', 'HEAD'), 'ref: refs/heads/x')
    write(os.path.join(out, 'nested', 'src', 'a.cpp'))
    write(os.path.join(out, 'plainx', 'a.bin'))
    age_tree(os.path.join(out, 'nested'), old)
    age_tree(os.path.join(out, 'plainx'), old)
    o = run(m.cmd_auto, 'end', False, None)
    check('fix 6 (planted: a nested clone): a folder holding a .git directory is not swept', exists('out/nested/src/a.cpp') and exists('out/nested/.git/HEAD'), o[-300:])
    check('twin: a plain folder goes', not exists('out/plainx'))

    # ---- 14. fix 7: an explicit keep-list entry on an unpack copy holds it; a lane name does not
    reset()
    write(os.path.join(out, 'leaf2', 'fview_unpack_7', 'capture', 'images.bin'))
    write(os.path.join(out, 'leaf2', 'fview_unpack_8', 'junk.bin'))
    write(os.path.join(out, 'leaf2', 'plain.bin'))
    open(keep, 'w').write('out/leaf2/fview_unpack_7 | the only copy of a capture I need while the case zip is lost | \n')
    lane(['out/leaf2'])
    m._held_cache.clear()
    o = run(m.cmd_auto, 'end', False, None)
    check('fix 7 (planted: a held unpack copy): a keep-list entry overrides the scratch pattern', exists('out/leaf2/fview_unpack_7/capture/images.bin'), o[-400:])
    check('twin: the other unpack copy in the same gate-named folder goes', not exists('out/leaf2/fview_unpack_8'), o[-500:])
    check('the named leaf keeps its own files', exists('out/leaf2/plain.bin'))

    print('%d checks, %d failed' % (N, len(FAILS)))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
