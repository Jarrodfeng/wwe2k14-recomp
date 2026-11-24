# S102 M2: the Surface lab's offline proof 0 game runs, 0 Surface runs: a local folder stands in for the Surface (lab.py --host fake:DIR)
# and lab_fake_rplay.py for rplay. Positive: jobs -> results.csv rows with the right fields. Planted twins that must be FAILED rows with blank ms (never 0 ms):
# rplay exits non-zero / times out / is not finished / writes short, zero or no timing; capture missing; ssh down (not "capture missing"); host busy (nothing killed);
# exe changed or never hashed; a stale csv left by an earlier run. Locks, flag checking, push hygiene, source lints.
#   py -I tools\surface\lab_selftest.py
import ast, csv, importlib.util, json, subprocess, sys, tempfile, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
LAB = HERE / 'lab.py'
fails = []
spec = importlib.util.spec_from_file_location('lab_mod', LAB)
labmod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(labmod)


def check(cond, msg):
    print(('PASS ' if cond else 'FAIL ') + msg)
    if not cond: fails.append(msg)


def lab(tmp, *a, host=None, labdir='lab'):
    return subprocess.run([sys.executable, '-I', str(LAB), '--host', host or f'fake:{tmp / "host"}', '--lab-dir', str(tmp / labdir)] + list(a),
                          capture_output=True, text=True, timeout=120)


def rows(tmp, labdir='lab'):
    p = tmp / labdir / 'results.csv'
    if not p.is_file(): return []
    with open(p, newline='') as f: return list(csv.DictReader(f))


# ---------------------------------------------------------------- source lints (E)
src = LAB.read_text()
tree = ast.parse(src)
SUBPROC = {'run', 'Popen', 'check_output', 'check_call', 'call'}
missing_timeout, raw_ssh = [], []
for node in ast.walk(tree):
    if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and node.func.attr in SUBPROC and isinstance(node.func.value, ast.Name) and node.func.value.id == 'subprocess':
        if not any(k.arg == 'timeout' for k in node.keywords): missing_timeout.append(node.lineno)
for fn in [n for n in ast.walk(tree) if isinstance(n, ast.FunctionDef)]:
    if fn.name in ('_ssh_cmd', '_scp_cmd'): continue
    for node in ast.walk(fn):
        if isinstance(node, ast.List) and node.elts and isinstance(node.elts[0], ast.Constant) and node.elts[0].value in ('ssh', 'scp'): raw_ssh.append(node.lineno)
for node in ast.walk(tree):   # module-level lists too
    pass
check(not missing_timeout, f'every subprocess call in lab.py has timeout= (lines without: {missing_timeout})')
check(not raw_ssh, f'every ssh / scp command is built by _ssh_cmd / _scp_cmd (raw lists at lines {raw_ssh})')
check(all(o in labmod.SSH_OPTS for o in ('ConnectTimeout=10', 'ServerAliveInterval=15', 'ServerAliveCountMax=4')) and 'ssh' not in ''.join(labmod.SSH_OPTS),
      'ssh / scp options carry ConnectTimeout=10, ServerAliveInterval=15, ServerAliveCountMax=4')
check(labmod._ssh_cmd('h', 'x')[:1] == ['ssh'] and 'ServerAliveCountMax=4' in labmod._ssh_cmd('h', 'x') and 'ServerAliveCountMax=4' in labmod._scp_cmd(['a', 'b']),
      'the builders put the options on both ssh and scp')
check(any(isinstance(n, ast.Try) for fn in ast.walk(tree) if isinstance(fn, ast.FunctionDef) and fn.name == 'run_job' for n in ast.walk(fn)) and
      src.index("host.mkdirs(['captures', 'results'])") > src.index('    try:\n        raw.mkdir'), 'run_job does its host.mkdirs inside the try')

# ---------------------------------------------------------------- flag table lint (C), pctl (I)
flags = labmod.rplay_flags()
check(len(flags) >= 40 and flags.get('crowd-ours') is False and flags.get('scale') is True and flags.get('capture') is True and flags.get('timing') is True,
      f'the flag parser finds >= 40 rplay flags incl. crowd-ours (bare) and scale (value): {len(flags)} found')
check(labmod.pctl(list(range(1, 101)), 99) == 99 and labmod.pctl(list(range(1, 201)), 99) == 198 and labmod.pctl([5], 99) == 5, 'pctl is nearest-rank: p99 of 1..100 is 99, of 1..200 is 198')
check(labmod.main_repo().is_dir() and (labmod.main_repo() / 'tools').is_dir() and (labmod.main_repo() / '.git').exists(), 'the default lab dir comes from the main repo')

with tempfile.TemporaryDirectory() as t:
    tmp = Path(t)
    cap = tmp / 'src' / 'cap_a'
    (cap / 'frames').mkdir(parents=True)
    (cap / 'frames' / 'f0.bin').write_bytes(b'x')   # made-up stand-ins; no game data
    cap2 = tmp / 'src' / 'cap_short'
    (cap2 / 'frames').mkdir(parents=True)
    (cap2 / 'frames_limit.txt').write_text('5')
    exe = tmp / 'rplay_dummy.exe'
    exe.write_bytes(b'MZ made up exe v1')
    host = tmp / 'host'

    # --- push (G) and push-exe (D)
    r = lab(tmp, 'push', str(cap)); check(r.returncode == 0 and (host / 'captures' / 'cap_a' / 'frames' / 'f0.bin').is_file(), 'push puts the capture in the host lab folder')
    (cap / 'frames' / 'f0.bin').unlink(); (cap / 'frames' / 'f1.bin').write_bytes(b'y')
    r = lab(tmp, 'push', str(cap))
    check(r.returncode == 0 and (host / 'captures' / 'cap_a' / 'frames' / 'f1.bin').is_file() and not (host / 'captures' / 'cap_a' / 'frames' / 'f0.bin').exists()
          and not (host / 'captures' / 'cap_a.partial').exists(), 'a second push replaces the capture whole (no stale file, no .partial left)')
    check(lab(tmp, 'push', str(cap2)).returncode == 0, 'push the short capture')
    r = lab(tmp, 'push', str(cap / '..'))
    check(r.returncode == 2 and (host / 'captures' / 'cap_a' / 'frames' / 'f1.bin').is_file(), 'planted twin: a literal ".." path exits 2 and the other capture stays intact')
    r = lab(tmp, 'push', str(tmp / 'src' / 'cap_a' / 'frames'))
    check(r.returncode == 2, 'a folder that is not a capture (no frames\\) is refused')
    check(lab(tmp, 'submit', 'cap_a', '--frames', '10').returncode == 0, 'submit before the exe is pushed')
    r = lab(tmp, 'run'); rs = rows(tmp)
    check(r.returncode == 1 and rs and rs[-1]['status'] == 'failed' and 'exe hash unknown or changed' in rs[-1]['error'] and rs[-1]['gpu_ms_med'] == '', 'planted twin: no rplay.sha256 on the host -> failed "exe hash unknown or changed"')
    r = lab(tmp, 'push-exe', '--exe', str(exe))
    check(r.returncode == 0 and (host / 'rplay.sha256').read_text().strip() == labmod.sha_of(exe) and (host / 'rplay.exe').is_file(), 'push-exe writes rplay.sha256 beside the exe')

    # --- positive: three jobs
    check(lab(tmp, 'submit', 'cap_a', '--frames', '40', '--label', 'base').returncode == 0, 'submit job 1')
    check(lab(tmp, 'submit', 'cap_a', '--frames', '40', '--repeat', '3', '--switch', 'crowd-ours=flag', '--switch', 'scale=0.5', '--switch', 'fake-passes=flag', '--label', 'ours').returncode == 0, 'submit job 2 (a bare and a value flag)')
    check(lab(tmp, 'submit', 'cap_short', '--frames', '40').returncode == 0, 'submit job 3 (capture shorter than --frames)')
    n0 = len(rows(tmp))
    r = lab(tmp, 'run'); check(r.returncode == 0, 'run exits 0 when every job succeeds: ' + r.stdout.strip().replace('\n', ' | ')[:200])
    rs = rows(tmp)[n0:]
    check(len(rs) == 3, f'results.csv gained three rows (got {len(rs)})')
    if len(rs) == 3:
        a, b, c = rs
        check(all(x['status'] == 'ok' for x in rs), 'all three rows ok')
        check(10.0 <= float(a['gpu_ms_med']) < 10.01, f"gpu_ms_med = the timestamps' median, ~10.003 (got {a['gpu_ms_med']})")
        check(abs(float(a['render_ms_med']) - 1.4) < 1e-6, f"render_ms_med = (load+bind+render) us, 1.400 (got {a['render_ms_med']}); frame 0 left out")
        check(abs(float(a['first_frame_ms']) - 7.0) < 1e-6, f"first_frame_ms reports frame 0 apart, 7.000 (got {a['first_frame_ms']})")
        check(a['timed_frames'] == '39' and b['timed_frames'] == '117', f"timed frames exclude frame 0 per repeat (39, 117; got {a['timed_frames']}, {b['timed_frames']})")
        check(a['captured_frames'] == '40' and c['captured_frames'] == '5' and c['timed_frames'] == '4', f"the real frame count is recorded (40; a 5-frame capture asked for 40 gives 5; got {a['captured_frames']}, {c['captured_frames']})")
        check(len(b['run_gpu_ms_med'].split('/')) == 3 and b['repeat'] == '3' and b['drift_pct'] != '', 'repeat 3 keeps three per-run medians and a drift')
        check(a['drift_pct'] == '', f"drift_pct is blank for repeat 1, not 0.000 (got {a['drift_pct']!r})")
        check('crowd-ours=flag' in b['switches'] and 'scale=0.5' in b['switches'] and 'shadow=2.000' in b['passes_ms'] and 'main=8.000' in b['passes_ms'], f"switches and pooled per-pass ms recorded (got {b['switches']!r}, {b['passes_ms']!r})")
        check(a['label'] == 'base' and a['capture'] == 'cap_a' and a['wall_s'] != '' and a['exe_sha'] == labmod.sha_of(exe)[:12], 'label, capture, wall time and exe_sha recorded')
    check(len(list((tmp / 'lab' / 'queue').glob('*.json'))) == 0, 'jobs moved from queue to done')
    check(not (tmp / 'lab' / 'run.lock').exists() and not (host / 'run.lock').exists(), 'both locks released')
    sh = lab(tmp, 'show')
    check(sh.returncode == 0 and sh.stdout.count(' gpu ') >= 3 and 'crowd-ours=flag' in sh.stdout and 'FAILED' in sh.stdout, 'show prints the newest rows (ok rows with gpu, the earlier failed one as FAILED)')

    # --- planted twins: each a failed row, blank ms, run exit 1
    def planted(name, submit_args, want, expect_kill=None):
        before = len(rows(tmp))
        kl = host / 'killed.log'
        kills = len(kl.read_text().splitlines()) if kl.is_file() else 0
        s = lab(tmp, 'submit', *submit_args)
        r = lab(tmp, 'run')
        rs2 = rows(tmp)
        ok = (s.returncode == 0 and len(rs2) == before + 1 and rs2[-1]['status'] == 'failed' and rs2[-1]['gpu_ms_med'] == '' and rs2[-1]['render_ms_med'] == ''
              and want in rs2[-1]['error'] and r.returncode == 1)
        check(ok, f'planted twin "{name}" is a failed row with blank ms and run exit 1' + ('' if ok else f' (got {rs2[-1] if rs2 else None}, exit {r.returncode}, {s.stderr.strip()})'))
        if expect_kill is not None:
            now = len(kl.read_text().splitlines()) if kl.is_file() else 0
            check((now > kills) == expect_kill, f'  ... and rplay.exe was {"" if expect_kill else "not "}killed on that path')
        return rs2[-1] if rs2 else {}
    row = planted('rplay exits non-zero', ['cap_a', '--frames', '20', '--switch', 'fake-exit=7'], 'exited 7', True)
    check(row.get('exit_code') == '7', 'the failed row keeps the exit code 7')
    planted('rplay timeout', ['cap_a', '--frames', '20', '--switch', 'fake-hang=flag', '--timeout', '2'], 'timed out', True)
    planted('capture not on host', ['cap_missing', '--frames', '20'], 'not on the host', False)
    planted('no GPU timestamps', ['cap_a', '--frames', '20', '--switch', 'fake-nogpu=flag'], 'gpu_ns 0', True)
    planted('no GPU timestamps from repeat 2 (the pooled median alone would pass)', ['cap_a', '--frames', '20', '--repeat', '2', '--switch', 'fake-nogpu-after=2'], 'repeat 2', True)
    planted('one body frame with gpu_ns 0', ['cap_a', '--frames', '20', '--switch', 'fake-zero-some=flag'], 'gpu_ns 0', True)
    planted('timing csv shorter than the frames played', ['cap_a', '--frames', '20', '--switch', 'fake-short=flag'], 'rows but rplay played', True)
    planted('exit 0 but no csv', ['cap_a', '--frames', '20', '--switch', 'fake-nocsv=flag'], 'no timing csv', True)
    # H: a stale csv from an earlier run must not be read
    s = lab(tmp, 'submit', 'cap_a', '--frames', '20', '--switch', 'fake-nocsv=flag')
    jid = Path(sorted((tmp / 'lab' / 'queue').glob('*.json'))[-1]).stem
    (host / 'results').mkdir(exist_ok=True)
    (host / 'results' / f'{jid}_r1.csv').write_text('frame,draws,load_us,bind_us,render_us,gpu_us,gpu_ns\n' + ''.join(f'{i},1,1,1,1,1,9000000\n' for i in range(20)))
    (tmp / 'lab' / 'raw' / jid).mkdir(parents=True, exist_ok=True)
    (tmp / 'lab' / 'raw' / jid / 'r1.csv').write_text('stale')
    r = lab(tmp, 'run'); last = rows(tmp)[-1]
    check(r.returncode == 1 and last['status'] == 'failed' and 'no timing csv' in last['error'], 'planted twin: a stale <id>_r1.csv on the host (and locally) is deleted, not read -> failed "no timing csv"')
    # F: ssh down is not "capture missing"
    (host / 'ssh-down.flag').write_text('x')
    before = len(rows(tmp)); s = lab(tmp, 'submit', 'cap_a', '--frames', '20'); r = lab(tmp, 'run')
    (host / 'ssh-down.flag').unlink()
    check(r.returncode == 1 and 'ssh failed' in r.stderr and len(rows(tmp)) == before, 'planted twin: ssh down (rc 255) on the lock call -> the run stops with "ssh failed" and writes no row')
    r = lab(tmp, 'run'); last = rows(tmp)[-1]   # the queued job is still there: run it for real now
    check(r.returncode == 0 and last['status'] == 'ok', 'the job left in the queue by the failed run still runs afterwards')
    # ssh dropping mid-job: exists() fails after the locks are held -> a failed row that says ssh, not "not on the host"
    class Drop(labmod.FakeHost):
        def exists(self, rel): raise labmod.HostError('ssh failed (rc 255): dropped')
    paths = labmod.lab_paths(tmp / 'lab'); job = {'id': 'drop_job', 'capture': 'cap_a', 'switches': {}, 'frames': 20, 'repeat': 1, 'warm_frames': 3, 'timeout_s': 20, 'label': ''}
    drow = labmod.run_job(Drop(host), paths, job)
    check(drow['status'] == 'failed' and 'ssh failed' in drow['error'] and 'not on the host' not in drow['error'], 'planted twin: ssh rc 255 during the job is "ssh failed", never "capture not on the host"')
    # A: host busy
    (host / 'busy.flag').write_text('x')
    planted('host busy (another GPU job)', ['cap_a', '--frames', '20'], 'host busy', False)
    (host / 'busy.flag').unlink()
    # D: exe overwritten without push-exe
    (host / 'rplay.exe').write_bytes(b'MZ someone else replaced it')
    planted('exe changed without push-exe', ['cap_a', '--frames', '20'], 'exe hash unknown or changed', False)
    check(lab(tmp, 'push-exe', '--exe', str(exe)).returncode == 0, 're-push the exe')
    # A: Surface-side lock held, then two concurrent runs
    (host / 'run.lock').mkdir()
    lab(tmp, 'submit', 'cap_a', '--frames', '20'); before = len(rows(tmp)); r = lab(tmp, 'run')
    check(r.returncode == 2 and 'Surface lock' in r.stderr and len(rows(tmp)) == before and not (tmp / 'lab' / 'run.lock').exists(), 'planted twin: a held Surface lock folder -> exit 2, no row, the PC lock released')
    (host / 'run.lock').rmdir()
    lab(tmp, 'run')   # drain the queue
    lab(tmp, 'submit', 'cap_a', '--frames', '20', '--switch', 'fake-sleep=4')
    before = len(rows(tmp))
    first = subprocess.Popen([sys.executable, '-I', str(LAB), '--host', f'fake:{host}', '--lab-dir', str(tmp / 'lab'), 'run'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    t0 = time.time()
    while not (tmp / 'lab' / 'run.lock').exists() and time.time() - t0 < 20: time.sleep(0.1)
    second = lab(tmp, 'run')
    out1, err1 = first.communicate(timeout=60)
    check(second.returncode == 2 and 'holds the lock' in second.stderr, 'planted twin: a second concurrent run exits 2 "holds the lock"')
    check(first.returncode == 0 and len(rows(tmp)) == before + 1, f'... and results.csv gained exactly one row from the first run (gained {len(rows(tmp)) - before})')

    # --- C: flag checking, at submit
    for bad, why in (('crowd-our=flag', 'a typo'), ('scale=flag', 'a bare value where one is needed'), ('crowd-ours=5', 'a value on a bare flag'), ('frames=10', 'a reserved name'),
                     ('capture=x', 'a reserved name'), ('timing=x', 'a reserved name'), ('warm-frames=5', 'a reserved name'), ('x=1&calc', 'shell characters')):
        q0 = len(list((tmp / 'lab' / 'queue').glob('*.json')))
        r = lab(tmp, 'submit', 'cap_a', '--switch', bad)
        check(r.returncode == 2 and len(list((tmp / 'lab' / 'queue').glob('*.json'))) == q0, f'planted twin: --switch {bad} ({why}) exits 2 and queues nothing')
    r = lab(tmp, 'submit', 'cap_a', '--switch', 'fake-exit=3', host='surface')
    check(r.returncode == 2 and 'fake-* only' in r.stderr, 'fake-* switches are refused when the host is not fake:')
    check(lab(tmp, 'submit', 'cap a').returncode == 2, 'a capture name with a space is refused')
    for p in (tmp / 'lab' / 'queue').glob('*.json'): p.unlink()

    # --- results.csv: an older lab.py's file is migrated (the first real run's row survives), another header is refused
    old_head = 'job_id,finished,host,capture,switches,label,frames,repeat,warm_frames,status,error,exit_code,timed_frames,first_frame_ms,render_ms_med,render_ms_p99,bind_ms_med,gpu_ms_med,gpu_ms_p99,gpu_wait_ms_med,drift_pct,run_gpu_ms_med,passes_ms,wall_s'
    old_row = '20261114_002608_000_s100_match300,2026-11-14 00:26:57,surface,s100_match300,,rplay starts on Surface,100,2,30,ok,,0,4,12.874,3.147,3.569,1.239,8.647,8.825,9.435,3.934,8.481/8.814,,24.800'
    (tmp / 'lab2').mkdir()
    (tmp / 'lab2' / 'results.csv').write_text(old_head + '\n' + old_row + '\n')
    lab(tmp, 'submit', 'cap_a', '--frames', '20', labdir='lab2'); r = lab(tmp, 'run', labdir='lab2')
    rs = rows(tmp, 'lab2')
    check(r.returncode == 0 and len(rs) == 2 and rs[0]['job_id'].endswith('s100_match300') and rs[0]['gpu_ms_med'] == '8.647' and rs[0]['exe_sha'] == '' and rs[1]['status'] == 'ok'
          and list(rs[0].keys()) == labmod.FIELDS, 'an older results.csv is migrated: its row kept, the new columns blank, the new row appended')
    (tmp / 'lab2' / 'results.csv').write_text('a,b\n1,2\n')
    lab(tmp, 'submit', 'cap_a', '--frames', '20', labdir='lab2'); r = lab(tmp, 'run', labdir='lab2')
    check(r.returncode != 0 and 'different header' in r.stderr, 'a results.csv with an unrelated header is refused')

print('\nlab selftest:', 'FAIL ' + str(len(fails)) if fails else 'PASS')
sys.exit(1 if fails else 0)
