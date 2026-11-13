# Selftest for the gate's exe handling (Session 101 lane 163). py -I tools\gate\selftest.py
# Plants, all in a temp tree with a fake exe and a fake build script (the real build outputs are never touched, no game, no window):
#   1 stale exe is rebuilt once         2 fresh exe is not rebuilt          3 exe held by a running process -> named BUILD failure (pid + command line)
#   4 two concurrent builders build once                                    5 a check whose build fails shows BUILD (not FAIL, not a PASS)
#   6 nested build / link error printed by a check -> BUILD; an ordinary failing check stays FAIL
#   7 past-bug re-find (movie_ours, 2026-11-12): a --no-build style check on an exe older than the source FAILs under the old gate (--no-exes) and
#     PASSes under the new one, which rebuilds first.
import json, os, shutil, subprocess, sys, tempfile, threading, time
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
    p = subprocess.run([PY, '-I', str(HERE / 'gate.py'), '--root', str(root), '--out', str(root / 'gate_out')] + list(extra), capture_output=True, text=True, timeout=300)
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
    finally:
        shutil.rmtree(base, ignore_errors=True)
    print('selftest: ' + ('PASS' if not FAILS else 'FAIL (%d)' % len(FAILS)))
    return 1 if FAILS else 0


def ctypes_void(h):
    import ctypes
    return ctypes.c_void_p(h)


if __name__ == '__main__':
    sys.exit(main())
