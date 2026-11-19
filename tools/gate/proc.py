"""proc.run(cmd, timeout): run a child under a hard timeout, in a Windows Job Object (kill-on-close), so the WHOLE tree dies with it.

    from proc import run          (tools/gate on sys.path)
    r = run(['clang++', ...], timeout=900, cwd=None, env=None, capture=True, text=True)   -> Result(returncode, stdout, stderr, timed_out)

QTF (S103, WTA fix 6): game_syntax.py had four subprocess calls with no timeout (a 10-hour hang); `timeout 900` of the shell killed `py`
but not the clang++ grandchild. Here the child is put in a job with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: on timeout (or when this process
dies, even by a hard kill) Windows kills every process in the job. On timeout run() returns returncode 124 with timed_out=True (it does NOT
raise), so callers keep their own failure paths. Off Windows it falls back to a process-group kill.

Details (QTF reviews):
  * The child is created SUSPENDED, assigned to the job, then resumed (NtResumeProcess, its NTSTATUS checked, in a finally): no gap in which a grandchild
    could escape. If the assignment fails (nested job) ONE line goes to stderr and the child runs unjobbed (a timeout kills it with taskkill /T only).
    The job handle is closed on every path, also when Popen raises.
  * When the call ends, each pid the job lists is opened, confirmed to be IN THIS JOB (IsProcessInJob) and terminated through that handle, so a reused
    pid is never hit. Everything runs through ctypes: without psutil the cleanup falls back to TerminateJobObject + CloseHandle, and never raises.
  * Job kill kills EVERY process in the job, shared daemons included. The named DAEMONS (sccache, adb) are left running and the kill-on-close flag is
    cleared before the job handle closes. Pass keep=... for another one.
  * Output: the pipes are read by threads. Once the child has exited, a grandchild that still holds a pipe open (a daemon that did not detach) gets ~5 s
    (PIPE_GRACE_S) to finish; then reading stops and what was read so far is returned, so run() never waits on a daemon forever.
  * Every child's environment carries WWE_SWEEP_OWNER=<this pid>:<create time> (tools/harness/proc_sweep.py's opt-in ownership).

    py tools\\gate\\proc.py --selftest
"""
import os
import subprocess
import sys
import threading
import time

TIMEOUT_RC = 124
PIPE_GRACE_S = 5.0
# shared helpers a compile may start: never killed with the job
DAEMONS = {'sccache', 'adb'}


class Result:
    def __init__(self, args, returncode, stdout, stderr, timed_out):
        self.args, self.returncode, self.stdout, self.stderr, self.timed_out = args, returncode, stdout, stderr, timed_out

    def __repr__(self):
        return f'Result(rc={self.returncode}, timed_out={self.timed_out})'


if os.name == 'nt':
    import ctypes
    from ctypes import wintypes

    _k32 = ctypes.WinDLL('kernel32', use_last_error=True)

    class _BASIC(ctypes.Structure):
        _fields_ = [('PerProcessUserTimeLimit', ctypes.c_int64), ('PerJobUserTimeLimit', ctypes.c_int64), ('LimitFlags', wintypes.DWORD),
                    ('MinimumWorkingSetSize', ctypes.c_size_t), ('MaximumWorkingSetSize', ctypes.c_size_t),
                    ('ActiveProcessLimit', wintypes.DWORD), ('Affinity', ctypes.c_size_t), ('PriorityClass', wintypes.DWORD),
                    ('SchedulingClass', wintypes.DWORD)]

    class _IO(ctypes.Structure):
        _fields_ = [(n, ctypes.c_uint64) for n in ('ReadOperationCount', 'WriteOperationCount', 'OtherOperationCount', 'ReadTransferCount',
                                                   'WriteTransferCount', 'OtherTransferCount')]

    class _EXT(ctypes.Structure):
        _fields_ = [('BasicLimitInformation', _BASIC), ('IoInfo', _IO), ('ProcessMemoryLimit', ctypes.c_size_t), ('JobMemoryLimit', ctypes.c_size_t),
                    ('PeakProcessMemoryUsed', ctypes.c_size_t), ('PeakJobMemoryUsed', ctypes.c_size_t)]

    class _PIDLIST(ctypes.Structure):
        _fields_ = [('NumberOfAssignedProcesses', wintypes.DWORD), ('NumberOfProcessIdsInList', wintypes.DWORD),
                    ('ProcessIdList', ctypes.c_size_t * 512)]

    _k32.CreateJobObjectW.restype = wintypes.HANDLE
    _k32.CreateJobObjectW.argtypes = [wintypes.LPVOID, wintypes.LPCWSTR]
    _k32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD]
    _k32.QueryInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD, wintypes.LPVOID]
    _k32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    _k32.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
    _k32.IsProcessInJob.argtypes = [wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL)]
    _k32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    _k32.CloseHandle.argtypes = [wintypes.HANDLE]
    _k32.OpenProcess.restype = wintypes.HANDLE
    _k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    _k32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    _ntdll = ctypes.WinDLL('ntdll')
    _ntdll.NtResumeProcess.argtypes = [wintypes.HANDLE]
    _ntdll.NtResumeProcess.restype = ctypes.c_long
    _PROCESS_ASSIGN = 0x0100 | 0x0001                    # SET_QUOTA | TERMINATE
    _PROCESS_END = 0x0001 | 0x1000                       # TERMINATE | QUERY_LIMITED_INFORMATION
    _KILL_ON_CLOSE = 0x2000
    _JOB_EXT_INFO = 9
    _CREATE_SUSPENDED = 0x4

    def _job_pids(job):
        info = _PIDLIST()
        if not _k32.QueryInformationJobObject(job, 3, ctypes.byref(info), ctypes.sizeof(info), None):
            return []
        return list(info.ProcessIdList[:info.NumberOfProcessIdsInList])

    def _image_name(h):
        buf = ctypes.create_unicode_buffer(1024)
        n = wintypes.DWORD(1024)
        if _k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)):
            return os.path.basename(buf.value).lower().removesuffix('.exe')
        return ''

    def _end_job(job, keep):
        """kill everything in the job except the named daemons, each through its own handle after IsProcessInJob (no pid-reuse kill); if a daemon
        lives, clear kill-on-close so it survives the handle close. Any failure falls back to TerminateJobObject; never raises."""
        try:
            survivors = 0
            for pid in _job_pids(job):
                h = _k32.OpenProcess(_PROCESS_END, False, pid)
                if not h:
                    continue
                try:
                    inj = wintypes.BOOL(0)
                    if not (_k32.IsProcessInJob(h, job, ctypes.byref(inj)) and inj.value):
                        continue                                # the pid now belongs to something else: not ours
                    if _image_name(h) in keep:
                        survivors += 1
                    else:
                        _k32.TerminateProcess(h, 1)
                finally:
                    _k32.CloseHandle(h)
            if survivors:
                info = _EXT()                                   # LimitFlags 0: closing the handle no longer kills the daemon
                if not _k32.SetInformationJobObject(job, _JOB_EXT_INFO, ctypes.byref(info), ctypes.sizeof(info)):
                    raise OSError('could not clear kill-on-close (error %d)' % ctypes.get_last_error())
            else:
                _k32.TerminateJobObject(job, 1)
        except Exception as e:                                  # noqa: BLE001
            sys.stderr.write('proc.run: job cleanup failed (%r): terminating the whole job\n' % (e,))
            try:
                _k32.TerminateJobObject(job, 1)
            except Exception:
                pass
        finally:
            try:
                _k32.CloseHandle(job)
            except Exception:
                pass

    def _make_job():
        h = _k32.CreateJobObjectW(None, None)
        if not h:
            return None
        info = _EXT()
        info.BasicLimitInformation.LimitFlags = _KILL_ON_CLOSE
        if not _k32.SetInformationJobObject(h, _JOB_EXT_INFO, ctypes.byref(info), ctypes.sizeof(info)):
            _k32.CloseHandle(h)
            return None
        return h

    def _assign(job, pid):
        ph = _k32.OpenProcess(_PROCESS_ASSIGN, False, pid)
        if not ph:
            return False
        try:
            return bool(_k32.AssignProcessToJobObject(job, ph))
        finally:
            _k32.CloseHandle(ph)


def _owned_env(env):
    """the child's environment + WWE_SWEEP_OWNER=<this pid>:<create_time> (opt-in ownership for tools/harness/proc_sweep.py)"""
    e = dict(os.environ if env is None else env)
    try:
        import psutil
        e['WWE_SWEEP_OWNER'] = '%d:%.0f' % (os.getpid(), psutil.Process().create_time())
    except Exception:
        pass
    return e


def _pump(stream, sink):
    try:
        while True:
            b = stream.read1(65536) if hasattr(stream, 'read1') else stream.read(65536)
            if not b:
                break
            sink.append(b)
    except (OSError, ValueError):
        pass


def run(cmd, timeout, cwd=None, env=None, capture=True, text=True, input=None, shell=False, check=False, keep=DAEMONS):
    """run cmd (list or str) with a hard `timeout` (seconds, required); the whole process tree dies on expiry or when this process dies."""
    if timeout is None or timeout <= 0:
        raise ValueError('proc.run needs a positive timeout (seconds)')
    kw = dict(cwd=cwd, env=_owned_env(env), shell=shell)
    if capture:
        kw.update(stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if input is not None:
        kw['stdin'] = subprocess.PIPE
    job = None
    p = None
    try:
        if os.name == 'nt':
            job = _make_job()
            kw['creationflags'] = _CREATE_SUSPENDED          # assigned to the job BEFORE it runs: no gap
            p = subprocess.Popen(cmd, **kw)
            try:
                if job is not None and not _assign(job, p.pid):
                    sys.stderr.write(f'proc.run: could not assign pid {p.pid} to a job object (nested job?): running unjobbed, '
                                     'a timeout kills with taskkill /T only\n')
                    _k32.CloseHandle(job)
                    job = None
            finally:
                st = _ntdll.NtResumeProcess(int(p._handle))
                if st != 0:
                    p.kill()
                    raise OSError('proc.run: NtResumeProcess failed (NTSTATUS 0x%08x): the child was killed' % (st & 0xFFFFFFFF))
        else:
            kw['start_new_session'] = True
            p = subprocess.Popen(cmd, **kw)
        outb, errb = [], []
        threads = []
        for stream, sink in ((p.stdout, outb), (p.stderr, errb)):
            if stream is not None:
                t = threading.Thread(target=_pump, args=(stream, sink), daemon=True)
                t.start()
                threads.append(t)
        if input is not None:
            def feed():
                try:
                    p.stdin.write(input.encode('utf-8', 'replace') if isinstance(input, str) else input)
                    p.stdin.close()
                except (OSError, ValueError):
                    pass
            threading.Thread(target=feed, daemon=True).start()
        timed_out = False
        try:
            p.wait(timeout=timeout)
            deadline = time.time() + PIPE_GRACE_S                # a daemon holding a pipe open must not hold us: ~5 s, then stop reading
        except subprocess.TimeoutExpired:
            timed_out = True
            kill_tree(p)
            if job is not None:
                _end_job_once(job, keep)
                job = None
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
            deadline = time.time() + 10
        for t in threads:
            t.join(max(0.0, deadline - time.time()))
    finally:
        if job is not None:
            _end_job_once(job, keep)      # always: nothing the child left behind survives the call (named daemons excepted)
    rc = TIMEOUT_RC if timed_out else p.returncode

    def fin(chunks):
        b = b''.join(chunks)
        return b.decode('utf-8', 'replace').replace('\r\n', '\n') if text else b
    out, err = (fin(outb), fin(errb)) if capture else (None, None)
    if check and rc != 0:
        raise subprocess.CalledProcessError(rc, cmd, out, err)
    return Result(cmd, rc, out, err, timed_out)


def _end_job_once(job, keep):
    if os.name == 'nt':
        _end_job(job, keep)


def kill_tree(p):
    if os.name == 'nt':
        try:
            subprocess.run(['taskkill', '/F', '/T', '/PID', str(p.pid)], capture_output=True, timeout=30)   # belt and braces (no job)
        except Exception:
            pass
    else:
        import signal
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except OSError:
            pass
    try:
        p.kill()
    except OSError:
        pass


def selftest():
    import tempfile
    d = tempfile.mkdtemp(prefix='proc_selftest_')
    pidf = os.path.join(d, 'gc.pid')
    child = os.path.join(d, 'child.py')
    with open(child, 'w') as f:
        f.write('import subprocess, sys, os, time\n'
                'g = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(300)"])\n'
                f'open({pidf!r}, "w").write(str(g.pid))\n'
                'time.sleep(300)\n')
    t0 = time.time()
    r = run([sys.executable, child], timeout=6)
    dt = time.time() - t0
    assert r.timed_out and r.returncode == TIMEOUT_RC, r
    assert 5 <= dt < 30, dt
    gpid = int(open(pidf).read())
    time.sleep(1.0)
    import psutil
    assert not psutil.pid_exists(gpid), f'grandchild {gpid} survived the timeout'
    ok = run([sys.executable, '-c', 'print("hi")'], timeout=30)
    assert ok.returncode == 0 and ok.stdout.strip() == 'hi' and not ok.timed_out
    bad = run([sys.executable, '-c', 'import sys; sys.exit(3)'], timeout=30)
    assert bad.returncode == 3
    # item 5: without psutil (import blocked) run() works, kills the tree at the timeout and never raises
    saved = sys.modules.get('psutil')
    sys.modules['psutil'] = None
    try:
        r2 = run([sys.executable, child], timeout=4)
        assert r2.timed_out and r2.returncode == TIMEOUT_RC
        ok2 = run([sys.executable, '-c', 'print(7)'], timeout=30)
        assert ok2.stdout.strip() == '7'
    finally:
        sys.modules['psutil'] = saved
    g2 = int(open(pidf).read())
    time.sleep(1.0)
    assert not psutil.pid_exists(g2), 'no-psutil run: grandchild survived'
    # item 5b: the per-pid cleanup itself failing falls back to TerminateJobObject (no raise, tree still dead)
    if os.name == 'nt':
        orig = _job_pids
        globals()['_job_pids'] = lambda job: (_ for _ in ()).throw(RuntimeError('planted failure'))
        try:
            r3 = run([sys.executable, '-c', 'import subprocess,sys;subprocess.Popen([sys.executable,"-c","import time;time.sleep(2e2)  # QTFP5"]);print(1)'], timeout=30)
        finally:
            globals()['_job_pids'] = orig
        time.sleep(0.5)
        left = [p for p in psutil.process_iter() if 'QTFP5' in ' '.join(_cl(p))]
        for p in left:
            p.kill()
        assert r3.returncode == 0 and not left, f'fallback cleanup left {len(left)} process(es)'
    # item 6: a grandchild that keeps the pipe open after the child exits does not hold run() beyond the grace
    t1 = time.time()
    r4 = run([sys.executable, '-c', 'import subprocess,sys;subprocess.Popen([sys.executable,"-c","import time;time.sleep(2e2)  # QTFP6"]);print("hello")'],
             timeout=60, keep={'python'})
    dt4 = time.time() - t1
    for p in psutil.process_iter():
        if 'QTFP6' in ' '.join(_cl(p)):
            p.kill()
    assert r4.stdout.strip() == 'hello' and dt4 < PIPE_GRACE_S + 8, (r4, dt4)
    print(f'proc selftest OK: parent + grandchild (pid {gpid}) dead {dt:.1f} s after start (timeout 6 s); normal runs pass; works without psutil; '
          f'cleanup failure falls back to TerminateJobObject; a pipe-holding daemon released run() after {dt4:.1f} s')
    return 0


def _cl(p):
    try:
        return p.cmdline()
    except Exception:
        return []


if __name__ == '__main__':
    if '--selftest' in sys.argv:
        sys.exit(selftest())
    print(__doc__)
