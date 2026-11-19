"""q's hard timeout (QTF, S103 WTA fix 5): no q call runs longer than WWE_Q_TIMEOUT seconds (default 120; 0 = off).

  start_watchdog(secs)   a daemon thread; when it fires it kills every child process of this process (the clang parse pool, a refresh, an overlay
                         build) and exits 124 with a line saying so. Imported by q.py's __main__ and by tools/q/qours.py.
  run_with_deadline(fn, secs, cancel=None)  run fn() in a daemon thread; on expiry set `cancel`, kill the children and return (False, None) so the caller can answer from the
                         index it already has (qours: the overlay build / refresh); else (True, result) (or re-raise fn's exception).

    py tools\\q\\qtimeout.py --selftest
"""
import os
import sys
import threading
import time

DEFAULT_SECS = 120


def mark_owner():
    """QTF review C3: opt-in ownership for tools\\harness\\proc_sweep.py: this process's children (parse pool, clang, refresh) carry WWE_SWEEP_OWNER=<pid>:<create_time>;
    when this process dies they become sweepable orphans. Nothing without the variable is ever swept."""
    try:
        import psutil
        os.environ['WWE_SWEEP_OWNER'] = '%d:%.0f' % (os.getpid(), psutil.Process().create_time())
    except Exception:
        pass


def timeout_secs():
    try:
        return float(os.environ.get('WWE_Q_TIMEOUT', DEFAULT_SECS))
    except ValueError:
        return float(DEFAULT_SECS)


def kill_children():
    """kill every descendant of this process (whole tree), not this process itself; returns how many were killed."""
    n = 0
    try:
        import psutil
        me = psutil.Process()
        kids = me.children(recursive=True)
        for k in kids:
            try:
                k.kill()
                n += 1
            except psutil.Error:
                pass
        psutil.wait_procs(kids, timeout=5)
    except ImportError:
        if os.name == 'nt':
            import subprocess
            subprocess.run(['taskkill', '/F', '/T', '/FI', f'PID ne {os.getpid()}', '/FI', f'PPID eq {os.getpid()}'], capture_output=True)
    return n


def start_watchdog(secs=None, what='q'):
    secs = timeout_secs() if secs is None else secs
    if not secs or secs <= 0:
        return None

    def fire():
        time.sleep(secs)
        n = kill_children()
        try:
            sys.stderr.write(f'{what}: TIMEOUT after {secs:g} s (WWE_Q_TIMEOUT): killed {n} child process(es) and stopped. '
                             f'The question is too big for one call: narrow it, or raise WWE_Q_TIMEOUT.\n')
            sys.stderr.flush()
        except Exception:
            pass
        os._exit(124)
    t = threading.Thread(target=fire, name='q-watchdog', daemon=True)
    t.start()
    return t


def run_with_deadline(fn, secs, cancel=None):
    box = {}

    def work():
        try:
            box['r'] = fn()
        except BaseException as e:   # noqa: BLE001 - handed back to the caller's thread
            box['e'] = e
    t = threading.Thread(target=work, name='q-deadline', daemon=True)
    t.start()
    t.join(secs)
    if t.is_alive():
        if cancel is not None:
            cancel.set()          # QTF review C2: the worker rolls back / drops its copy and returns (build_ours checks it before every commit, the merge and the swap)
        kill_children()
        t.join(5)
        return False, None
    if 'e' in box:
        raise box['e']
    return True, box.get('r')


def selftest():
    import subprocess
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(here)), 'tools', 'gate'))
    import proc
    pidf = os.path.join(os.environ.get('TEMP', '.'), f'qtimeout_gc_{os.getpid()}.pid')
    code = ('import sys, os, subprocess, time\n'
            f'sys.path.insert(0, {here!r})\n'
            'import qtimeout\n'
            'qtimeout.start_watchdog(4)\n'
            'g = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(300)"])\n'
            f'open({pidf!r}, "w").write(str(g.pid))\n'
            'time.sleep(300)\n')
    t0 = time.time()
    r = proc.run([sys.executable, '-c', code], timeout=60)
    dt = time.time() - t0
    assert r.returncode == 124 and not r.timed_out, (r, r.stderr)
    assert 'TIMEOUT after 4' in r.stderr, r.stderr
    assert 3 <= dt < 20, dt
    import psutil
    gpid = int(open(pidf).read())
    time.sleep(0.5)
    assert not psutil.pid_exists(gpid), 'planted sleep-300 child survived'
    os.remove(pidf)
    # run_with_deadline: a hung worker returns (False, None) and kills its child
    def hang():
        p = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(300)'])
        time.sleep(300)
        return p
    t0 = time.time()
    ok, _ = run_with_deadline(hang, 2)
    assert not ok and time.time() - t0 < 10
    ok, v = run_with_deadline(lambda: 7, 5)
    assert ok and v == 7
    print(f'qtimeout selftest OK: a planted sleep-300 child was killed {dt:.1f} s after start (watchdog 4 s), q exited 124 with the TIMEOUT line; deadline helper ok')
    return 0


if __name__ == '__main__':
    if '--selftest' in sys.argv:
        sys.exit(selftest())
    print(__doc__)
