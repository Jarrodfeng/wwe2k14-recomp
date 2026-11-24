"""measure_peak.py: run ONE command in a Windows Job Object and print how much memory it really took (CQM, 2026-11-20).

    py -I tools\\memory\\measure_peak.py [--label NAME] [--cwd DIR] [--timeout S] [--no-ccache] [--log FILE.jsonl] -- CMD ARG ...
    py -I tools\\memory\\measure_peak.py --selftest

Printed (one block, and one JSON row appended to --log when given):
  * peak job commit      PeakJobMemoryUsed: the highest COMMIT charge of the whole job at any instant (exact, kept by the OS). This is the number
                         that matters on this PC: an LLVM "out of memory" is a commit failure (commit limit = RAM + page file).
  * largest process      PeakProcessMemoryUsed: the highest commit of any one process in the job (exact, OS).
  * peak physical        two honest bounds: `sampled` = the largest SUM of the job's working sets seen by a 0.5 s sampler (a lower bound: a peak
                         between samples is missed); `per-process max` = the largest single process's own peak working set (exact per process,
                         psutil peak_wset, read at each sample).
  * wall seconds, return code, process count seen.
  * before the start: the machine's available physical RAM (ullAvailPhys = free + standby) and remaining commit (ullAvailPageFile), plus the
    commit limit, and the compilers / builds already running (clang, clang++, lld, ninja, cmake, sccache, python counts) - other agents
    compile on this PC too; the reading is still one job's own figure (job accounting is per job), but the machine numbers say how tight it was.

Rules (CLAUDE.md Safety): one measurement at a time; no load of its own (the sampler wakes twice a second and reads a pid list).
A compile through a RUNNING sccache server runs in the server's process tree, outside this job: pass --no-ccache (WWE_NO_CCACHE=1) to measure it.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'compile'))
import proc  # noqa: E402

GB = 2 ** 30
WATCH = ('clang', 'clang++', 'clang-cl', 'lld-link', 'ld.lld', 'ninja', 'cmake', 'sccache', 'python', 'py', 'glslangvalidator', 'link', 'cl')


def machine():
    """GlobalMemoryStatusEx: available physical, remaining commit, commit limit, total physical (GB)"""
    import ctypes
    from ctypes import wintypes

    class MS(ctypes.Structure):
        _fields_ = [('dwLength', wintypes.DWORD), ('dwMemoryLoad', wintypes.DWORD), ('ullTotalPhys', ctypes.c_uint64),
                    ('ullAvailPhys', ctypes.c_uint64), ('ullTotalPageFile', ctypes.c_uint64), ('ullAvailPageFile', ctypes.c_uint64),
                    ('ullTotalVirtual', ctypes.c_uint64), ('ullAvailVirtual', ctypes.c_uint64), ('ullAvailExtendedVirtual', ctypes.c_uint64)]
    m = MS()
    m.dwLength = ctypes.sizeof(MS)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)):
        return {}
    return {'avail_phys_gb': round(m.ullAvailPhys / GB, 2), 'avail_commit_gb': round(m.ullAvailPageFile / GB, 2),
            'commit_limit_gb': round(m.ullTotalPageFile / GB, 2), 'total_phys_gb': round(m.ullTotalPhys / GB, 2)}


def others():
    """counts of compile-ish processes running right now (by image name), excluding this process"""
    try:
        import psutil
    except ImportError:
        return {}
    me = os.getpid()
    n = {}
    for p in psutil.process_iter(['name', 'pid']):
        try:
            nm = (p.info['name'] or '').lower().removesuffix('.exe')
        except Exception:
            continue
        if p.info['pid'] != me and nm in WATCH:
            n[nm] = n.get(nm, 0) + 1
    return dict(sorted(n.items()))


import ctypes  # noqa: E402
from ctypes import wintypes  # noqa: E402


class _PMC(ctypes.Structure):
    _fields_ = [('cb', wintypes.DWORD), ('PageFaultCount', wintypes.DWORD)] + [(n, ctypes.c_size_t) for n in (
        'PeakWorkingSetSize', 'WorkingSetSize', 'QuotaPeakPagedPoolUsage', 'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage',
        'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage')]


_k32 = ctypes.WinDLL('kernel32', use_last_error=True)
_k32.OpenProcess.restype = wintypes.HANDLE
_k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
_k32.K32GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD]
_k32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
_k32.CloseHandle.argtypes = [wintypes.HANDLE]
_QUERY = 0x1000 | 0x00100000                                     # QUERY_LIMITED_INFORMATION | SYNCHRONIZE


def _pmc(h):
    m = _PMC()
    m.cb = ctypes.sizeof(_PMC)
    return m if _k32.K32GetProcessMemoryInfo(h, ctypes.byref(m), m.cb) else None


def _alive(h):
    c = wintypes.DWORD(0)
    return bool(_k32.GetExitCodeProcess(h, ctypes.byref(c))) and c.value == 259   # STILL_ACTIVE


class Sampler:
    """Every `interval` s: the processes of the JOB and of the child's PROCESS TREE (parent links; the Store pwsh 7.7 starts its native children
    OUTSIDE the job it runs in: CQM 2026-11-20, so job accounting alone misses every compile a pwsh build script starts). A handle is kept to each
    process seen until the end: that keeps its pid from being reused and lets finish() read its OWN exact peaks (PeakPagefileUsage = peak commit,
    PeakWorkingSetSize) after it has exited. The sums over processes alive at a sample (commit now, working set now) are lower bounds of the
    tree's simultaneous peak; the sum of the per-process peaks is an upper bound. A process that lives less than one interval can be missed.
    Shared daemons (proc.DAEMONS: sccache, adb) are never adopted nor descended through: a cache server the command starts lives on and runs
    other agents' compiles (CQM review 3). Whether a process is in the job is asked of the OS (IsProcessInJob) when its handle is taken."""

    def __init__(self, interval=0.5):
        self.interval, self.job, self.root, self.stop = interval, None, None, threading.Event()
        self.held = {}                                            # pid -> [handle, name, in_job, create time]
        self.skipped = set()                                      # daemon pids seen and left out
        self.max_commit_sum = self.max_wset_sum = 0
        self.samples = 0
        self.t = None

    def start(self, job, popen):
        """proc.run's on_start: `job` is a duplicated handle this sampler owns (closed in finish())"""
        self.job, self.root = job, popen.pid
        self.t = threading.Thread(target=self.loop, daemon=True)
        self.t.start()

    def loop(self):
        import psutil
        while not self.stop.is_set():
            try:
                self.sample(psutil)
            except Exception:
                pass
            self.stop.wait(self.interval)

    def _in_job(self, h):
        if self.job is None:
            return False
        b = wintypes.BOOL(0)
        return bool(proc._k32.IsProcessInJob(h, self.job, ctypes.byref(b))) and bool(b.value)

    def _hold(self, psutil, pid, ctime=None):
        if pid in self.held or pid in self.skipped:
            return
        try:
            pr = psutil.Process(pid)
            name = pr.name()
            ctime = pr.create_time() if ctime is None else ctime
        except Exception:
            name = '?'
        if name.lower().removesuffix('.exe') in proc.DAEMONS:
            self.skipped.add(pid)
            return
        h = _k32.OpenProcess(_QUERY, False, pid)
        if not h:
            return
        self.held[pid] = [h, name, self._in_job(h), ctime]

    def sample(self, psutil):
        jp = set(proc._job_pids(self.job)) if self.job is not None else set()
        for pid in jp:
            self._hold(psutil, pid)
        if self.root is not None:
            self._hold(psutil, self.root)
        # the tree: any process whose parent is a process we hold AND that was created after that parent, compared with the parent's create time
        # stored when we first held it. A detached process (the sccache server) keeps the pid of a parent that died long ago as its ppid; when that
        # pid is reused by a process of OUR tree, only the create times tell them apart (CQM U4 r2: the server and 141 foreign clang++ were adopted).
        # A child whose create time cannot be read is not adopted.
        try:
            pmap = psutil._psplatform.ppid_map()
        except Exception:
            pmap = {}
        grew = True
        while grew:
            grew = False
            for pid, ppid in pmap.items():
                if pid in self.held or ppid not in self.held:
                    continue
                pct = self.held[ppid][3]
                try:
                    cct = psutil.Process(pid).create_time()
                except Exception:
                    continue
                if pct is None or cct + 0.01 < pct:
                    continue
                self._hold(psutil, pid, cct)
                grew = pid in self.held or grew
        c = w = 0
        for h, _, _, _ in self.held.values():
            if _alive(h):
                m = _pmc(h)
                if m:
                    c += m.PagefileUsage
                    w += m.WorkingSetSize
        self.samples += 1
        self.max_commit_sum = max(self.max_commit_sum, c)
        self.max_wset_sum = max(self.max_wset_sum, w)

    def finish(self):
        """stop sampling; read every held process's own peaks; close the handles. Returns the per-process table."""
        self.stop.set()
        if self.t is not None:
            self.t.join(5)
        proc.close_handle(self.job)                               # the duplicated job handle proc.run handed over: ours to close
        self.job = None
        rows = []
        for pid, (h, name, in_job, _) in self.held.items():
            m = _pmc(h)
            if m:
                rows.append({'pid': pid, 'name': name, 'in_job': in_job, 'peak_commit': m.PeakPagefileUsage, 'peak_wset': m.PeakWorkingSetSize})
            _k32.CloseHandle(h)
        self.held = {}
        return rows


def measure(cmd, label='', cwd=None, timeout=3600, no_ccache=False, env=None, interval=0.5, quiet=False, show_output=False):
    e = dict(os.environ if env is None else env)
    if no_ccache:
        e['WWE_NO_CCACHE'] = '1'
    before, running = machine(), others()
    s = Sampler(interval)
    t0 = time.time()
    r = proc.run(cmd, timeout=timeout, cwd=cwd, env=e, capture=True, on_start=s.start)
    wall = time.time() - t0
    procs = s.finish()
    g = lambda b: None if b is None else round(b / GB, 3)
    outside = [p for p in procs if not p['in_job']]
    big_c = max(procs, key=lambda p: p['peak_commit'], default=None)
    big_w = max(procs, key=lambda p: p['peak_wset'], default=None)
    sum_peaks = sum(p['peak_commit'] for p in procs)
    by_name = {}
    for p in procs:
        by_name[p['name']] = by_name.get(p['name'], 0) + 1
    # the commit figure to use: exact (the job's own) when nothing ran outside the job; else the larger of the job's and the sampled tree sum
    # (a lower bound) with the sum of per-process peaks as the upper bound
    exact = not outside and r.peak_job_commit is not None
    commit_lo = r.peak_job_commit if exact else max(r.peak_job_commit or 0, s.max_commit_sum)
    commit_hi = r.peak_job_commit if exact else max(sum_peaks, commit_lo)
    row = {'label': label, 'when': time.strftime('%Y-%m-%d %H:%M:%S'), 'cmd': cmd if isinstance(cmd, str) else subprocess.list2cmdline(cmd),
           'cwd': cwd or os.getcwd(), 'rc': r.returncode, 'timed_out': r.timed_out, 'wall_s': round(wall, 1),
           'peak_job_commit_gb': g(r.peak_job_commit), 'peak_job_process_commit_gb': g(r.peak_process_commit),
           'commit_exact': exact, 'peak_commit_lo_gb': g(commit_lo), 'peak_commit_hi_gb': g(commit_hi),
           'largest_proc_commit_gb': g(big_c['peak_commit']) if big_c else None, 'largest_proc_commit_name': big_c['name'] if big_c else '',
           'largest_proc_wset_gb': g(big_w['peak_wset']) if big_w else None,
           'peak_phys_sampled_gb': g(s.max_wset_sum), 'peak_commit_sampled_gb': g(s.max_commit_sum),
           'procs_seen': len(procs), 'procs_outside_job': len(outside), 'procs_by_name': by_name, 'samples': s.samples,
           'no_ccache': bool(no_ccache), 'before': before, 'running_before': running}
    if not quiet:
        print(f'[measure_peak] {label or "-"}: rc {r.returncode}{" TIMED OUT" if r.timed_out else ""}, wall {wall:.1f} s; '
              f'{len(procs)} processes seen ({len(outside)} outside the job) {by_name}')
        if exact:
            print(f'  peak commit {row["peak_commit_lo_gb"]} GB (exact: the job\'s PeakJobMemoryUsed)')
        else:
            print(f'  peak commit {row["peak_commit_lo_gb"]} .. {row["peak_commit_hi_gb"]} GB (processes ran OUTSIDE the job: lower bound = '
                  f'max(job {row["peak_job_commit_gb"]}, sampled tree sum {row["peak_commit_sampled_gb"]}), upper bound = sum of per-process peaks)')
        print(f'  largest process commit {row["largest_proc_commit_gb"]} GB ({row["largest_proc_commit_name"]}, exact, its own PeakPagefileUsage); '
              f'job\'s PeakProcessMemoryUsed {row["peak_job_process_commit_gb"]} GB')
        print(f'  peak physical: sampled tree working-set sum {row["peak_phys_sampled_gb"]} GB (lower bound, {s.samples} samples at {interval} s); '
              f'largest process peak working set {row["largest_proc_wset_gb"]} GB (exact)')
        print(f'  before: available physical {before.get("avail_phys_gb")} GB, remaining commit {before.get("avail_commit_gb")} GB '
              f'(limit {before.get("commit_limit_gb")}); running: {running}')
        if r.returncode != 0 or show_output:
            tail = ((r.stderr or '') + (r.stdout or ''))[-1500:]
            print('  output tail:\n' + tail)
    return row, r


def selftest():
    """a child that COMMITS 512 MB (and touches 256 MB of it), holds 3 s; then the same split over a child + grandchild (256 + 256 MB)"""
    d = tempfile.mkdtemp(prefix='measure_peak_selftest_')
    alloc = os.path.join(d, 'alloc.py')
    with open(alloc, 'w') as f:
        f.write('import ctypes, sys, time, subprocess\n'
                'k = ctypes.windll.kernel32\n'
                'k.VirtualAlloc.restype = ctypes.c_void_p\n'
                'k.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulong, ctypes.c_ulong]\n'
                'mb, touch, kid = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3] == "1"\n'
                'g = subprocess.Popen([sys.executable, __file__, str(mb), str(touch), "0"]) if kid else None\n'
                'a = k.VirtualAlloc(None, mb << 20, 0x3000, 4)\n'
                'assert a\n'
                'ctypes.memset(a, 1, touch << 20)\n'
                'time.sleep(3)\n'
                'if g: g.wait()\n')
    ok = True
    row, _ = measure([sys.executable, alloc, '512', '256', '0'], label='selftest one process 512 MB', timeout=60)
    c = row['peak_job_commit_gb'] * 1024
    p = row['largest_proc_wset_gb'] * 1024
    lc = row['largest_proc_commit_gb'] * 1024
    c_ok = 512 * 0.9 <= c <= 512 * 1.1 and row['commit_exact'] and 512 * 0.9 <= lc <= 512 * 1.1
    p_ok = 256 <= p <= 256 + 64
    print(f'  check: job commit {c:.0f} MB (exact) and the process\'s own peak commit {lc:.0f} MB vs 512 (+-10 %): {"ok" if c_ok else "FAIL"}; '
          f'peak working set {p:.0f} MB vs 256 touched: {"ok" if p_ok else "FAIL"}')
    ok &= c_ok and p_ok
    row2, _ = measure([sys.executable, alloc, '256', '128', '1'], label='selftest child + grandchild 256 + 256 MB', timeout=60)
    c2 = row2['peak_job_commit_gb'] * 1024
    pp2 = row2['peak_job_process_commit_gb'] * 1024
    s2 = row2['peak_phys_sampled_gb'] * 1024
    c2_ok = 512 * 0.9 <= c2 <= 512 * 1.1 and 256 * 0.9 <= pp2 <= 256 * 1.15
    s2_ok = 256 * 0.9 <= s2 <= 256 + 96
    print(f'  check: job commit {c2:.0f} MB vs 512 and largest process {pp2:.0f} MB vs 256 (+-10 %): {"ok" if c2_ok else "FAIL"}; '
          f'sampled working set {s2:.0f} MB vs 2 x 128 touched: {"ok" if s2_ok else "FAIL"}')
    ok &= c2_ok and s2_ok
    # the tree mode: the same 512 MB child started by pwsh (the Store pwsh 7.7 starts it OUTSIDE the job). The child's own exact peak commit is read
    # after it has exited (the handle kept since the sample that saw it), and the bounds must hold 512 MB.
    import shutil
    pw = shutil.which('pwsh')
    if pw:
        row3, _ = measure([pw, '-NoProfile', '-Command', f"& '{sys.executable}' '{alloc}' 512 256 0"], label='selftest pwsh -> child 512 MB', timeout=90)
        lc3 = (row3['largest_proc_commit_gb'] or 0) * 1024
        lo3, hi3 = row3['peak_commit_lo_gb'] * 1024, row3['peak_commit_hi_gb'] * 1024
        c3_ok = 512 * 0.9 <= lc3 <= 512 * 1.1 and lo3 <= 512 * 1.1 + 150 and hi3 >= 512 * 0.9 and lo3 >= 512 * 0.9
        print(f'  check: pwsh child seen ({row3["procs_outside_job"]} outside the job); its own peak commit {lc3:.0f} MB vs 512 (+-10 %); '
              f'bounds {lo3:.0f} .. {hi3:.0f} MB hold 512: {"ok" if c3_ok else "FAIL"}')
        ok &= c3_ok
        # a daemon is never adopted (review 3): the same run with python counted as a daemon must not hold the python child
        saved = proc.DAEMONS
        proc.DAEMONS = set(saved) | {'python'}
        try:
            row4, _ = measure([pw, '-NoProfile', '-Command', f"& '{sys.executable}' '{alloc}' 64 0 0"], label='selftest daemon not adopted',
                              timeout=90, quiet=True)
        finally:
            proc.DAEMONS = saved
        d_ok = 'python.exe' not in row4['procs_by_name'] and row4['procs_seen'] >= 1
        print(f'  check: a daemon-named child is not adopted ({row4["procs_by_name"]}): {"ok" if d_ok else "FAIL"}')
        ok &= d_ok
    else:
        print('  (no pwsh on PATH: the tree-mode cases are skipped)')
    print('measure_peak selftest ' + ('OK' if ok else 'FAILED'))
    return 0 if ok else 1


def compdb_line(db, unit, outdir, match=''):
    """the EXACT compile line a build tree uses for `unit` (a path suffix), with its -o / -MF outputs moved into `outdir` (absolute) and nothing else
    changed; returned as the raw command string (CreateProcess parses it as clang would get it from ninja). The build tree is only read.
    match: a text the entry's output path must hold (a multi-config tree lists one entry per config, e.g. '/Release/')."""
    import re
    rows = [e for e in json.load(open(db, encoding='utf-8')) if e['file'].replace('\\', '/').endswith(unit.replace('\\', '/'))
            and match in e.get('output', '').replace('\\', '/')]
    if len(rows) != 1:
        raise SystemExit(f'{unit}: {len(rows)} entries in {db} (need exactly 1)')
    e = rows[0]
    cmd = e.get('command') or subprocess.list2cmdline(e['arguments'])
    base = re.sub(r'[^A-Za-z0-9_.-]', '_', os.path.basename(unit))
    os.makedirs(outdir, exist_ok=True)
    obj, dep = os.path.join(outdir, base + '.obj'), os.path.join(outdir, base + '.obj.d')
    n_o = n_mf = 0
    cmd, n_o = re.subn(r'(?<=\s)-o\s+\S+', lambda m: '-o ' + obj, cmd)
    cmd, n_mf = re.subn(r'(?<=\s)-MF\s+\S+', lambda m: '-MF ' + dep, cmd)
    if n_o != 1 or ('-MD' in cmd.split() and n_mf != 1):
        raise SystemExit(f'{unit}: could not redirect the outputs (-o {n_o}, -MF {n_mf})')
    return cmd, e['directory']


def main():
    if '--selftest' in sys.argv[1:2]:
        return selftest()
    argv = sys.argv[1:]
    if '--compdb' in argv and '--' not in argv:
        argv = argv + ['--']
    if '--' not in argv:
        print(__doc__)
        return 2
    i = argv.index('--')
    ap = argparse.ArgumentParser()
    ap.add_argument('--compdb', help='compile_commands.json (e.g. `ninja -t compdb` of a build tree, saved elsewhere); with --unit and --outdir')
    ap.add_argument('--unit', help='source path suffix whose line is run, outputs moved to --outdir')
    ap.add_argument('--outdir')
    ap.add_argument('--match', default='', help="with --compdb: a text the entry's output path must hold (multi-config trees: /Release/)")
    ap.add_argument('--print-cmd', action='store_true')
    ap.add_argument('--show-output', action='store_true', help="print the tail of the command's output also when it succeeds")
    ap.add_argument('--label', default='')
    ap.add_argument('--cwd')
    ap.add_argument('--timeout', type=float, default=3600)
    ap.add_argument('--no-ccache', action='store_true')
    ap.add_argument('--interval', type=float, default=0.5)
    ap.add_argument('--log', help='append the JSON row to this .jsonl file')
    a = ap.parse_args(argv[:i])
    cmd = argv[i + 1:]
    if a.compdb:
        if cmd or not (a.unit and a.outdir):
            print('--compdb needs --unit and --outdir and no command after --')
            return 2
        cmd, _ = compdb_line(a.compdb, a.unit, os.path.abspath(a.outdir), a.match)
        a.cwd = a.cwd or os.path.abspath(a.outdir)              # relative paths of the line land in the scratch folder, never in the build tree
        if a.print_cmd:
            print(cmd)
    if not cmd:
        print('no command after --')
        return 2
    if a.interval < 0.2:
        print('--interval below 0.2 s refused (no tight sampling loop)')
        return 2
    row, r = measure(cmd, label=a.label, cwd=a.cwd, timeout=a.timeout, no_ccache=a.no_ccache, interval=a.interval, show_output=a.show_output)
    if a.log:
        os.makedirs(os.path.dirname(os.path.abspath(a.log)), exist_ok=True)
        with open(a.log, 'a', encoding='utf-8') as f:
            f.write(json.dumps(row) + '\n')
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
