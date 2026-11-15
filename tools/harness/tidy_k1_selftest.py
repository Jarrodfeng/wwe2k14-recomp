"""S102 lane K1 selftest for tools\\harness\\tidy.py and tidy_disk.py. 0 game runs, 0 deletes outside a fake tree under the checkout's out\\k1_selftest.

  py -I tools\\harness\\tidy_k1_selftest.py

Every case has a planted twin that must come out the other way (a check that cannot fail proves nothing). The first case re-finds the
2026-11-13 bug: out/s100 (39.8 GB) was held because a lane named out/s100/attires.
"""
import ctypes
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
WORK = os.path.join(ROOT, 'out', 'k1_selftest')
FAILS, N = [], 0

k32 = ctypes.windll.kernel32
k32.CreateFileW.restype = ctypes.c_void_p
k32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
k32.SetFileTime.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
k32.CloseHandle.argtypes = [ctypes.c_void_p]


def set_times(path, t):
    ft = ctypes.c_ulonglong(int((t + 11644473600) * 1e7))
    flag = 0x02000000     # backup semantics: works on folders too
    h = k32.CreateFileW('\\\\?\\' + os.path.abspath(path), 0x100, 7, None, 3, flag, None)
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


def write(p, text='x', binary=None):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    if binary is not None:
        open(p, 'wb').write(binary)
    else:
        open(p, 'w', encoding='utf-8').write(text)


def load_tidy(root):
    """tidy.py imported as a module against a fake root (its paths are fixed at import)."""
    os.environ['TIDY_TEST_ROOT'] = root
    os.environ['TIDY_TEST_PROJECTS'] = os.path.join(root, 'projects')
    os.makedirs(os.path.join(root, 'projects'), exist_ok=True)
    spec = importlib.util.spec_from_file_location('tidy_under_test', os.path.join(HERE, 'tidy.py'))
    m = importlib.util.module_from_spec(spec)
    sys.modules['tidy_under_test'] = m
    spec.loader.exec_module(m)
    return m


def main():
    if os.path.isdir(WORK):
        shutil.rmtree(WORK)
    os.makedirs(WORK)
    sys.path.insert(0, HERE)
    import tidy_disk as td
    now = time.time()
    day = 86400.0

    # ---- 1. strip_py: comments / docstrings / pragma lines are not inputs (R12 change 2)
    src = ('"""docs mention out/docstring_only/x"""\n'
           'import os\n'
           '# comment names out/comment_only/y\n'
           "H = os.path.join(REPO, 'out', 's100', 'harness')  # tidy: output\n"
           "R = os.path.join(REPO, 'out', 'real_input', 'cap')\n")
    st = td.strip_py(src)
    check('strip_py drops docstring', 'docstring_only' not in st)
    check('strip_py drops comment', 'comment_only' not in st)
    check('strip_py drops pragma line', 's100' not in st)
    check('strip_py keeps a real input (twin)', 'real_input' in st)
    check('strip_py unparsable text falls back to raw', 'def (' in td.strip_py('def ('))

    # ---- fake repo
    R = os.path.join(WORK, 'repo')
    os.makedirs(os.path.join(R, 'out', 'tidy'))
    os.makedirs(os.path.join(R, 'tools', 'gate', 'lanes'))
    write(os.path.join(R, 'tools', 'gate', 'lanes', 'lane_a.json'),
          json.dumps({'checks': [{'name': 'a', 'cmd': ['py', 'tools/x_check.py', 'out/s100x/attires'], 'needs': ['out/gated/capture']}]}))
    write(os.path.join(R, 'tools', 'x_check.py'), src)
    T = load_tidy(R)

    # ---- 2. KEEP rules (R12 change 1): the out/s100 re-find
    def keep(rel, text, age_days):
        d = os.path.join(R, 'out', *rel.split('/'))
        os.makedirs(d, exist_ok=True)
        write(os.path.join(d, 'KEEP'), text)
        set_times(os.path.join(d, 'KEEP'), time.time() - age_days * day)
        T._keep_cache.clear()
        return T.keep_info(d)
    i = keep('s100x', 'Session 100: shader packets, local only', 9)
    check('RE-FIND out/s100: a longer path named by a lane (out/s100x/attires) does NOT hold the parent', not i['valid'], str(i))
    i = keep('s100x/attires', 'gate input', 9)
    check('twin: the lane that names the folder itself holds it', i['valid'] and 'named' in i['kind'], str(i))
    i = keep('bare_old', '# todo', 9)
    check('bare KEEP (comment only), 9 days old: NOT honoured', not i['valid'], str(i))
    i = keep('bare_new', '# todo', 2)
    check('twin: the same bare KEEP, 2 days old: the 7-day hold', i['valid'], str(i))
    i = keep('dated', 'for the S102 check, until %s' % time.strftime('%Y-%m-%d', time.localtime(now + 5 * day)), 30)
    check('KEEP with a reason and a future date: honoured', i['valid'], str(i))
    i = keep('dated_old', 'for the S102 check, until %s' % time.strftime('%Y-%m-%d', time.localtime(now - 5 * day)), 30)
    check('twin: the same with a past date: not honoured', not i['valid'], str(i))
    i = keep('tool_ref', 'read by tools/x_check.py', 30)
    check('a reason pointing at a tool that does not name the folder: not honoured', not i['valid'], str(i))
    # the docstring/comment in x_check.py names out/docstring_only: a KEEP there must not be honoured by a docstring
    i = keep('docstring_only', 'x', 30)
    check('a folder named only in a docstring is not "named"', not i['valid'], str(i))

    # ---- 3. strict age: a live session's START no longer widens it (R12 change 3)
    live = [('abc', now - 70 * 3600, now)]
    age, strict = T.make_ages(now, live)
    check('strict cutoff with a session open 70 h = now - 24 h (was its start)', abs(strict.cutoff - (now - 24 * 3600)) < 5, '%.1f h' % ((now - strict.cutoff) / 3600))
    check('low pass strict = now - 1 h', abs(T.make_ages(now, live, low=True)[1].cutoff - (now - 3600)) < 5)

    # ---- 4. a gate-named capture older than 72 h is kept; an unnamed twin is not
    cap = os.path.join(R, 'out', 'gated', 'capture')
    write(os.path.join(cap, 'images.bin'), binary=b'a' * 100)
    write(os.path.join(cap, 'manifest.json'), '{}')
    age_tree(os.path.join(R, 'out', 'gated'), now - 100 * 3600)
    other = os.path.join(R, 'out', 'ungated', 'capture')
    write(os.path.join(other, 'images.bin'), binary=b'a' * 100)
    age_tree(os.path.join(R, 'out', 'ungated'), now - 100 * 3600)
    bad = dict(T.guard_paths([cap]))
    check('gate-named capture, 100 h old, is refused by the guard', bool(bad.get(cap)), str(bad))
    bad = dict(T.guard_paths([other]))
    check('twin: an unnamed capture of the same age is not refused', not bad.get(other), str(bad))
    protect, _ = T.protect_keys()
    check('protect_keys names out/gated/capture', 'out/gated/capture' in protect or 'out/gated' in protect or any(k.startswith('out/gated') for k in protect))
    check('JOIN false positive gone: a dict key after a joined path is not part of it',
          not any('j1' in k for k in _join_keys(T, "{'a': MAIN / 'out' / 'x' / 'capture',\n 'j1': ROOT / 'out'}")), '')
    check('twin: the real joined path is still found', 'out/x/capture' in _join_keys(T, "A = MAIN / 'out' / 'x' / 'capture'"))

    # ---- 5. duplicate captures reported once
    c1, c2, c3 = (os.path.join(WORK, 'dup', n) for n in ('c1', 'c2', 'c3'))
    for c, payload in ((c1, b'SAME' * 1000), (c2, b'SAME' * 1000), (c3, b'DIFF' * 1000)):
        write(os.path.join(c, 'images.bin'), binary=payload)
        write(os.path.join(c, 'manifest.json'), '{"n": 1}')
    g = td.find_dupes([c1, c2, c3])
    check('duplicate capture set reported once (2 of 3)', len(g) == 1 and sorted(g[0][1]) == sorted([c1, c2]), str(g))
    L, saved = td.dupe_lines(g, lambda p: p == c2, lambda b: '%d B' % b)
    check('the gate-named copy is the one kept', 'keep ' + c2 in L[0] and 'duplicate: ' + c1 in L[0] and saved == 4008, L[0])
    write(os.path.join(c2, 'images.bin'), binary=b'SAMF' + b'SAME' * 999)
    check('twin: one changed byte -> no duplicate', td.find_dupes([c1, c2, c3]) == [])

    # ---- 6. free-space log, rate of fall, reserve
    rows = [(now - 3600 - 60, 200.0), (now - 1800, 190.0), (now - 600, 181.0)]
    rate, mins = td.burn_rate(rows, 170.0, now)
    check('rate of fall: 30 GB in 61 min', 29 < rate < 31 and 60 < mins < 62, '%s %s' % (rate, mins))
    line, hours = td.disk_line(170.0, rows, now, 40)
    check('[disk] line says when the reserve is reached', hours is not None and 4 < hours < 5 and '[disk]' in line and 'GB/h' in line, line)
    line, hours = td.disk_line(170.0, [(now - 4000, 150.0)], now, 40)
    check('twin: free space rising -> no countdown', hours is None, line)
    td.log_free(os.path.join(R, 'out', 'tidy'), 123.4, now)
    check('free.csv written and read back', td.read_free(os.path.join(R, 'out', 'tidy'))[-1][1] == 123.4)
    env = dict(os.environ, TIDY_TEST_ROOT=R, TIDY_TEST_FREE='10')
    env.pop('TIDY_REPO', None)
    r = subprocess.run([sys.executable, os.path.join(HERE, 'tidy.py'), '--reserve-ok'], env=env, capture_output=True, text=True)
    check('--reserve-ok exits 1 with 10 GB free', r.returncode == 1 and '[disk]' in r.stdout, r.stdout + r.stderr)
    env['TIDY_TEST_FREE'] = '100'
    r = subprocess.run([sys.executable, os.path.join(HERE, 'tidy.py'), '--reserve-ok'], env=env, capture_output=True, text=True)
    check('twin: --reserve-ok exits 0 with 100 GB free', r.returncode == 0, r.stdout + r.stderr)

    # ---- 7. per-worktree quota
    wt = os.path.join(WORK, 'wt')
    o = os.path.join(wt, 'out')
    for n, size in (('big', 3000), ('mid', 2000), ('small', 1000), ('recent', 4000)):
        write(os.path.join(o, n, 'f.bin'), binary=b'z' * size)
        age_tree(os.path.join(o, n), now - (5 if n != 'recent' else 0) * 60 * 60)
    names, left = T.quota_pick(o, 7000, now - 15 * 60)
    check('quota: the biggest idle children go until under quota, a recent one never', names == ['big'] and left == 7000 and 'recent' not in names, '%s %s' % (names, left))
    names, _ = T.quota_pick(o, 100000, now - 15 * 60)
    check('twin: under quota -> nothing picked', names == [])
    check('wt_quota_gb defaults to %d' % T.SCRATCH_QUOTA_GB, T.wt_quota_gb(wt) == T.SCRATCH_QUOTA_GB)
    write(os.path.join(wt, '.tidy_quota'), '9\n')
    check('.tidy_quota raises it', T.wt_quota_gb(wt) == 9)

    # ---- 8. scratchpad rule (R12 change 5)
    sp = os.path.join(WORK, 'pads')
    os.environ['TIDY_TEST_SCRATCHPADS'] = sp
    os.environ['TIDY_TEST_LIVE'] = json.dumps([['livesess', now - 5 * 3600, now - 60]])
    for sid in ('deadsess', 'livesess'):
        for child, age_h in (('old_case', 20), ('new_case', 1)):
            write(os.path.join(sp, sid, 'scratchpad', child, 'f.bin'), binary=b'q' * 1000)
            age_tree(os.path.join(sp, sid, 'scratchpad', child), now - age_h * 3600)
    write(os.path.join(sp, 'deadsess', 'tasks', 't.txt'), 'task')
    age_tree(os.path.join(sp, 'deadsess', 'tasks'), now - 40 * 3600)
    total, lines = T.scratchpad_pass(now, dry=True)
    check('scratchpad dry: only the idle session\'s old child counts (1000 B)', total == 1000, str(total))
    total, lines = T.scratchpad_pass(now, dry=False)
    check('scratchpad: old child of the idle session removed', not os.path.exists(os.path.join(sp, 'deadsess', 'scratchpad', 'old_case')))
    check('twin: its recent child stays', os.path.isdir(os.path.join(sp, 'deadsess', 'scratchpad', 'new_case')))
    check('twin: the LIVE session\'s old child stays', os.path.isdir(os.path.join(sp, 'livesess', 'scratchpad', 'old_case')))
    check('the session folder\'s tasks stay', os.path.isfile(os.path.join(sp, 'deadsess', 'tasks', 't.txt')))
    check('the budget measures idle sessions\' scratchpads too', any(k.startswith('scratchpad\\deadsess'[:19]) for k, _ in T.top_level_sizes()))

    # ---- 9. shared lock slot (R12 change 7)
    shared = 'mode: SHARED (2 of 3 slots)\nslot1: held by x\nslot2: held by y\nbuild1: held by z\ngame processes: none\n'
    os.environ['TIDY_TEST_LOCKSTATUS'] = shared
    check('shared holders block the strict check (old behaviour)', bool(T.lock_busy()))
    check('shared holders do NOT block a pass that takes its own shared slot', T.lock_busy(shared_ok=True) == '')
    os.environ['TIDY_TEST_LOCKSTATUS'] = 'mode: EXCLUSIVE\ngame processes: none\n'
    check('twin: an EXCLUSIVE hold (timed run) still blocks it', bool(T.lock_busy(shared_ok=True)))
    os.environ['TIDY_TEST_LOCKSTATUS'] = 'mode: free\ngame processes: wwe2k14.exe (pid 1)\n'
    check('twin: a game process still blocks it', bool(T.lock_busy(shared_ok=True)))
    os.environ.pop('TIDY_TEST_LOCKSTATUS')

    # ---- 10. regenerable inputs are not "missing"; a lost capture is
    write(os.path.join(R, 'tools', 'build_it.ps1'), '#')
    write(os.path.join(R, 'tools', 'gate', 'regenerable.json'), json.dumps({'out/built': 'tools/build_it.ps1', 'out/gated2': 'tools/nope.ps1'}))
    write(os.path.join(R, 'tools', 'gate', 'lanes', 'lane_b.json'), json.dumps({'needs': ['out/built', 'out/gated2/capture', 'out/lostcap/capture']}))
    miss = [m[0] for m in T.missing_gate_inputs()]
    check('a build output with a recipe on disk is not MISSING', 'out/built' not in miss, str(miss))
    check('twin: a recipe whose file is gone is MISSING', 'out/gated2/capture' in miss or 'out/gated2' in miss, str(miss))
    check('twin: a lost capture (no recipe) is MISSING', 'out/lostcap/capture' in miss, str(miss))

    # ---- 11. review fixes (K1 review, 2026-11-14)
    # (1) the quota never touches a RUNNING agent's out\ and never a folder holding compiled output; a FINISHED agent over quota is picked
    wt2 = os.path.join(WORK, 'wt2')
    o2 = os.path.join(wt2, 'out')
    write(os.path.join(o2, 'renderer', 'dev', 'fview.exe'), binary=b'e' * 1000)
    write(os.path.join(o2, 'renderer', 'obj', 'x.obj'), binary=b'o' * 1000)
    write(os.path.join(o2, 'cases', 'f.bin'), binary=b'c' * 5000)
    age_tree(o2, now - 20 * 60)
    write(os.path.join(wt2, '.tidy_quota'), '0.000004\n')      # 4000 bytes
    names, _ = T.quota_pick(o2, 4000, now - 15 * 60)
    check('quota_pick skips a folder holding .exe / .obj (a builder\'s tree) and takes the idle cases', names == ['cases'], str(names))

    class _R:
        stdout = ''
        returncode = 0
    saved = {k: getattr(T, k) for k in ('scratch_dirs', 'agent_infos', 'agent_state', 'session_branch', 'git', 'is_merged', 'scratch_skip_reason', 'tracked_under_out')}
    T.scratch_dirs = lambda: [({'path': wt2, 'head': None}, o2)]
    T.agent_infos = lambda: []
    T.session_branch = lambda: 'b'
    T.git = lambda *a, **k: _R()
    T.is_merged = lambda *a: True
    T.scratch_skip_reason = lambda *a: ''
    T.tracked_under_out = lambda wt: set()
    try:
        T.agent_state = lambda infos, live_ids, path, n, h: (n, True, [{'finished': False, 'tr': 0, 'meta_t': 0}])
        got, lines = T.scratch_pass(now, dry=True, free=200.0)
        check('RUNNING agent over its quota, C: has 200 GB: nothing is picked', got == 0, '%s %s' % (got, lines))
        T.agent_state = lambda infos, live_ids, path, n, h: (n, False, [{'finished': True, 'tr': 0, 'meta_t': 0}])
        got, lines = T.scratch_pass(now, dry=True, free=200.0)
        check('twin: a FINISHED agent over its quota: the idle cases folder is picked, the build folder is not', got == 5000, '%s %s' % (got, lines))
    finally:
        for k, v in saved.items():
            setattr(T, k, v)

    # (2) scratchpad pass: junction, KEEP, loose files, no LOCALAPPDATA; ON by default since Jarrod's yes (2026-11-14 01:02)
    check('the scratchpad delete pass is ON by default (Jarrod, S102)', T.SCRATCHPAD_PASS is True)
    os.environ.pop('TIDY_TEST_SCRATCHPADS')
    tgt = os.path.join(WORK, 'jtarget')
    write(os.path.join(tgt, 'old.bin'), binary=b'j' * 500)
    age_tree(tgt, now - 40 * 3600)
    jroot = os.path.join(WORK, 'pads2')
    os.makedirs(os.path.join(jroot, 'deadjunc'))
    subprocess.run(['cmd', '/c', 'mklink', '/J', os.path.join(jroot, 'deadjunc', 'scratchpad'), tgt], capture_output=True)
    write(os.path.join(jroot, 'deadkeep', 'scratchpad', 'old_case', 'f.bin'), binary=b'k' * 500)
    age_tree(os.path.join(jroot, 'deadkeep', 'scratchpad'), now - 40 * 3600)
    write(os.path.join(jroot, 'deadkeep', 'scratchpad', 'KEEP'), 'until %s' % time.strftime('%Y-%m-%d', time.localtime(now + 3 * day)))
    write(os.path.join(jroot, 'deadloose', 'scratchpad', 'loose.bin'), binary=b'l' * 500)
    age_tree(os.path.join(jroot, 'deadloose', 'scratchpad'), now - 40 * 3600)
    os.environ['TIDY_TEST_SCRATCHPADS'] = jroot
    check('test setup: the scratchpad junction exists', os.path.isfile(os.path.join(jroot, 'deadjunc', 'scratchpad', 'old.bin')))
    T.scratchpad_pass(now, dry=False)
    check('scratchpad that is a junction: its target survives', os.path.isfile(os.path.join(tgt, 'old.bin')))
    check('scratchpad with an active KEEP (future until): survives', os.path.isfile(os.path.join(jroot, 'deadkeep', 'scratchpad', 'old_case', 'f.bin')))
    check('twin: an old loose file in a plain idle scratchpad goes', not os.path.exists(os.path.join(jroot, 'deadloose', 'scratchpad', 'loose.bin')))
    os.rmdir(os.path.join(jroot, 'deadjunc', 'scratchpad'))
    os.environ.pop('TIDY_TEST_SCRATCHPADS')
    saved_env = os.environ.pop('LOCALAPPDATA', None)
    was = T.TEST
    T.TEST = False
    try:
        check('no LOCALAPPDATA: no scratchpad is found (never a relative glob)', T.scratchpad_sessions() == [])
    finally:
        T.TEST = was
        if saved_env is not None:
            os.environ['LOCALAPPDATA'] = saved_env

    # (4) shared lock only for --scratch and the watch
    class _A:
        def __init__(self, **k):
            self.auto, self.now, self.scratch = None, False, False
            self.__dict__.update(k)
    check('shared_ok: the watch', T.lock_shared_ok(_A(auto='watch')))
    check('shared_ok: --scratch', T.lock_shared_ok(_A(scratch=True)))
    check('twin: the hourly pass keeps the strict check', not T.lock_shared_ok(_A(auto='hourly')))
    check('twin: --now keeps the strict check', not T.lock_shared_ok(_A(now=True)))
    os.environ['TIDY_TEST_LOCKSTATUS'] = 'mode: BUILD only\nbuild1: held by x\ngame processes: none\n'
    check('hourly pass with "mode: BUILD only" is skipped (leaves an expired live_X under out\\harness\\bisect\\exe)', bool(T.lock_busy(shared_ok=T.lock_shared_ok(_A(auto='hourly')))))
    check('twin: the watch is not stopped by it', T.lock_busy(shared_ok=T.lock_shared_ok(_A(auto='watch'))) == '')
    os.environ.pop('TIDY_TEST_LOCKSTATUS')

    # (5) regenerable.json is an exact key
    write(os.path.join(R, 'tools', 'gate', 'regenerable.json'), json.dumps({'out/ring': 'tools/build_it.ps1'}))
    write(os.path.join(R, 'tools', 'gate', 'lanes', 'lane_c.json'), json.dumps({'needs': ['out/ring', 'out/ring/capX/capture']}))
    miss = [m[0] for m in T.missing_gate_inputs()]
    check('a lane naming out/ring/capX/capture while out/ring is listed: still MISSING', 'out/ring/capx/capture' in miss, str(miss))
    check('twin: out/ring itself (exact key, recipe on disk) is not', 'out/ring' not in miss, str(miss))

    print('\n%d checks, %d failed%s' % (N, len(FAILS), (': ' + ', '.join(FAILS)) if FAILS else ''))
    shutil.rmtree(WORK, ignore_errors=True)
    return 1 if FAILS else 0


def _join_keys(T, text):
    found = {}
    T._scan_text(text, found, 'test')
    return list(found)


if __name__ == '__main__':
    sys.exit(main())
