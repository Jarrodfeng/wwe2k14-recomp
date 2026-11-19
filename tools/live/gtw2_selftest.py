"""GTW2: offline test of the goto / live start fixes from GTS (docs/agents/s103/GTS.md). 0 game runs; a fake status channel
replaces live.request (the pipe) and the game launch (subprocess.Popen); the production entry functions run.

  py -I tools/live/gtw2_selftest.py

1. live.start(script menu): returns only at busy no + seq 75 + menu 1 ("at the main menu"); twin: a game that answers at the title
   (seq 3) or while busy does not return started; idle returns at the first answer; the timeout text names what it saw;
2. resolve.execute (a nav step) on seq 3 stops with "--script menu"; twin: seq 75 goes on to Nav.go;
3. goto.main: a second goto on a held lock (live pid) is refused; twin: no lock -> both run; a dead pid's lock is replaced;
   the lock is gone after the run.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tools', 'goto'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'atlas'))
import live  # noqa: E402
import golock  # noqa: E402
import goto  # noqa: E402
import resolve  # noqa: E402
import nav  # noqa: E402

fails = []


def check(ok, what):
    print('%s  %s' % ('ok  ' if ok else 'FAIL', what))
    if not ok:
        fails.append(what)


class FakeGame:
    """A scripted `!status` channel: answers[i] is the answer to the i-th call (the last repeats); None = no game."""
    def __init__(self, answers):
        self.answers, self.calls = answers, 0

    def request(self, name, text, connect_wait=10.0):
        a = self.answers[min(self.calls, len(self.answers) - 1)]
        self.calls += 1
        if a is None:
            raise live.NoGame('no game')
        return a


def status(busy, states, frame=100):
    return 'status: ok\nrun: 044\nframe: %d\nbusy: %s\nstates: %s\ntext: x\n.\n' % (frame, busy, states)


MENU = status('no', 'seq=75 scene_word=1 menu=1 cursor=0')
TITLE = status('no', 'seq=3 menu=0')
BOOT = status('yes', 'seq=3 menu=0')
BUSY_MENU = status('yes', 'seq=75 menu=1')


def run_start(answers, script='menu', launch='', boot_wait=1):
    g = FakeGame([None] + list(answers))   # the first call of start() is the "already running" probe: no game yet
    saved = (live.request, subprocess.Popen, live.OUT, live.EXE_DIR_FILE, live.exe_defines, live.wait_started)
    tmp = tempfile.mkdtemp()
    live.request, live.OUT, live.EXE_DIR_FILE = g.request, tmp, os.path.join(tmp, 'exe_dir.txt')
    live.exe_defines = lambda *a: False
    subprocess.Popen = lambda *a, **k: None   # no game is launched
    orig = live.wait_started
    live.wait_started = lambda n, s, l, f, p, bw=180, poll_s=0.5: orig(n, s, l, f, p, boot_wait, 0.01)
    try:
        return live.start('t_gtw2', script, launch, True), g
    finally:
        (live.request, subprocess.Popen, live.OUT, live.EXE_DIR_FILE, live.exe_defines, live.wait_started) = saved


def part1():
    t, g = run_start([BOOT, BOOT, TITLE, BUSY_MENU, MENU])
    check(t.startswith('started') and 'at the main menu' in t and g.calls == 6, 'menu start returns only at seq 75 / menu 1 / not busy: ' + t)
    t, g = run_start([BOOT, TITLE])
    check(t.startswith('answers but not at the main menu') and 'seq=3' in t, 'twin: stuck at the title -> not "started", names what it saw: ' + t)
    t, g = run_start([BOOT])
    check(not t.startswith('started') and 'busy yes' in t, 'twin: busy forever -> not started: ' + t)
    t, g = run_start([BOOT], script='idle')
    check(t.startswith('started') and 'at the main menu' not in t and g.calls == 2, 'idle returns at the first answer (unchanged): ' + t)
    t, g = run_start([BOOT], launch='x')
    check(t.startswith('started') and 'at the main menu' not in t, 'a launch scenario returns at the first answer: ' + t)
    t, g = run_start([None])
    check(t.startswith('no answer after'), 'no answer keeps its message: ' + t)
    t, g = run_start([MENU])
    check(t.startswith('already running') or t.startswith('started'), 'sanity: ' + t)


def batch_fake(seq):
    def run_batch(name, lines, timeout=120, shot=False, width=1280):
        log = []
        for i, ln in enumerate(lines, 1):
            if ln.startswith('log_state'):
                key = ln.split()[1]
                v = seq if key == 'seq' else 0
                log.append('[x] [wwe2k14][live] line %d (%s): state %s = 0x%X (%d)' % (i, ln, key, v, v))
        return {'status': 'ok'}, log, []
    return run_batch


def part2():
    plan = resolve.resolve('menu:list:1')
    went = []
    saved = (live.run_batch, nav.Nav.go)
    nav.Nav.go = lambda self, target: (went.append(target) or iter(['went']))
    try:
        live.run_batch = batch_fake(3)
        try:
            resolve.execute(plan, 't_gtw2', say=lambda *a: None)
            check(False, 'seq 3 stops the goto')
        except resolve.Stopped as x:
            check('--script menu' in str(x) and not went, 'seq 3 (title): goto stops with the message, presses nothing: %s' % x)
        live.run_batch = batch_fake(75)
        try:
            resolve.execute(plan, 't_gtw2', say=lambda *a: None)
        except resolve.Stopped as x:
            check(False, 'seq 75 stopped: %s' % x)
        check(len(went) == 1, 'twin: seq 75 -> Nav.go runs: %s' % went)
    finally:
        live.run_batch, nav.Nav.go = saved


def part3():
    tmp = tempfile.mkdtemp()
    saved = (live.OUT, resolve.execute, resolve.failure_report)
    live.OUT = tmp
    ran = []

    def fake_execute(plan, name, say=print):
        ran.append(name)
        check(os.path.exists(golock.lock_path(name)), 'the lock exists while the goto runs')
    resolve.execute = fake_execute
    try:
        argv = ['menu:list:1', '--name', 'lk']
        check(goto.main(argv) == 0 and ran == ['lk'] and not os.path.exists(golock.lock_path('lk')), 'twin: no lock -> runs, lock cleared on exit')
        child = subprocess.Popen([sys.executable, '-c', 'import sys; sys.stdin.read()'], stdin=subprocess.PIPE)  # a live pid until killed
        try:
            with open(golock.lock_path('lk'), 'w') as f:
                f.write(str(child.pid))
            ran.clear()
            check(goto.main(argv) == 1 and not ran, 'a held lock (live pid) refuses the second goto')
            check(os.path.exists(golock.lock_path('lk')), "the holder's lock is left alone")
            check(goto.main(['menu:list:1', '--name', 'other']) == 0 and ran == ['other'], 'twin: another live name is free')
        finally:
            child.kill()
            child.wait()
        ran.clear()
        check(goto.main(argv) == 0 and ran == ['lk'], "a dead pid's lock is replaced")

        def stopping(plan, name, say=print):
            raise resolve.Stopped('x')
        resolve.execute = stopping
        resolve.failure_report = lambda n, m: 'stopped'
        goto.main(argv)
        check(not os.path.exists(golock.lock_path('lk')), 'lock cleared after a Stopped goto')
    finally:
        live.OUT, resolve.execute, resolve.failure_report = saved


part1()
part2()
part3()
print('FAILED: %d' % len(fails) if fails else 'all passed')
sys.exit(1 if fails else 0)
