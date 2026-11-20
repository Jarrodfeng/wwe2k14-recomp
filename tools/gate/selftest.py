# entry: cmake_state
# Selftest for the gate's exe handling (Session 101 lane 163). py -I tools\gate\selftest.py
# Plants, all in a temp tree with a fake exe and a fake build script (the real build outputs are never touched, no game, no window):
#   1 stale exe is rebuilt once         2 fresh exe is not rebuilt          3 exe held by a running process -> named BUILD failure (pid + command line)
#   4 two concurrent builders build once                                    5 a check whose build fails shows BUILD (not FAIL, not a PASS)
#   6 nested build / link error printed by a check -> BUILD; an ordinary failing check stays FAIL
#   7 past-bug re-find (movie_ours, 2026-11-12): a --no-build style check on an exe older than the source FAILs under the old gate (--no-exes) and
#     PASSes under the new one, which rebuilds first.
import json, os, re, shutil, subprocess, sys, tempfile, threading, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import exes as gexes

PY = sys.executable
FAILS = []


def check(ok, text):
    print(('ok    ' if ok else 'FAIL  ') + text, flush=True)
    if not ok:
        FAILS.append(text)


BUILD_PY = r'''
import os, sys, time
root = os.getcwd()
mode = sys.argv[1] if len(sys.argv) > 1 else 'ok'
open(os.path.join(root, 'builds.txt'), 'a').write('x\n')
if mode == 'slow':
    time.sleep(2.0)
if mode == 'fail':
    print('src/a.cpp:1:1: error: expected something'); print('fview build (exit 1)'); sys.exit(1)
try:
    f = open(os.path.join(root, 'out', 'tool.exe'), 'wb')
except PermissionError:
    print('link failed: permission denied'); sys.exit(1)
f.write(open(os.path.join(root, 'src', 'a.cpp'), 'rb').read()); f.close()
print('built tool')
'''


def make_tree(base, name, mode='ok'):
    root = Path(base) / name
    (root / 'src').mkdir(parents=True)
    (root / 'out').mkdir()
    (root / 'tools' / 'gate' / 'lanes').mkdir(parents=True)
    (root / 'src' / 'a.cpp').write_text('v1')
    (root / 'build.py').write_text(BUILD_PY)
    exe = root / 'out' / 'tool.exe'
    exe.write_text('v0')
    old = time.time() - 3600
    os.utime(exe, (old, old))
    spec = {'path': 'out/tool.exe', 'build': [PY, '-I', 'build.py', mode], 'timeout_s': 120, 'sources': {'dirs': ['src'], 'files': []}}
    return root, spec


def builds(root):
    p = root / 'builds.txt'
    return len(p.read_text().split()) if p.exists() else 0


def hold_exe(path):
    # open the exe with FILE_SHARE_READ only, like a running image: any write open fails with a sharing violation (permission denied)
    import ctypes
    k = ctypes.windll.kernel32
    k.CreateFileW.restype = ctypes.c_void_p
    h = k.CreateFileW(str(path), 0x80000000, 1, None, 3, 0x80, None)
    return h, k


def gate_run(root, extra=()):
    p = subprocess.run([PY, '-I', str(HERE / 'gate.py'), '--root', str(root), '--out', str(root / 'gate_out'), '--no-toolcache'] + list(extra), capture_output=True, text=True, timeout=300)
    return p.returncode, p.stdout + p.stderr


def main():
    base = tempfile.mkdtemp(prefix='gate_selftest_')
    try:
        # 1 + 2
        root, spec = make_tree(base, 't12')
        st, tx = gexes.ensure(root, spec, wait_s=2, poll=0.1)
        check(st == 'built' and builds(root) == 1 and (root / 'out' / 'tool.exe').read_text() == 'v1', f'1 stale exe rebuilt once ({st}: {tx})')
        st, tx = gexes.ensure(root, spec, wait_s=2, poll=0.1)
        check(st == 'fresh' and builds(root) == 1, f'2 fresh exe not rebuilt ({st}: {tx})')
        # touching a source makes it stale again; the stamp keeps a no-op build from looping
        later = time.time() + 5
        os.utime(root / 'src' / 'a.cpp', (later, later))
        st, tx = gexes.ensure(root, spec, wait_s=2, poll=0.1)
        check(st == 'built' and builds(root) == 2, f'2b touched source -> rebuilt ({st})')

        # 2c S103 GX: the tool cache. Twin checkout, same sources: the first builds and publishes, the second COPIES (no build); a source change misses.
        import toolcache as gtc
        cdir = Path(base) / 'tcache'
        r1, sp1 = make_tree(base, 'tc1')
        r2, sp2 = make_tree(base, 'tc2')
        r3, sp3 = make_tree(base, 'tc3')
        (r3 / 'src' / 'a.cpp').write_text('v1-edited')
        for r in (r1, r2, r3):
            (r / 'out' / 'tool.exe').unlink()
        q = lambda *_a, **_k: None
        st1, tx1 = gexes.ensure(r1, sp1, wait_s=2, poll=0.1, cache=gtc.ToolCache(r1, cdir, log=q))
        st2, tx2 = gexes.ensure(r2, sp2, wait_s=2, poll=0.1, cache=gtc.ToolCache(r2, cdir, log=q))
        check(st1 == 'built' and 'built, cached' in tx1 and st2 == 'cached' and 'cache hit' in tx2 and builds(r2) == 0 and (r2 / 'out' / 'tool.exe').read_text() == 'v1',
              f'2c twin checkout copies the cached exe, no build ({st1}: {tx1[:60]} / {st2}: {tx2[:60]}, builds {builds(r2)})')
        st3, tx3 = gexes.ensure(r3, sp3, wait_s=2, poll=0.1, cache=gtc.ToolCache(r3, cdir, log=q))
        check(st3 == 'built' and builds(r3) == 1 and (r3 / 'out' / 'tool.exe').read_text() == 'v1-edited', f'2d a changed source misses and builds ({st3})')

        # 2e S102 08:56 re-find (renderer_adapter missing from the source list): a header OUTSIDE the spec's source dirs changed. The mtime list cannot see it;
        # the cache key hashes the whole src / tools tree, so a cold checkout with that change must MISS (and build), never copy the old exe.
        r4, sp4 = make_tree(base, 'tc4')
        (r4 / 'out' / 'tool.exe').unlink()
        (r4 / 'tools' / 'unlisted.h').write_text('changed outside the spec sources')
        st4, tx4 = gexes.ensure(r4, sp4, wait_s=2, poll=0.1, cache=gtc.ToolCache(r4, cdir, log=q))
        check(st4 == 'built' and builds(r4) == 1, f'2e a change outside the spec source list still misses the cache ({st4})')

        # 3 held by a running process: a copy of ping.exe is the "exe", running
        root, spec = make_tree(base, 't3')
        ping = Path(os.environ.get('SystemRoot', 'C:/Windows')) / 'System32' / 'PING.EXE'
        shutil.copy(ping, root / 'out' / 'tool.exe')
        old = time.time() - 3600
        os.utime(root / 'out' / 'tool.exe', (old, old))
        proc = subprocess.Popen([str(root / 'out' / 'tool.exe'), '-n', '30', '127.0.0.1'], stdout=subprocess.DEVNULL)
        try:
            time.sleep(0.5)
            t0 = time.time()
            st, tx = gexes.ensure(root, spec, wait_s=1.5, poll=0.2, retry_s=0.4)
            check(st == 'BUILD' and f'pid {proc.pid}' in tx and 'held by' in tx and 'tool.exe -n 30' in tx, f'3 held exe -> named BUILD failure ({tx[:200]})')
            check(proc.poll() is None, '3b the holder was not killed')
            check(not (root / 'out' / 'tool.exe.lock').exists(), '3c the lock is released after a failed build')
        finally:
            proc.kill()
            proc.wait()
        # 3d: an open handle without a process still names the cause
        root, spec = make_tree(base, 't3d')
        h, k = hold_exe(root / 'out' / 'tool.exe')
        try:
            st, tx = gexes.ensure(root, spec, wait_s=1, poll=0.2, retry_s=0.4)
            check(st == 'BUILD' and 'held by' in tx, f'3d open handle -> BUILD "held by" ({tx[:160]})')
        finally:
            k.CloseHandle(ctypes_void(h))

        # stale lock of a dead pid is taken over
        root, spec = make_tree(base, 't3e')
        (root / 'out' / 'tool.exe.lock').write_text(json.dumps({'pid': 2147483000}))
        st, tx = gexes.ensure(root, spec, wait_s=2, poll=0.1)
        check(st == 'built', f'3e a lock held by a dead pid is stale and removed ({st})')

        # 4 two concurrent builders: one builds (slow build), the other waits on the lock and finds it fresh
        root, spec = make_tree(base, 't4', mode='slow')
        results = []
        driver = ("import sys, json; sys.path.insert(0, %r); import exes; s = json.loads(sys.argv[2]); st, tx = exes.ensure(sys.argv[1], s, wait_s=5, poll=0.1); print(st, '|', tx)"
                  % str(HERE))
        procs = [subprocess.Popen([PY, '-I', '-c', driver, str(root), json.dumps(spec)], stdout=subprocess.PIPE, text=True) for _ in range(2)]
        outs = [p.communicate(timeout=120)[0].strip() for p in procs]
        check(builds(root) == 1 and sorted(o.split(' ')[0] for o in outs) == ['built', 'fresh'], f'4 two concurrent runs build once ({outs})')

        # 5 + 6 + 7 through the gate itself
        root, spec = make_tree(base, 't5', mode='fail')
        lane = {'checks': [{'name': 'x.check', 'cmd': [PY, '-I', 'chk.py'], 'exes': [spec], 'timeout_s': 60}]}
        (root / 'chk.py').write_text('print("check ran")')
        (root / 'tools' / 'gate' / 'lanes' / 'x.json').write_text(json.dumps(lane))
        rc, out = gate_run(root)
        line = [l for l in out.splitlines() if l.startswith('BUILD') and 'x.check' in l]
        check(rc == 1 and line and 'out/tool.exe' in line[0] and 'build failed' in line[0], f'5 failed build -> BUILD line naming the exe ({line[:1]})')
        check('check ran' not in out and 'pass or skip, 1 fail (1 of them BUILD' in out, '5b the check is not run on a missing exe; the summary counts BUILD')

        root, spec = make_tree(base, 't6')
        (root / 'chk_link.py').write_text('import sys; print("link failed: permission denied"); sys.exit(1)')
        (root / 'chk_nested.py').write_text('print("FAIL fview build (exit 1)"); print("PASS everything else")')
        (root / 'chk_real.py').write_text('print("FAIL wrong colour at 3,4"); raise SystemExit(1)')
        lane = {'checks': [{'name': 'x.link', 'cmd': [PY, '-I', 'chk_link.py'], 'timeout_s': 60},
                           {'name': 'x.nested', 'cmd': [PY, '-I', 'chk_nested.py'], 'timeout_s': 60},
                           {'name': 'x.real', 'cmd': [PY, '-I', 'chk_real.py'], 'timeout_s': 60}]}
        (root / 'tools' / 'gate' / 'lanes' / 'x.json').write_text(json.dumps(lane))
        rc, out = gate_run(root)
        v = {l.split()[2]: l.split()[0] for l in out.splitlines() if l.split()[:1] and l.split()[0] in ('PASS', 'FAIL', 'BUILD')}
        check(v.get('x.link') == 'BUILD' and v.get('x.nested') == 'BUILD' and v.get('x.real') == 'FAIL', f'6 nested link error -> BUILD, nested "FAIL fview build" in a PASS -> BUILD, real failure stays FAIL ({v})')

        # 7 re-find: the check passes only when the exe was built from the current source
        root, spec = make_tree(base, 't7')
        (root / 'src' / 'a.cpp').write_text('v2-the-merge')
        chk = ("import sys\nexe = open('out/tool.exe').read()\nsrc = open('src/a.cpp').read()\n"
               "print('PASS exe matches source' if exe == src else 'FAIL exe is %r, source is %r (stale exe)' % (exe, src))\nsys.exit(0 if exe == src else 1)\n")
        (root / 'chk.py').write_text(chk)
        lane = {'checks': [{'name': 'movie.proof', 'cmd': [PY, '-I', 'chk.py'], 'exes': [spec], 'timeout_s': 60}]}
        (root / 'tools' / 'gate' / 'lanes' / 'x.json').write_text(json.dumps(lane))
        rc_old, out_old = gate_run(root, ['--no-exes'])
        old_line = [l for l in out_old.splitlines() if 'movie.proof' in l]
        os.utime(root / 'out' / 'tool.exe', (time.time() - 3600,) * 2)
        rc_new, out_new = gate_run(root)
        new_line = [l for l in out_new.splitlines() if 'movie.proof' in l or l.startswith('[gate]')]
        check(rc_old == 1 and old_line and old_line[0].startswith('FAIL'), f'7a old behaviour: check FAILs on the stale exe ({old_line[:1]})')
        check(rc_new == 0 and any('built fresh out/tool.exe' in l for l in new_line) and any(l.startswith('PASS') for l in new_line), f'7b new behaviour: rebuilt first, check PASSes ({new_line})')
        s105_rows(base)
    finally:
        shutil.rmtree(base, ignore_errors=True)
    print('selftest: ' + ('PASS' if not FAILS else 'FAIL (%d)' % len(FAILS)))
    return 1 if FAILS else 0


def s105_rows(base):
    """S105 E rows. entry: game_syntax.cmake_content_unchanged_since / cmake_state (row 8), gate.py main verdict + exit code, gate_wait.summarise (row 9)."""
    import game_syntax as gs
    env = dict(os.environ, GIT_AUTHOR_NAME='t', GIT_AUTHOR_EMAIL='t@t', GIT_COMMITTER_NAME='t', GIT_COMMITTER_EMAIL='t@t')
    git = lambda d, *a: subprocess.run(['git', '-C', str(d)] + list(a), env=env, capture_output=True, text=True)
    # 8 NEEDS-MAIN stamp: content, not mtime (entry: game_syntax.cmake_state, the function --changed calls)
    main = Path(base) / 'gs_main'
    (main / 'out' / 'b').mkdir(parents=True)
    (main / 'CMakeLists.txt').write_text('project(x)\n')
    (main / 'CMakePresets.json').write_text('{}')
    git(main, 'init', '-q')
    git(main, 'add', '-A')
    subprocess.run(['git', '-C', str(main), 'commit', '-q', '-m', 'one'], env=dict(env, GIT_COMMITTER_DATE='2020-01-01T00:00:00'), capture_output=True)   # committer time = what %ct reads: older than the ninja
    ninja = main / 'out' / 'b' / 'build.ninja'
    ninja.write_text('n')
    gs.BUILD_DIRS['on'] = 'out/b'
    t_old = time.time()
    os.utime(ninja, (t_old, t_old))
    time.sleep(2.2)   # the reflog has one-second resolution: later steps must land in a later second
    new = time.time() + 5
    os.utime(main / 'CMakeLists.txt', (new, new))   # touched: newer mtime, same bytes
    check(gs.cmake_state(str(main), str(main), ['on']) == [], '8a PLANTED twin: CMakeLists.txt mtime newer than build.ninja, content unchanged = no NEEDS-MAIN reason')
    (main / 'CMakeLists.txt').write_text('project(x)\nadd_subdirectory(y)\n')   # content changed (uncommitted)
    r = gs.cmake_state(str(main), str(main), ['on'])
    check(any('build.ninja is older' in x for x in r), f'8b content changed after the ninja was written = NEEDS-MAIN reason kept ({r})')
    git(main, 'add', '-A')
    git(main, 'commit', '-q', '-m', 'two')   # committed now: its commit time is newer than the ninja
    r = gs.cmake_state(str(main), str(main), ['on'])
    check(any('build.ninja is older' in x for x in r), f'8c content changed and committed after the ninja was written = NEEDS-MAIN reason kept ({r})')
    # 8d review 1 (the unsafe direction): CMakeLists.txt changed on a BRANCH (commit time T1), build.ninja written at T2 > T1 from a checkout WITHOUT that change, the branch merged
    # at T3 > T2 (`git log -1 -- file` skips the merge and reports T1 < T2: "unchanged"): the alarm must stay.
    m2 = Path(base) / 'gs_main2'
    (m2 / 'out' / 'b').mkdir(parents=True)
    (m2 / 'CMakeLists.txt').write_text('project(x)\n')
    git(m2, 'init', '-q')
    git(m2, 'add', '-A')
    git(m2, 'commit', '-q', '-m', 'one')
    git(m2, 'checkout', '-q', '-b', 'side')
    (m2 / 'CMakeLists.txt').write_text('project(x)\nadd_subdirectory(y)\n')
    git(m2, 'commit', '-q', '-am', 'side change')
    git(m2, 'checkout', '-q', '-')
    ninja2 = m2 / 'out' / 'b' / 'build.ninja'
    ninja2.write_text('n')
    t_n = time.time()
    os.utime(ninja2, (t_n, t_n))
    time.sleep(2.2)
    git(m2, 'merge', '-q', '--no-ff', '-m', 'merge side', 'side')
    later = time.time() + 5
    os.utime(m2 / 'CMakeLists.txt', (later, later))
    r = gs.cmake_state(str(m2), str(m2), ['on'])
    check(any('build.ninja is older' in x for x in r), f'8d PLANTED: CMakeLists changed on a branch, merged AFTER build.ninja was written = NEEDS-MAIN reason kept ({r})')
    # 9 BLOCKED verdict: a check whose program cannot start is BLOCKED (own word, exit 3), a real failure stays FAIL (exit 1)
    root = Path(base) / 'blk'
    (root / 'tools' / 'gate' / 'lanes').mkdir(parents=True)
    (root / 'ok.py').write_text('print("PASS")')
    (root / 'bad.py').write_text('print("FAIL wrong"); raise SystemExit(1)')
    lane = {'checks': [{'name': 'b.nostart', 'cmd': ['no_such_program_s105e.exe'], 'timeout_s': 60, 'tier': 'cheap', 'gpu': False},
                       {'name': 'b.ok', 'cmd': [PY, '-I', 'ok.py'], 'timeout_s': 60, 'tier': 'cheap', 'gpu': False}]}
    (root / 'tools' / 'gate' / 'lanes' / 'b.json').write_text(json.dumps(lane))
    rc, out = gate_run(root)
    rows = {l.split()[2]: l.split()[0] for l in out.splitlines() if l.split()[:1] and l.split()[0] in ('PASS', 'FAIL', 'BLOCKED', 'BUILD')}
    check(rc == 3 and rows.get('b.nostart') == 'BLOCKED' and 'of them BLOCKED' in out, f'9a a program that cannot start = BLOCKED row, summary names it, gate exit 3 ({rc}, {rows})')
    lane['checks'].append({'name': 'b.real', 'cmd': [PY, '-I', 'bad.py'], 'timeout_s': 60, 'tier': 'cheap', 'gpu': False})
    (root / 'tools' / 'gate' / 'lanes' / 'b.json').write_text(json.dumps(lane))
    rc, out = gate_run(root)
    check(rc == 1 and re.search(r'^FAIL\s+\S+\s+b\.real', out, re.M), f'9b a real failure beside a BLOCKED row is still FAIL, exit 1 ({rc})')
    # gate_wait: BLOCKED-only gate = 3, never 1; with a FAIL = 1
    d = Path(base) / 'gw'
    d.mkdir()
    log = d / 'g.log'
    wait = [PY, '-I', str(HERE / 'gate_wait.py'), '--attach', str(log), '--timeout', '20']
    log.write_text('PASS  l  a  1.0 s  x\nBLOCKED  l  b  0.0 s  cannot start\ngate: 1 of 2 pass or skip, 1 fail (1 of them BLOCKED: the check could not run, not a verdict on the code), 1.0 s wall\n')
    p = subprocess.run(wait, capture_output=True, text=True)
    check(p.returncode == 3 and 'BLOCKED  l  b' in p.stdout, f'9c gate_wait on a BLOCKED-only gate exits 3 and prints the BLOCKED row ({p.returncode})')
    log.write_text('BLOCKED  l  b  0.0 s  x\nFAIL  l  c  1.0 s  boom\ngate: 0 of 2 pass or skip, 2 fail (1 of them BLOCKED: x), 1.0 s wall\n')
    p = subprocess.run(wait, capture_output=True, text=True)
    check(p.returncode == 1, f'9d gate_wait: a FAIL beside a BLOCKED row exits 1 ({p.returncode})')
    # 10 item 7: a twin that was caught ends on PLANTED-OK, never on an inner FAIL line; one that was NOT caught fails and does not say PLANTED-OK (entry: tools/release/expect_fail.py main)
    ef = str(HERE.parent / 'release' / 'expect_fail.py')
    p = subprocess.run([PY, '-I', ef, PY, '-c', 'print("FAIL inner: the planted fault"); raise SystemExit(1)'], capture_output=True, text=True)
    last = (p.stdout.strip().splitlines() or [''])[-1]
    check(p.returncode == 0 and last.startswith('PLANTED-OK:'), f'10a a caught twin (inner prints FAIL, exits 1): wrapper exit 0, LAST line PLANTED-OK ({p.returncode}: {last[:70]})')
    p = subprocess.run([PY, '-I', ef, PY, '-c', 'print("PASS inner")'], capture_output=True, text=True)
    check(p.returncode == 1 and 'PLANTED-OK' not in p.stdout, f'10b a twin that was NOT caught (inner exits 0): wrapper exit 1, no PLANTED-OK ({p.returncode})')
    p = subprocess.run([PY, '-I', ef, PY, '-c', 'raise RuntimeError("x")'], capture_output=True, text=True)
    check(p.returncode == 1 and 'PLANTED-OK' not in p.stdout, f'10c a twin that CRASHED (traceback, exit 1) is not a caught fault: wrapper exit 1, no PLANTED-OK ({p.returncode})')
    print('PLANTED-OK: touched-not-changed CMakeLists is not NEEDS-MAIN; changed content still is; a could-not-run check is BLOCKED (exit 3), never FAIL; a real FAIL stays exit 1')


def ctypes_void(h):
    import ctypes
    return ctypes.c_void_p(h)


if __name__ == '__main__':
    sys.exit(main())
