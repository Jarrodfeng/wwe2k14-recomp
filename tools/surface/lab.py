# S102 lane M2: the Surface lab. A queue of rplay replay jobs run one at a time on a host (the Surface over ssh, or a local folder standing in
# for it), returning GPU and render-thread milliseconds for a capture, with no game run. Page R10 gap (c); docs\renderer\SURFACE_LAB.md.
#
#   py -I tools\surface\lab.py push-exe [--exe out\renderer\surface\rplay.exe]      rplay.exe + rplay.sha256 -> <lab>\ on the host (our code, no game data)
#   py -I tools\surface\lab.py push CAPTURE_DIR                                      one capture folder -> <lab>\captures\NAME (game data: stays on Jarrod's machines)
#   py -I tools\surface\lab.py submit CAPTURE [--switch NAME=VAL ...] [--frames N] [--repeat K] [--warm-frames W] [--timeout S] [--label TEXT]
#   py -I tools\surface\lab.py run [--max N]                                         runs queued jobs one at a time, appends rows to results.csv
#   py -I tools\surface\lab.py show [-n 10]                                          newest rows
#   py -I tools\surface\lab.py status                                                queue, host, lock
# Host: --host surface (ssh alias, lab folder C:/wwe/lab; the main session runs it) or --host fake:DIR (a local folder; rplay is tools\surface\lab_fake_rplay.py).
# --switch NAME=VAL becomes the rplay argument "--NAME VAL"; "NAME=flag" (or "NAME=") is the bare flag "--NAME" (e.g. crowd-ours=flag). Names are checked
# against the flags src\renderer\player\rplay.cpp parses (a typo is refused at submit); capture / frames / warm-frames / timing belong to the lab.
# State (queue, results, raw csvs) lives under <MAIN repo>\out\s102\lab (--lab-dir). Failed jobs are recorded status=failed with the cause, never as 0 ms.
# One GPU job on the Surface at a time: a PC lock (O_EXCL file), a Surface lock folder (<lab>\run.lock), a tasklist check for rplay.exe / wwe2k14.exe before every
# repeat, and a taskkill of rplay.exe on every path that does not end ok.
import argparse, csv, hashlib, json, math, os, re, shutil, subprocess, sys, tempfile, time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
RPLAY_SRC = REPO / 'src' / 'renderer' / 'player' / 'rplay.cpp'
NAME_RE = re.compile(r'^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$')
VAL_RE = re.compile(r'^[A-Za-z0-9_.,:+-]{1,64}$')
RESERVED = ('capture', 'frames', 'warm-frames', 'timing')
FAKE_FLAGS = {'fake-exit': True, 'fake-sleep': True, 'fake-hang': False, 'fake-nogpu': False, 'fake-nocsv': False, 'fake-passes': False, 'fake-short': False,
              'fake-zero-some': False, 'fake-nogpu-after': True}
SSH_OPTS = ['-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', '-o', 'ServerAliveInterval=15', '-o', 'ServerAliveCountMax=4']
FIELDS = ['job_id', 'finished', 'host', 'exe_sha', 'capture', 'switches', 'label', 'frames', 'captured_frames', 'repeat', 'warm_frames', 'status', 'error', 'exit_code',
          'timed_frames', 'first_frame_ms', 'render_ms_med', 'render_ms_p99', 'bind_ms_med', 'gpu_ms_med', 'gpu_ms_p99', 'gpu_wait_ms_med',
          'drift_pct', 'run_gpu_ms_med', 'passes_ms', 'wall_s']
BUSY_NAMES = ('rplay.exe', 'wwe2k14.exe')


class HostError(Exception):
    """The host could not be reached or a transfer/command on it failed (ssh rc 255, a timeout): not a statement about the job's capture or rplay."""


def fail(msg, code=2):
    print('lab: ' + msg, file=sys.stderr)
    sys.exit(code)


# ---------------------------------------------------------------- hosts
def _ssh_cmd(alias, cmd):
    return ['ssh'] + SSH_OPTS + [alias, cmd]


def _scp_cmd(args):
    return ['scp', '-q'] + SSH_OPTS + args


class Host:
    name = ''
    def mkdirs(self, rels): raise NotImplementedError
    def put_file(self, local, rel): raise NotImplementedError
    def put_dir(self, local, rel): raise NotImplementedError          # replaces rel as a whole (via rel.partial)
    def get_file(self, rel, local): raise NotImplementedError          # False when the file is not there
    def exists(self, rel): raise NotImplementedError
    def rm_glob(self, rel_glob): raise NotImplementedError
    def sha256(self, rel): raise NotImplementedError                   # None when the file is not there
    def busy(self): raise NotImplementedError                          # names of GPU programs already running
    def lock(self): raise NotImplementedError                          # False when held
    def unlock(self): raise NotImplementedError
    def kill(self): raise NotImplementedError
    def run(self, args, log_rel, timeout_s): raise NotImplementedError   # returns rplay's exit code or 'timeout'


class FakeHost(Host):
    """A local folder standing in for the Surface: the same commands, run here. rplay is tools\\surface\\lab_fake_rplay.py (rplay.exe in the folder is only hashed).
    Test hooks: <root>\\busy.flag = a GPU program is running; <root>\\ssh-down.flag = every call fails like ssh rc 255."""
    def __init__(self, root):
        self.root = Path(root).resolve()
        self.name = 'fake:' + str(root)
        self.root.mkdir(parents=True, exist_ok=True)

    def _p(self, rel):
        if (self.root / 'ssh-down.flag').exists(): raise HostError('ssh failed (rc 255): fake host is down')
        return self.root / rel

    def mkdirs(self, rels):
        for r in rels: self._p(r).mkdir(parents=True, exist_ok=True)

    def put_file(self, local, rel):
        self._p(rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(local, self._p(rel))

    def put_dir(self, local, rel):
        final, part = self._p(rel), self._p(rel + '.partial')
        if part.exists(): shutil.rmtree(part)
        shutil.copytree(local, part)
        if final.exists(): shutil.rmtree(final)
        if final.exists(): raise HostError(f'could not remove the old {rel}')
        part.rename(final)

    def get_file(self, rel, local):
        if not self._p(rel).is_file(): return False
        Path(local).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(self._p(rel), local)
        return True

    def exists(self, rel): return self._p(rel).exists()

    def rm_glob(self, rel_glob):
        p = self._p(rel_glob)
        for f in p.parent.glob(p.name):
            if f.is_file(): f.unlink()

    def sha256(self, rel):
        p = self._p(rel)
        return hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else None

    def busy(self): return ['busy.flag'] if self._p('busy.flag').exists() else []

    def lock(self):
        try: self._p('run.lock').mkdir(); return True
        except FileExistsError: return False

    def unlock(self):
        try: self._p('run.lock').rmdir()
        except OSError: pass

    def kill(self):
        with open(self._p('killed.log'), 'a') as f: f.write('taskkill rplay.exe\n')

    def run(self, args, log_rel, timeout_s):
        cmd = [sys.executable, '-I', str(Path(__file__).with_name('lab_fake_rplay.py'))]
        self._p(log_rel).parent.mkdir(parents=True, exist_ok=True)
        with open(self._p(log_rel), 'w', encoding='utf-8', errors='replace') as log:
            try:
                return subprocess.run(cmd + args, cwd=self.root, stdout=log, stderr=subprocess.STDOUT, timeout=timeout_s).returncode
            except subprocess.TimeoutExpired:
                return 'timeout'


class SshHost(Host):
    """The Surface Pro 6 over ssh (host alias "surface" in ~\\.ssh\\config, as the other tools\\surface\\ scripts). Remote shell is cmd.exe.
    rc 255 from ssh / scp is HostError (ssh failed), never read as an answer about the job."""
    def __init__(self, alias='surface', root='C:/wwe/lab'):
        self.alias, self.root = alias, root.rstrip('/')
        self.name = alias

    def _win(self, rel): return (self.root + '/' + rel).replace('/', '\\').rstrip('\\')

    def _sh(self, cmd, timeout=120):
        try:
            r = subprocess.run(_ssh_cmd(self.alias, cmd), capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            raise HostError(f'ssh failed: timed out after {timeout} s ({cmd[:60]})')
        if r.returncode == 255: raise HostError('ssh failed (rc 255): ' + (r.stderr.strip() or 'no message')[:200])
        return r

    def _scp(self, args, timeout=900):
        try:
            r = subprocess.run(_scp_cmd(args), capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            raise HostError(f'scp failed: timed out after {timeout} s')
        if r.returncode: raise HostError(f'scp failed (rc {r.returncode}): ' + r.stderr.strip()[:200])

    def mkdirs(self, rels):
        for r in rels: self._sh(f'if not exist "{self._win(r)}" mkdir "{self._win(r)}"')

    def put_file(self, local, rel):
        parent = str(Path(rel).parent).replace('\\', '/')
        self.mkdirs(['' if parent == '.' else parent])
        self._scp([str(local), f'{self.alias}:{self.root}/{rel}'])

    def put_dir(self, local, rel):
        parent = str(Path(rel).parent).replace('\\', '/')
        self.mkdirs(['' if parent == '.' else parent])
        part, final = self._win(rel + '.partial'), self._win(rel)
        self._sh(f'if exist "{part}" rmdir /s /q "{part}"')
        if self.exists(rel + '.partial'): raise HostError(f'could not remove the old {rel}.partial')
        self._scp(['-r', str(local), f'{self.alias}:{self.root}/{rel}.partial'])
        self._sh(f'if exist "{final}" rmdir /s /q "{final}"')
        if self.exists(rel): raise HostError(f'rmdir did not remove the old {rel}; not copying over it')
        self._sh(f'move "{part}" "{final}"')
        if not self.exists(rel) or self.exists(rel + '.partial'): raise HostError(f'move of {rel}.partial into place failed')

    def get_file(self, rel, local):
        if not self.exists(rel): return False
        Path(local).parent.mkdir(parents=True, exist_ok=True)
        self._scp([f'{self.alias}:{self.root}/{rel}', str(local)])
        return True

    def exists(self, rel):
        r = self._sh(f'if exist "{self._win(rel)}" (exit 0) else (exit 9)')
        if r.returncode == 0: return True
        if r.returncode == 9: return False
        raise HostError(f'ssh failed: exists check returned {r.returncode}')

    def rm_glob(self, rel_glob):
        self._sh(f'del /q "{self._win(rel_glob)}" 2>nul & exit 0')

    def sha256(self, rel):
        if not self.exists(rel): return None
        r = self._sh(f'certutil -hashfile "{self._win(rel)}" SHA256')
        lines = [l.strip() for l in r.stdout.splitlines() if l.strip()]
        for l in lines[1:2]:
            h = l.replace(' ', '').lower()
            if re.fullmatch(r'[0-9a-f]{64}', h): return h
        raise HostError('could not read the exe hash: ' + r.stdout.strip()[:120])

    def busy(self):
        found = []
        for n in BUSY_NAMES:
            r = self._sh(f'tasklist /FI "IMAGENAME eq {n}" /NH')
            if n in r.stdout.lower(): found.append(n)
        return found

    def lock(self):
        self.mkdirs([''])
        return self._sh(f'mkdir "{self._win("run.lock")}"').returncode == 0

    def unlock(self):
        self._sh(f'rmdir "{self._win("run.lock")}"')

    def kill(self):
        self._sh('taskkill /f /im rplay.exe', timeout=30)

    def run(self, args, log_rel, timeout_s):
        cmd = f'cd /d "{self._win("")}" && rplay.exe {" ".join(args)} > "{self._win(log_rel)}" 2>&1'
        try:
            r = subprocess.run(_ssh_cmd(self.alias, cmd), capture_output=True, text=True, timeout=timeout_s)
        except subprocess.TimeoutExpired:
            return 'timeout'
        if r.returncode == 255: raise HostError('ssh failed (rc 255): ' + (r.stderr.strip() or 'no message')[:200])
        return r.returncode


def make_host(spec):
    if spec.startswith('fake:'): return FakeHost(spec[5:])
    if spec == 'surface': return SshHost('surface')
    fail(f'--host must be "surface" or "fake:DIR", got {spec!r}')


# ---------------------------------------------------------------- paths, flags
def main_repo():
    try:
        r = subprocess.run(['git', '-C', str(REPO), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20)
        if r.returncode == 0 and r.stdout.strip(): return Path(r.stdout.strip()).parent
    except Exception:
        pass
    return REPO


def lab_paths(lab_dir):
    d = Path(lab_dir)
    return {'dir': d, 'queue': d / 'queue', 'done': d / 'done', 'raw': d / 'raw', 'results': d / 'results.csv', 'lock': d / 'run.lock'}


def rplay_flags(src=RPLAY_SRC):
    """{name: takes_value} from rplay.cpp's argument parser: arg == "--name" [&& i + 1 < argc]. Raises when the source cannot be read."""
    text = Path(src).read_text(errors='replace')
    flags = {}
    for m in re.finditer(r'arg == "--([A-Za-z0-9-]+)"(\s*&&\s*i\s*\+\s*1\s*<\s*argc)?', text):
        flags[m.group(1)] = flags.get(m.group(1), False) or bool(m.group(2))
    if not flags: raise RuntimeError(f'no flags found in {src}')
    return flags


def parse_switches(items, fake_ok):
    sw = {}
    try: known = rplay_flags()
    except Exception as e: fail(f'cannot read rplay\'s flags to check --switch ({e})')
    for it in items or []:
        name, _, val = it.partition('=')
        if not NAME_RE.match(name) or (val and not VAL_RE.match(val)): fail(f'bad --switch {it!r} (NAME=VAL, letters digits . _ - , : + only)')
        if name in RESERVED: fail(f'--switch {name}: the lab sets --{name} itself (use the submit option)')
        if name.startswith('fake-'):
            if not fake_ok: fail(f'--switch {name}: fake-* only with --host fake:DIR')
            takes = FAKE_FLAGS.get(name)
            if takes is None: fail(f'unknown fake switch {name}')
        elif name not in known: fail(f'unknown rplay flag --{name} (not in rplay.cpp\'s parser)')
        else: takes = known[name]
        bare = val in ('', 'flag')
        if takes and bare: fail(f'--{name} takes a value: --switch {name}=VALUE')
        if not takes and not bare: fail(f'--{name} is a bare flag: --switch {name}=flag')
        sw[name] = val
    return sw


def rplay_args(job, rel_timing):
    a = ['--capture', 'captures/' + job['capture'], '--frames', str(job['frames']), '--warm-frames', str(job['warm_frames']), '--timing', rel_timing]
    for n, v in job['switches'].items():
        a.append('--' + n)
        if v and v != 'flag': a.append(v)
    return a


def cmd_submit(a, paths):
    if not NAME_RE.match(a.capture): fail(f'bad capture name {a.capture!r}')
    if a.frames < 2 or a.repeat < 1: fail('--frames must be >= 2 (frame 0 is reported apart) and --repeat >= 1')
    sw = parse_switches(a.switch, a.host.startswith('fake:'))
    paths['queue'].mkdir(parents=True, exist_ok=True)
    n = len(list(paths['queue'].glob('*.json'))) + (len(list(paths['done'].glob('*.json'))) if paths['done'].exists() else 0)
    job_id = f"{time.strftime('%Y%m%d_%H%M%S')}_{n:03d}_{a.capture}"
    job = {'id': job_id, 'capture': a.capture, 'switches': sw, 'frames': a.frames, 'repeat': a.repeat, 'warm_frames': a.warm_frames,
           'timeout_s': a.timeout, 'label': a.label, 'submitted': time.strftime('%Y-%m-%d %H:%M:%S')}
    (paths['queue'] / (job_id + '.json')).write_text(json.dumps(job, indent=1))
    print('queued', job_id)


# ---------------------------------------------------------------- results
def pctl(vals, p):
    s = sorted(vals)
    return s[min(len(s) - 1, max(0, math.ceil(p * len(s) / 100) - 1))]


def med(vals):
    s = sorted(vals); n = len(s)
    return s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2


def read_timing(path):
    """Rows of rplay's --timing csv (frame,draws,load_us,bind_us,render_us,gpu_us,gpu_ns). Raises ValueError on anything unreadable."""
    rows = []
    with open(path, newline='') as f:
        rd = csv.DictReader(f)
        need = ['frame', 'load_us', 'bind_us', 'render_us', 'gpu_us', 'gpu_ns']
        if not rd.fieldnames or any(c not in rd.fieldnames for c in need): raise ValueError(f'timing csv header {rd.fieldnames}')
        for r in rd:
            rows.append({k: float(r[k]) for k in need})
    return rows


def summarise(runs):
    """runs: one list of timing rows per repeat. Frame 0 of each run is its own number; medians are over frames >= 1."""
    if not runs or any(len(r) < 2 for r in runs): raise ValueError('a run has fewer than 2 timed frames')
    for k, r in enumerate(runs, 1):
        zero = sum(1 for x in r[1:] if x['gpu_ns'] <= 0)
        if zero: raise ValueError(f'repeat {k}: {zero} of {len(r) - 1} frames have gpu_ns 0 (no GPU timestamps; a 0 ms result would be a lie)')
    first = med([(r[0]['load_us'] + r[0]['bind_us'] + r[0]['render_us']) / 1000 for r in runs])
    body = [row for r in runs for row in r[1:]]
    render = [(x['load_us'] + x['bind_us'] + x['render_us']) / 1000 for x in body]
    gpu = [x['gpu_ns'] / 1e6 for x in body]
    per_run_gpu = [med([x['gpu_ns'] / 1e6 for x in r[1:]]) for r in runs]
    drift = (per_run_gpu[-1] - per_run_gpu[0]) / per_run_gpu[0] * 100 if len(runs) > 1 and per_run_gpu[0] > 0 else ''
    return {'timed_frames': len(body), 'first_frame_ms': first, 'render_ms_med': med(render), 'render_ms_p99': pctl(render, 99),
            'bind_ms_med': med([x['bind_us'] / 1000 for x in body]), 'gpu_ms_med': med(gpu), 'gpu_ms_p99': pctl(gpu, 99),
            'gpu_wait_ms_med': med([x['gpu_us'] / 1000 for x in body]), 'drift_pct': drift, 'run_gpu_ms_med': '/'.join(f'{g:.3f}' for g in per_run_gpu)}


def read_passes(paths_list):
    """Optional per-pass files (frame,pass,gpu_ns), one per repeat, pooled (rplay has none today: R10 missing item 4)."""
    per = {}
    for path in paths_list:
        with open(path, newline='') as f:
            for r in csv.DictReader(f):
                if int(r['frame']) >= 1: per.setdefault(r['pass'], []).append(float(r['gpu_ns']) / 1e6)
    return ';'.join(f'{k}={med(v):.3f}' for k, v in sorted(per.items()))


def fmt(v): return f'{v:.3f}' if isinstance(v, float) else str(v)


def append_row(paths, row):
    paths['dir'].mkdir(parents=True, exist_ok=True)
    res = paths['results']
    if res.exists():
        with open(res, newline='') as f:
            rd = csv.DictReader(f)
            head, old = rd.fieldnames, list(rd)
        if head != FIELDS:
            if not head or any(h not in FIELDS for h in head): fail(f'{res} has a different header than this lab.py writes; move it aside')
            tmp = res.with_suffix('.csv.tmp')   # an older lab.py's file: same columns minus the new ones; rewritten with them blank
            with open(tmp, 'w', newline='') as f:
                w = csv.DictWriter(f, fieldnames=FIELDS)
                w.writeheader()
                for r in old: w.writerow({k: r.get(k, '') for k in FIELDS})
            os.replace(tmp, res)
    with open(res, 'a', newline='') as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        if f.tell() == 0: w.writeheader()
        w.writerow({k: fmt(row.get(k, '')) for k in FIELDS})


# ---------------------------------------------------------------- run
def _pid_alive(pid):
    if not pid: return False
    try:
        if os.name == 'nt':
            out = subprocess.run(['tasklist', '/FI', f'PID eq {pid}', '/NH'], capture_output=True, text=True, timeout=20).stdout
            return str(pid) in out
        return os.path.exists(f'/proc/{pid}')
    except Exception:
        return False


def acquire_lock(paths):
    paths['dir'].mkdir(parents=True, exist_ok=True)
    for _ in range(3):
        try:
            fd = os.open(paths['lock'], os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, str(os.getpid()).encode()); os.close(fd)
            return
        except FileExistsError:
            try: pid = int(paths['lock'].read_text().strip())
            except (ValueError, OSError): pid = 0
            if _pid_alive(pid): fail(f'another lab run holds the lock (pid {pid}, {paths["lock"]})')
            try: paths['lock'].unlink()
            except OSError: pass
    fail(f'could not take {paths["lock"]}')


def fail_job(row, msg):
    row['status'] = 'failed'
    row['error'] = msg.replace('\n', ' ')[:300]
    for k in ('timed_frames', 'first_frame_ms', 'render_ms_med', 'render_ms_p99', 'bind_ms_med', 'gpu_ms_med', 'gpu_ms_p99', 'gpu_wait_ms_med', 'drift_pct', 'run_gpu_ms_med', 'passes_ms'):
        row[k] = ''


def run_job(host, paths, job):
    t0 = time.time()
    deadline = t0 + job['timeout_s'] * job['repeat'] + 60
    row = {'job_id': job['id'], 'host': host.name, 'capture': job['capture'], 'label': job.get('label', ''), 'frames': job['frames'], 'repeat': job['repeat'],
           'warm_frames': job['warm_frames'], 'switches': ' '.join(f'{k}={v}' for k, v in job['switches'].items()), 'status': 'ok', 'error': '', 'exit_code': 0}
    raw = paths['raw'] / job['id']
    started = False   # this job launched rplay (the busy check passed first, so a stranger's rplay is never killed for a "host busy" row)
    finished_ok = False
    try:
        raw.mkdir(parents=True, exist_ok=True)
        if any(k.startswith('fake-') for k in job['switches']) and not isinstance(host, FakeHost): raise RuntimeError('fake-* switches on a real host')
        host.mkdirs(['captures', 'results'])
        if not host.get_file('rplay.sha256', raw / 'rplay.sha256'): raise RuntimeError('exe hash unknown or changed: no rplay.sha256 on the host (lab.py push-exe)')
        want = (raw / 'rplay.sha256').read_text().strip().lower()
        have = host.sha256('rplay.exe')
        if have is None or have != want: raise RuntimeError(f'exe hash unknown or changed: rplay.exe is {str(have)[:12]}, push-exe recorded {want[:12]}')
        row['exe_sha'] = have[:12]
        if not host.exists('captures/' + job['capture']):
            raise RuntimeError(f'capture {job["capture"]} is not on the host (lab.py push <folder>)')
        runs, passes, counts = [], [], []
        for k in range(1, job['repeat'] + 1):
            left = deadline - time.time()
            if left <= 0: raise RuntimeError(f'job deadline passed before repeat {k}')
            b = host.busy()
            if b: raise RuntimeError('host busy: ' + ', '.join(b) + ' is running; no two GPU jobs share the Surface')
            stem = f"results/{job['id']}_r{k}"
            host.rm_glob(stem + '.*')
            for old in raw.glob(f'r{k}.*'): old.unlink()
            started = True
            code = host.run(rplay_args(job, stem + '.csv'), stem + '.log', min(job['timeout_s'], left))
            host.get_file(stem + '.log', raw / f'r{k}.log')
            log = (raw / f'r{k}.log').read_text(errors='replace') if (raw / f'r{k}.log').is_file() else ''
            if code != 0:
                row['exit_code'] = code
                tail = ' | ' + ' / '.join(log.strip().splitlines()[-2:])[:200] if log.strip() else ''
                raise RuntimeError(('rplay timed out' if code == 'timeout' else f'rplay exited {code}') + f' on repeat {k}' + tail)
            m = re.search(r'\[rplay\] capture: (\d+) frames', log)
            if not m: raise RuntimeError(f'repeat {k}: rplay exited 0 but its closing "[rplay] capture: N frames" line is missing (did not finish)')
            if not host.get_file(stem + '.csv', raw / f'r{k}.csv'): raise RuntimeError(f'rplay exited 0 but wrote no timing csv on repeat {k}')
            rows_k = read_timing(raw / f'r{k}.csv')
            n = int(m.group(1))
            if len(rows_k) != n: raise RuntimeError(f'repeat {k}: timing csv has {len(rows_k)} rows but rplay played {n} frames')
            if n < 2: raise RuntimeError(f'repeat {k}: only {n} frame(s) played')
            runs.append(rows_k); counts.append(n)
            if host.get_file(stem + '.csv.passes.csv', raw / f'r{k}.passes.csv'): passes.append(raw / f'r{k}.passes.csv')
        if passes and len(passes) != len(runs): raise RuntimeError('per-pass file present on some repeats but not all')
        row.update(summarise(runs))
        row['passes_ms'] = read_passes(passes) if passes else ''
        row['captured_frames'] = counts[0] if len(set(counts)) == 1 else '/'.join(map(str, counts))
        finished_ok = True
    except HostError as e:
        fail_job(row, str(e))
    except Exception as e:   # a failed job is a failed row, never a 0 ms one
        fail_job(row, str(e))
    finally:
        if started and not finished_ok:   # every path that does not end ok (a failed row, ssh 255, KeyboardInterrupt): nothing of ours keeps the GPU
            try: host.kill()
            except Exception: pass
    row['finished'] = time.strftime('%Y-%m-%d %H:%M:%S')
    row['wall_s'] = round(time.time() - t0, 1)
    append_row(paths, row)
    return row


def cmd_run(a, paths, host):
    jobs = sorted(paths['queue'].glob('*.json')) if paths['queue'].exists() else []
    if not jobs: print('queue empty'); return 0
    acquire_lock(paths)
    got_host = False
    bad = 0
    try:
        try: got_host = host.lock()
        except HostError as e: fail(str(e), 1)
        if not got_host: fail('another lab run holds the Surface lock (run.lock folder in the lab folder; rmdir it if no run is alive)')
        paths['done'].mkdir(parents=True, exist_ok=True)
        for jf in jobs[:a.max or None]:
            job = json.loads(jf.read_text())
            row = run_job(host, paths, job)
            print(f"{row['job_id']}: {row['status']}" + (f" ({row['error']})" if row['error'] else f" gpu {fmt(row['gpu_ms_med'])} ms, render {fmt(row['render_ms_med'])} ms"))
            bad += row['status'] != 'ok'
            shutil.move(str(jf), str(paths['done'] / jf.name))
    finally:
        if got_host:
            try: host.unlock()
            except Exception: pass
        paths['lock'].unlink(missing_ok=True)
    return 1 if bad else 0


def cmd_show(a, paths):
    if not paths['results'].exists(): print('no results yet'); return
    with open(paths['results'], newline='') as f: rows = list(csv.DictReader(f))
    for r in rows[-a.n:]:
        if r['status'] == 'ok':
            drift = f"{float(r['drift_pct']):+.1f}%" if r.get('drift_pct') else 'n/a'
            print(f"{r['finished']}  {r['capture']:<24} {r['switches'] or '-':<24} gpu {float(r['gpu_ms_med']):7.3f} ms (p99 {float(r['gpu_ms_p99']):7.3f})  render {float(r['render_ms_med']):7.3f} ms  x{r['repeat']} drift {drift}  {r['passes_ms']}")
        else:
            print(f"{r['finished']}  {r['capture']:<24} {r['switches'] or '-':<24} FAILED: {r['error']}")


def cmd_status(a, paths, host):
    q = sorted(p.name for p in paths['queue'].glob('*.json')) if paths['queue'].exists() else []
    print(f'host {host.name}; queued {len(q)}; lock {"held" if paths["lock"].exists() else "free"}; results {paths["results"]}')
    for n in q: print('  ', n)


def sha_of(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description='The Surface lab: a queue of rplay replay jobs (see the header of this file)')
    ap.add_argument('--host', default='surface')
    ap.add_argument('--lab-dir', default=str(main_repo() / 'out' / 's102' / 'lab'))
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('submit'); s.add_argument('capture'); s.add_argument('--switch', action='append'); s.add_argument('--frames', type=int, default=300)
    s.add_argument('--repeat', type=int, default=1); s.add_argument('--warm-frames', type=int, default=30); s.add_argument('--timeout', type=int, default=300)
    s.add_argument('--label', default='')
    r = sub.add_parser('run'); r.add_argument('--max', type=int, default=0)
    w = sub.add_parser('show'); w.add_argument('-n', type=int, default=10)
    sub.add_parser('status')
    p = sub.add_parser('push'); p.add_argument('folder')
    p.add_argument('--name', default='', help='the capture name on the host (default: the folder name); S102 main: case zips unpack to a folder called "capture"')
    e = sub.add_parser('push-exe'); e.add_argument('--exe', default=str(REPO / 'out' / 'renderer' / 'surface' / 'rplay.exe'))
    a = ap.parse_args()
    paths = lab_paths(a.lab_dir)
    if a.cmd == 'submit': return cmd_submit(a, paths)
    if a.cmd == 'show': return cmd_show(a, paths)
    host = make_host(a.host)
    try:
        if a.cmd == 'status': return cmd_status(a, paths, host)
        if a.cmd == 'run': return cmd_run(a, paths, host)
        if a.cmd == 'push':
            src = Path(a.folder).resolve()
            name = a.name or src.name
            if not NAME_RE.match(name): fail(f'capture name {name!r} is not a plain name')
            if not (src / 'frames').is_dir(): fail(f'{src} is not a capture folder (no frames\\)')
            host.mkdirs(['captures']); host.put_dir(src, 'captures/' + name); print('pushed', src, 'as', name, 'to', host.name)
        if a.cmd == 'push-exe':
            if not Path(a.exe).is_file(): fail(f'{a.exe} not built (tools\\renderer\\build.ps1 -Surface)')
            tmp = Path(tempfile.mkdtemp()) / 'rplay.sha256'
            tmp.write_text(sha_of(a.exe) + '\n')
            host.mkdirs(['']); host.put_file(a.exe, 'rplay.exe'); host.put_file(tmp, 'rplay.sha256')
            shutil.rmtree(tmp.parent, ignore_errors=True)
            print('pushed rplay.exe + rplay.sha256 to', host.name, sha_of(a.exe)[:12])
    except HostError as e:
        fail(str(e), 1)
    return 0


if __name__ == '__main__':
    sys.exit(main() or 0)
