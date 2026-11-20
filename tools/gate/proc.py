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
    """peak_job_commit / peak_process_commit: bytes (JOBOBJECT_EXTENDED_LIMIT_INFORMATION PeakJobMemoryUsed / PeakProcessMemoryUsed, i.e. the
    peak COMMIT charge of the whole job / of its largest single process), read just before the job is ended; None when no job (off Windows, a
    nested-job failure) or the query failed. CQM 2026-11-20: the measurement admit's reservations rest on."""
    def __init__(self, args, returncode, stdout, stderr, timed_out, peak_job_commit=None, peak_process_commit=None):
        self.args, self.returncode, self.stdout, self.stderr, self.timed_out = args, returncode, stdout, stderr, timed_out
        self.peak_job_commit, self.peak_process_commit = peak_job_commit, peak_process_commit
        # CQM F1: the Store pwsh 7.7 starts its native children OUTSIDE the job it runs in, so for a PowerShell command the job's peak is only
        # PowerShell's own (a near-zero number that is not the command's memory). peak_exact False says so; None = no peak was read.
        self.peak_exact = None if peak_job_commit is None else not _is_powershell(args)

    def __repr__(self):
        return f'Result(rc={self.returncode}, timed_out={self.timed_out})'

    def mem_line(self):
        g = lambda b: 'n/a' if b is None else '%.3f GB' % (b / 2**30)
        if self.peak_exact is False:
            return (f'peak commit not exact (PowerShell starts its children outside the job): the job saw {g(self.peak_job_commit)}, '
                    'measure with tools\\memory\\measure_peak.py')
        return f'peak job commit {g(self.peak_job_commit)}, largest process commit {g(self.peak_process_commit)}'


def _is_powershell(args):
    """the command's program is pwsh / powershell (first list item, or the first token of a string command)"""
    try:
        first = args[0] if isinstance(args, (list, tuple)) else (str(args).strip().split('"')[1] if str(args).strip().startswith('"')
                                                                 else str(args).strip().split()[0])
        return os.path.basename(str(first)).lower().removesuffix('.exe') in ('pwsh', 'powershell')
    except Exception:
        return False


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

    def _job_peaks(job):
        """(PeakJobMemoryUsed, PeakProcessMemoryUsed) in bytes, or (None, None) if the query fails; never raises"""
        try:
            info = _EXT()
            if _k32.QueryInformationJobObject(job, _JOB_EXT_INFO, ctypes.byref(info), ctypes.sizeof(info), None):
                return int(info.PeakJobMemoryUsed), int(info.PeakProcessMemoryUsed)
        except Exception:
            pass
        return None, None

    _k32.GetCurrentProcess.restype = wintypes.HANDLE
    _k32.DuplicateHandle.argtypes = [wintypes.HANDLE, wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.HANDLE), wintypes.DWORD,
                                     wintypes.BOOL, wintypes.DWORD]

    def _dup(h):
        """a second handle to the same kernel object (DUPLICATE_SAME_ACCESS), owned by the caller; None on failure"""
        out = wintypes.HANDLE()
        me = _k32.GetCurrentProcess()
        if _k32.DuplicateHandle(me, h, me, ctypes.byref(out), 0, False, 2):
            return out.value
        return None

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


def close_handle(h):
    """close a handle run() handed to on_start (Windows); a no-op for None / off Windows"""
    if h and os.name == 'nt':
        try:
            _k32.CloseHandle(h)
        except Exception:
            pass


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


def run(cmd, timeout, cwd=None, env=None, capture=True, text=True, input=None, shell=False, check=False, keep=DAEMONS, mem=False,
        on_start=None):
    """run cmd (list or str) with a hard `timeout` (seconds, required); the whole process tree dies on expiry or when this process dies.
    The Result carries the job's peak commit (peak_job_commit / peak_process_commit, bytes; peak_exact False for a PowerShell command, whose
    children run outside the job). mem=True also prints Result.mem_line() to stderr.
    on_start(job, popen): called once the child runs and its output pumps have started, e.g. to start a sampler. `job` is a DUPLICATED handle to
    the job (or None): the callback owns it, may use it after run() returns, and must close it with proc.close_handle(job) (if on_start raises,
    run() closes it). Holding it does not keep the job's processes alive: run() kills them explicitly. on_start must return quickly (start a
    thread): the timeout clock (p.wait) only starts after it returns. Its exceptions are ignored; the child still runs to its end or timeout.
    A compile through a running sccache server runs in THAT server's tree, outside this job: measure with the cache off (WWE_NO_CCACHE=1)."""
    if timeout is None or timeout <= 0:
        raise ValueError('proc.run needs a positive timeout (seconds)')
    peaks = [None, None]

    def read_peaks(j):
        """high-water marks: keep the larger of what was read before and now; a failed read never erases a good one"""
        if os.name != 'nt' or j is None:
            return
        new = _job_peaks(j)
        peaks[:] = [o if n is None else (n if o is None else max(o, n)) for o, n in zip(peaks, new)]

    def end_job(j):
        read_peaks(j)
        _end_job_once(j, keep)
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
        if on_start is not None:                                 # after the pumps: a full pipe never blocks the child while the callback runs
            jd = _dup(job) if (os.name == 'nt' and job is not None) else None
            try:
                on_start(jd, p)
            except Exception:
                close_handle(jd)
        timed_out = False
        try:
            p.wait(timeout=timeout)
            deadline = time.time() + PIPE_GRACE_S                # a daemon holding a pipe open must not hold us: ~5 s, then stop reading
        except subprocess.TimeoutExpired:
            timed_out = True
            read_peaks(job)                                      # before kill_tree: the peak is a high-water mark, read it while the job is whole
            kill_tree(p)
            if job is not None:
                end_job(job)
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
            end_job(job)                  # always: nothing the child left behind survives the call (named daemons excepted)
    rc = TIMEOUT_RC if timed_out else p.returncode

    def fin(chunks):
        b = b''.join(chunks)
        return b.decode('utf-8', 'replace').replace('\r\n', '\n') if text else b
    out, err = (fin(outb), fin(errb)) if capture else (None, None)
    res = Result(cmd, rc, out, err, timed_out, peaks[0], peaks[1])
    if mem:
        sys.stderr.write('proc.run: %s\n' % res.mem_line())
    if check and rc != 0:
        raise subprocess.CalledProcessError(rc, cmd, out, err)
    return res


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
    # item 7 (CQM): the job's peak commit is returned; a child that commits 256 MB (VirtualAlloc MEM_COMMIT, never touched: no physical RAM)
    memline = 'peak commit: not on this OS'
    if os.name == 'nt':
        mb = 256
        r5 = run([sys.executable, '-c', 'import ctypes;k=ctypes.windll.kernel32;k.VirtualAlloc.restype=ctypes.c_void_p;'
                  f'k.VirtualAlloc.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_ulong,ctypes.c_ulong];a=k.VirtualAlloc(None,{mb}<<20,0x3000,4);'
                  'print(1 if a else 0)'], timeout=60)
        assert r5.returncode == 0 and r5.stdout.strip() == '1', r5
        got = (r5.peak_job_commit or 0) / 2**20
        assert mb <= got < mb + 128, f'peak job commit {got:.0f} MB for a {mb} MB commit'
        assert r5.peak_process_commit is not None and r5.peak_process_commit <= r5.peak_job_commit
        assert ok.peak_job_commit is not None and ok.peak_job_commit < 128 << 20, ok.peak_job_commit
        memline = f'peak job commit {got:.0f} MB for a {mb} MB commit (a plain print: {ok.peak_job_commit / 2**20:.0f} MB)'
        commit_child = ('import ctypes,time;k=ctypes.windll.kernel32;k.VirtualAlloc.restype=ctypes.c_void_p;'
                        'k.VirtualAlloc.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_ulong,ctypes.c_ulong];'
                        f'a=k.VirtualAlloc(None,{mb}<<20,0x3000,4);print(1 if a else 0,flush=True);time.sleep(300)')
        # item 8a (CQM review 1): the TIMEOUT path keeps the peak, and a failing second read never erases the first one
        orig_peaks = _job_peaks
        calls = []

        def flaky(j):
            calls.append(1)
            return orig_peaks(j) if len(calls) == 1 else (None, None)
        globals()['_job_peaks'] = flaky
        try:
            r6 = run([sys.executable, '-c', commit_child], timeout=5)
        finally:
            globals()['_job_peaks'] = orig_peaks
        got6 = (r6.peak_job_commit or 0) / 2**20
        assert r6.timed_out and len(calls) >= 2 and mb <= got6 < mb + 128, (r6, calls, got6)
        # item 8b (review 2, 6): on_start that RAISES: the child still runs, is killed at the timeout and waited; the handle run() made is closed
        held = {}

        def bad(job, popen):
            held['pid'] = popen.pid
            raise RuntimeError('planted on_start failure')
        t7 = time.time()
        r7 = run([sys.executable, '-c', 'import time; time.sleep(300)'], timeout=4, on_start=bad)
        assert r7.timed_out and r7.returncode == TIMEOUT_RC and time.time() - t7 < 30, r7
        time.sleep(0.5)
        assert not psutil.pid_exists(held['pid']), 'on_start raised: the child survived the timeout'
        # item 8c (review 2): the duplicated job handle outlives run(): the peaks read through it after run() returned equal the Result's,
        # the job's processes are dead even though the handle is still open, then the owner closes it
        box = {}
        r8 = run([sys.executable, '-c', commit_child], timeout=5, on_start=lambda job, popen: box.update(job=job, pid=popen.pid))
        assert box.get('job'), 'on_start got no job handle'
        after = _job_peaks(box['job'])
        time.sleep(0.5)
        alive = psutil.pid_exists(box['pid'])
        close_handle(box['job'])
        assert after[0] == r8.peak_job_commit and after[0] >= mb << 20, (after, r8.peak_job_commit)
        assert not alive, 'the child outlived run() while the duplicated handle was open'
        # item 8d: a PowerShell command's peak is marked not exact (its children run outside the job)
        assert Result(['pwsh', '-c', 'x'], 0, '', '', False, 1, 1).peak_exact is False
        assert 'not exact' in Result('"C:\\x\\powershell.exe" -c x', 0, '', '', False, 1, 1).mem_line()
        assert Result([sys.executable], 0, '', '', False, 1, 1).peak_exact is True
        memline += (f'; timeout path kept {got6:.0f} MB through a failed second read; a raising on_start still timed out and killed the child; '
                    f'the duplicated handle read {after[0] / 2**20:.0f} MB after run() returned; PowerShell peaks marked not exact')
    print(f'proc selftest OK: parent + grandchild (pid {gpid}) dead {dt:.1f} s after start (timeout 6 s); normal runs pass; works without psutil; '
          f'cleanup failure falls back to TerminateJobObject; a pipe-holding daemon released run() after {dt4:.1f} s; {memline}')
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
