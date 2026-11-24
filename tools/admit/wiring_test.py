r"""Units for the compile queue's wiring helpers (lane CQW; docs\agents\admit_wiring.md). 0 load: a stub admit (tools\admit\wiring_stub_admit.py
behind a .cmd under %TEMP%, named in WWE_ADMIT_EXE), trivial children only.   py -I tools\admit\wiring_test.py   (about 90 s: two hung-admit cases
wait out their bound of timeout + 30 s)

  P1-P14  Request-BuildParallel / Release-BuildParallel (tools\admit\Admit.ps1): the hold asks --min ceil(N/4) --timeout 300 --max-s 900 for this process;
          the grant is passed through; a manual WWE_BUILD_PARALLEL wins with no hold; a missing exe, WWE_NO_ADMIT=1, a failing exe, garbage,
          a grant below the minimum (given back) each give N, never 1; S109 GATE: exit 75 (a refusal, with or without its `refused` line) THROWS;
          a HUNG admit is killed within the bound and N is returned; $LASTEXITCODE is kept; dot-sourcing runs nothing and adds no variable; the release
          ends this script's own hold only (on an error too; never a manual grant; never another script's).
  Y1-Y12  admit_py.hold: the same rules for the stdin form (--min, --timeout); released at the block's end, also on an exception; a hung admit is
          killed (with its tree) within the bound; S109 GATE: exit 75 raises admit_py.Refused (SystemExit 75); `narrowed` is a held grant.
  R1-R4   S109 GATE: the exe judged by content (admit.exe.sha256), rebuilt on ask once per process, never compiled when current or not admitted.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import admit_py  # noqa: E402

PY = sys.executable
ADMIT_PS1 = HERE / 'Admit.ps1'
FAILS = []
# WWE_ADMIT_CONFIG is cleared too: the cases that want it set it
CLEAN_ENV = ('WWE_BUILD_PARALLEL', 'WWE_NO_ADMIT', 'WWE_ADMIT_EXE', 'STUB_MODE', 'STUB_LOG', 'WWE_ADMIT_HELD', 'WWE_ADMIT_CONFIG', 'STUB_SEQ')
CFG = {'path': None}   # a config whose timeout_s is 300 (the cases below were written for a 300 s queue limit); the real admit.json says 900


def make_cfg(tmp, timeout_s):
    p = Path(tmp) / f'cfg_{timeout_s}.json'
    p.write_text(json.dumps({'timeout_s': timeout_s}), encoding='ascii')
    return str(p)


def ok(cond, label):
    print(('PASS ' if cond else 'FAIL ') + label, flush=True)
    if not cond:
        FAILS.append(label)


def make_stub(tmp):
    cmd = Path(tmp) / 'stub_admit.cmd'
    cmd.write_text('@"%s" -I "%s" %%*\r\n' % (PY, HERE / 'wiring_stub_admit.py'), encoding='ascii')
    return str(cmd)


def env_with(**kv):
    e = {k: v for k, v in os.environ.items() if k not in CLEAN_ENV}
    if CFG['path']:
        e['WWE_ADMIT_CONFIG'] = CFG['path']
    e.update({k: str(v) for k, v in kv.items() if v is not None})
    return e


def stub_calls(log):
    p = Path(log)
    if not p.exists():
        return []
    return [json.loads(x) for x in p.read_text(encoding='utf-8').splitlines() if x.strip()]


def pid_alive(pid):
    r = subprocess.run(['tasklist', '/FI', f'PID eq {pid}', '/NH'], capture_output=True, text=True, timeout=30)
    return str(pid) in r.stdout


def ps(body, env):
    p = subprocess.run(['pwsh', '-NoProfile', '-Command', f". '{ADMIT_PS1}'; " + body], capture_output=True, text=True, env=env, timeout=300)
    return p.stdout, p.stdout + p.stderr


def ps_rc(body, env):
    p = subprocess.run(['pwsh', '-NoProfile', '-Command', f". '{ADMIT_PS1}'; " + body], capture_output=True, text=True, env=env, timeout=300)
    return p.returncode, p.stdout + p.stderr


def ps_request(want, env, extra=''):
    body = (f"$global:LASTEXITCODE = 42; $n = Request-BuildParallel -Want {want}{extra}; "
            "Write-Output \"N=$n LEC=$LASTEXITCODE PID=$PID\"")
    so, out = ps(body, env)
    for ln in so.splitlines():
        if ln.startswith('N='):
            parts = dict(x.split('=', 1) for x in ln.split())
            return int(parts['N']), parts['LEC'], parts['PID'], out
    return None, None, None, out


def test_ps(tmp):
    stub = make_stub(tmp)
    log = str(Path(tmp) / 'ps.log')

    def fresh():
        for f in (log, log + '.pid', log + '.n'):
            if os.path.exists(f):
                os.remove(f)
    n, lec, pid, out = ps_request(16, env_with(WWE_ADMIT_EXE=stub, STUB_MODE='grant 5', STUB_LOG=log))
    calls = stub_calls(log)
    ok(n == 5 and calls == [['hold', '--kind', 'cc.tool', '--want', '16', '--min', '4', '--timeout', '300', '--pid', pid, '--detach', '--max-s', '900']],
       f'P1 the grant is passed through (5 of 16); one detached hold for this process: --min 4 (a quarter), --timeout 300, --max-s 900: {calls}')
    ok(lec == '42', f'P2 $LASTEXITCODE is left as the caller had it ({lec})')
    fresh()
    n, _l, _p, out = ps_request(16, env_with(WWE_ADMIT_EXE=stub, STUB_MODE='grant 5', STUB_LOG=log, WWE_BUILD_PARALLEL=4))
    n2, _l, _p, _o = ps_request(2, env_with(WWE_ADMIT_EXE=stub, STUB_MODE='grant 5', STUB_LOG=log, WWE_BUILD_PARALLEL=4))
    ok(n == 4 and n2 == 2 and not stub_calls(log), f'P3 a manual WWE_BUILD_PARALLEL=4 wins: 16 -> {n}, 2 -> {n2}, no hold')
    missing = str(Path(tmp) / 'no_such_admit.exe')
    so, out = ps("$a = Request-BuildParallel -Want 16; $b = Request-BuildParallel -Want 12; Write-Output \"N=$a,$b\"", env_with(WWE_ADMIT_EXE=missing))
    said = [x for x in out.splitlines() if x.startswith('[admit]')]
    ok('N=16,12' in so and said == [f'[admit] not installed ({missing}): this build is not queued'],
       f'P4 a missing exe gives N (16, 12) and says so ONCE per process: {said}')
    n, _l, _p, out = ps_request(16, env_with(WWE_ADMIT_EXE=stub, STUB_MODE='grant 5', STUB_LOG=log, WWE_NO_ADMIT=1))
    ok(n == 16 and not stub_calls(log) and '[admit]' not in out, f'P5 WWE_NO_ADMIT=1 gives N (16 -> {n}), admit is not called, nothing printed')
    for i, (mode, label) in enumerate((('fail', 'a failing exe (exit 3, no grant)'), ('garbage', 'garbage output'))):
        n, _l, _p, out = ps_request(16, env_with(WWE_ADMIT_EXE=stub, STUB_MODE=mode, STUB_LOG=log))
        ok(n == 16 and '(not held:' in out, f'P{6 + i} {label} gives N (16 -> {n}), never 1, with one line: {[x for x in out.splitlines() if "[admit]" in x][:1]}')
    # S109 GATE: exit 75 is a REFUSAL: Request-BuildParallel throws (the script stops, nothing compiles), carrying admit's `refused` line when there is one
    for mode, want_text, label in (('timeout75', 'not admitted within 300 s (exit 75)', 'P8 exit 75 without a line (an older admit)'),
                                   ('refused', 'REFUSED after 3 s: cc.tool needs', 'P8b a `refused` line + exit 75')):
        rc, out = ps_rc("try { $n = Request-BuildParallel -Want 16; 'N=' + $n } catch { 'THROWN ' + $_.Exception.Message }",
                        env_with(WWE_ADMIT_EXE=stub, STUB_MODE=mode, STUB_LOG=log))
        ok('THROWN [admit] cc.tool build of 16: ' in out and want_text in out and 'N=' not in out, f'{label}: Request-BuildParallel throws the refusal, never N: {out.strip()[:200]}')
    fresh()
    n, _l, pid, out = ps_request(16, env_with(WWE_ADMIT_EXE=stub, STUB_MODE='grant 1 held', STUB_LOG=log))
    c = stub_calls(log)
    ok(n == 16 and c[-1:] == [['release', '--pid', pid, '--kind', 'cc.tool']] and 'below the minimum' in out,
       f'P9 a HELD grant below the minimum (1 of 16, min 4) is refused and given back: N, never 1: {c}')
    fresh()
    t0 = time.time()
    n, _l, _p, out = ps_request(16, env_with(WWE_ADMIT_EXE=stub, STUB_MODE='hang', STUB_LOG=log), extra=' -TimeoutS 1 -Interactive')
    dt = time.time() - t0
    spid = Path(log + '.pid').read_text().strip() if Path(log + '.pid').exists() else ''
    ok(n == 16 and 31 <= dt < 32 + 20 and spid and not pid_alive(spid) and 'no answer in 32 s' in out,
       f'P10 a HUNG admit: N after the bound (ceil(1.1 x 1) + 30 = 32 s; took {dt:.0f} s) and the stub (pid {spid}) is dead')
    # the answer words: the count and the one line tell the truth
    rows = []
    for mode, want in (('grant 5 held', 16), ('grant 16 observed', 16), ('grant 3 nested', 16), ('grant 16 open', 16), ('grant 5', 16)):
        fresh()
        n, _l, _p, out = ps_request(want, env_with(WWE_ADMIT_EXE=stub, STUB_MODE=mode, STUB_LOG=log))
        rows.append((mode, n, next((x for x in out.splitlines() if x.startswith('[admit]')), '')))
    exp = [('grant 5 held', 5, '(held: granted 5 of 16'), ('grant 16 observed', 16, '(observed: not limited'), ('grant 3 nested', 3, '(nested under'),
           ('grant 16 open', 16, "(not held: admit's error path"), ('grant 5', 5, '(held: granted 5 of 16')]
    ok(all(r[1] == e[1] and e[2] in r[2] for r, e in zip(rows, exp)) and not any('until the loop ends' in r[2] for r in rows if r[0] in ('grant 16 observed', 'grant 16 open')),
       f'P15 the answer words: held 5 / observed 16 (not limited) / nested 3 (accepted BELOW the minimum 4) / open 16 (not held) / a bare `granted 5` = held: {[(r[0], r[1]) for r in rows]}')
    # Ctrl+C: a stopped request kills its admit at once (it can never win a grant afterwards)
    fresh()
    body = ("$ps = [powershell]::Create(); [void]$ps.AddScript(\". '" + str(ADMIT_PS1) + "'; Request-BuildParallel -Want 16 -TimeoutS 300\"); "
            "$h = $ps.BeginInvoke(); $t = [DateTime]::UtcNow.AddSeconds(30); while (-not (Test-Path ($env:STUB_LOG + '.pid')) -and [DateTime]::UtcNow -lt $t) { Start-Sleep -Milliseconds 200 }; "
            "Start-Sleep -Milliseconds 500; $t0 = [DateTime]::UtcNow; $ps.Stop(); Write-Output (\"STOP=\" + [int]([DateTime]::UtcNow - $t0).TotalMilliseconds)")
    so, out = ps(body, env_with(WWE_ADMIT_EXE=stub, STUB_MODE='hang', STUB_LOG=log))
    spid = Path(log + '.pid').read_text().strip() if Path(log + '.pid').exists() else ''
    stop_ms = next((int(x.split('=')[1]) for x in so.splitlines() if x.startswith('STOP=')), None)
    time.sleep(1)
    ok(stop_ms is not None and stop_ms < 5000 and spid and not pid_alive(spid),
       f'P16 a stopped request (Ctrl+C) returns in {stop_ms} ms (500 ms slices, not up to 330 s) and its admit (pid {spid}) is killed')
    fresh()
    so, out = ps("'loaded'; (Get-Variable | Where-Object { $_.Name -like 'Admit*' }).Count", env_with(WWE_ADMIT_EXE=stub, STUB_MODE='grant 5', STUB_LOG=log))
    ok(so.split()[:2] == ['loaded', '0'] and not stub_calls(log), f'P11 dot-sourcing Admit.ps1 calls nothing and adds no variable: {so.split()[:2]}')
    body = ("Set-StrictMode -Version Latest; try { $n = Request-BuildParallel -Want 16; throw 'compile failed' } catch { 'caught' } finally { Release-BuildParallel }; "
            "Release-BuildParallel; Write-Output \"PID=$PID\"")
    so, out = ps(body, env_with(WWE_ADMIT_EXE=stub, STUB_MODE='grant 5', STUB_LOG=log))
    pid = next((ln.split('=', 1)[1] for ln in so.splitlines() if ln.startswith('PID=')), '?')
    c = stub_calls(log)
    ok(len(c) == 2 and c[1] == ['release', '--pid', pid, '--kind', 'cc.tool'],
       f'P12 Release-BuildParallel in a finally ends this script\'s own hold on an error, once (strict mode; a second call is a no-op): {c}')
    fresh()
    so, out = ps("$n = Request-BuildParallel -Want 16; Release-BuildParallel; 'done'", env_with(WWE_ADMIT_EXE=stub, STUB_MODE='grant 5', STUB_LOG=log, WWE_BUILD_PARALLEL=4))
    ok('done' in so and not stub_calls(log), 'P13 under a manual WWE_BUILD_PARALLEL Release-BuildParallel releases nothing')
    inner = Path(tmp) / 'inner.ps1'
    inner.write_text(f". '{ADMIT_PS1}'\nRelease-BuildParallel\n", encoding='utf-8')
    so, out = ps(f"$n = Request-BuildParallel -Want 16; & '{inner}'; 'done'", env_with(WWE_ADMIT_EXE=stub, STUB_MODE='grant 5', STUB_LOG=log))
    c = stub_calls(log)
    ok('done' in so and len(c) == 1 and c[0][0] == 'hold', f'P14 another script\'s Release-BuildParallel (same process) does not end this hold: {c}')
    # a wired script run with & inside another wired script's process: its hold nests under the first; its own release must do nothing, the outer's ends
    fresh()
    inner.write_text(f". '{ADMIT_PS1}'\ntry {{ $m = Request-BuildParallel -Want 16 }} finally {{ Release-BuildParallel }}\n", encoding='utf-8')
    so, out = ps(f"try {{ $n = Request-BuildParallel -Want 16; & '{inner}' }} finally {{ Release-BuildParallel }}; Write-Output \"PID=$PID\"",
                 env_with(WWE_ADMIT_EXE=stub, STUB_SEQ='grant 8 held;grant 8 nested', STUB_LOG=log))
    pid = next((ln.split('=', 1)[1] for ln in so.splitlines() if ln.startswith('PID=')), '?')
    c = stub_calls(log)
    ok([x[0] for x in c] == ['hold', 'hold', 'release'] and c[2] == ['release', '--pid', pid, '--kind', 'cc.tool'],
       f'P17 an & script in the same process gets `nested`: its release does nothing, the outer\'s held hold is released once: {[x[0] for x in c]}')
    # which answers Release-BuildParallel gives back: held and observed (an observed record must not linger in later "would" decisions); never nested / open
    got = {}
    for word in ('held', 'observed', 'nested', 'open'):
        fresh()
        so, out = ps("try { $n = Request-BuildParallel -Want 16 } finally { Release-BuildParallel }; 'done'",
                     env_with(WWE_ADMIT_EXE=stub, STUB_MODE=f'grant 16 {word}', STUB_LOG=log))
        got[word] = [x[0] for x in stub_calls(log)]
    ok(got == {'held': ['hold', 'release'], 'observed': ['hold', 'release'], 'nested': ['hold'], 'open': ['hold']},
       f'P18 Release-BuildParallel ends this call\'s hold when it was held OR observed, never nested / open: {got}')
    # S106 0CF (review B5): a 20 s caller still waits the queue's own limit; an interactive one keeps its 20 s
    cfg900 = make_cfg(tmp, 900)
    fresh()
    ps_request(16, env_with(WWE_ADMIT_EXE=stub, STUB_MODE='grant 5', STUB_LOG=log, WWE_ADMIT_CONFIG=cfg900), extra=' -TimeoutS 20')
    a = stub_calls(log)
    fresh()
    ps_request(16, env_with(WWE_ADMIT_EXE=stub, STUB_MODE='grant 5', STUB_LOG=log, WWE_ADMIT_CONFIG=cfg900), extra=' -TimeoutS 20 -Interactive')
    b = stub_calls(log)
    ok(a and b and a[0][a[0].index('--timeout') + 1] == '20' and b[0][b[0].index('--timeout') + 1] == '20',
       f'P19 (S109 GATE) every caller\'s limit is passed as given (admit cuts it to the queue\'s): --timeout 20 both: {a[:1]} {b[:1]}')


def test_py(tmp):
    stub = make_stub(tmp)
    log = str(Path(tmp) / 'py.log')
    saved = {k: os.environ.get(k) for k in CLEAN_ENV}

    def setenv(**kv):
        for f in (log, log + '.pid', log + '.n'):
            if os.path.exists(f):
                os.remove(f)
        for k in CLEAN_ENV:
            os.environ.pop(k, None)
        if CFG['path']:
            os.environ['WWE_ADMIT_CONFIG'] = CFG['path']
        for k, v in kv.items():
            os.environ[k] = str(v)
    try:
        setenv(WWE_ADMIT_EXE=stub, STUB_MODE='grant 3', STUB_LOG=log)
        with admit_py.hold('q.parse_worker', 8, timeout_s=300) as n:
            during = stub_calls(log)
        after = stub_calls(log)
        ok(n == 3 and n.admitted and during == [['hold', '--kind', 'q.parse_worker', '--want', '8', '--min', '2', '--timeout', '300']] and after[-1] == 'released',
           f'Y1 the grant is passed through (3 of 8, min 2, --timeout 300), held during the block, released at its end: {after}')
        setenv(WWE_ADMIT_EXE=stub, STUB_MODE='grant 3', STUB_LOG=log, WWE_BUILD_PARALLEL=2)
        with admit_py.hold('syntax.worker', 8, timeout_s=120) as n:
            pass
        ok(n == 2 and not n.admitted and not stub_calls(log), 'Y2 a manual WWE_BUILD_PARALLEL wins (2), no hold')
        res = []
        for kv in (dict(WWE_ADMIT_EXE=str(Path(tmp) / 'nope.exe')), dict(WWE_ADMIT_EXE=stub, STUB_MODE='grant 3', STUB_LOG=log, WWE_NO_ADMIT=1),
                   dict(WWE_ADMIT_EXE=stub, STUB_MODE='fail', STUB_LOG=log), dict(WWE_ADMIT_EXE=stub, STUB_MODE='garbage', STUB_LOG=log)):
            setenv(**kv)
            with admit_py.hold('syntax.worker', 6, timeout_s=120) as g:
                pass
            res.append(g)
        ok(all(g == 6 and not g.admitted for g in res), f'Y3 missing exe / WWE_NO_ADMIT=1 / failing exe / garbage each give N (6), never 1: {[(int(g), g.why) for g in res]}')
        # S109 GATE: exit 75 is a REFUSAL: admit_py.Refused (a SystemExit, code 75) leaves the with block before the pool starts; `except Exception` cannot catch it
        got = []
        for mode in ('timeout75', 'refused'):
            setenv(WWE_ADMIT_EXE=stub, STUB_MODE=mode, STUB_LOG=log)
            ran = False
            try:
                try:
                    with admit_py.hold('syntax.worker', 6, timeout_s=120):
                        ran = True
                except Exception:
                    got.append((mode, 'swallowed by except Exception'))
                    continue
            except admit_py.Refused as r:
                got.append((mode, r.code, isinstance(r, SystemExit), r.line[:40], ran))
        ok(got == [('timeout75', 75, True, 'not admitted within 120 s (exit 75)', False), ('refused', 75, True, 'REFUSED after 3 s: cc.tool needs 1 x 2.0', False)],
           f'Y12 exit 75 (no line / a `refused` line) raises Refused (SystemExit 75, the line carried), the block never runs: {got}')
        setenv(WWE_ADMIT_EXE=stub, STUB_MODE='grant 1 held', STUB_LOG=log)
        with admit_py.hold('cc.game_src', 12, timeout_s=300) as g:
            pass
        ok(g == 12 and not g.admitted and stub_calls(log)[-1:] == ['released'], f'Y4 a HELD grant below the minimum (1 of 12, min 3) is refused and given back: N: {stub_calls(log)}')
        got = []
        for mode in ('grant 12 observed', 'grant 2 nested', 'grant 12 open', 'grant 5 held', 'grant 1 narrowed', 'grant 1 forced'):
            setenv(WWE_ADMIT_EXE=stub, STUB_MODE=mode, STUB_LOG=log)
            with admit_py.hold('cc.game_src', 12, timeout_s=300) as g:
                pass
            got.append((int(g), g.why, g.admitted))
        ok(got == [(12, 'observed', False), (2, 'nested', False), (12, 'not held', False), (5, 'held', True), (1, 'narrowed', True), (1, 'narrowed', True)],
           f'Y9 the answer words: observed 12 (not limited), nested 2 (accepted below the minimum 3), open -> 12 not held, held 5, narrowed 1 (an older `forced` too): {got}')
        setenv(WWE_ADMIT_EXE=str(Path(tmp) / 'nope2.exe'))
        admit_py._SAID['not_installed'] = False
        import io
        import contextlib as cl
        buf = io.StringIO()
        with cl.redirect_stderr(buf):
            for _i in range(2):
                with admit_py.hold('cc.tool', 4, timeout_s=10):
                    pass
        said = [x for x in buf.getvalue().splitlines() if x.startswith('[admit]')]
        ok(said == [f'[admit] not installed ({Path(tmp) / "nope2.exe"}): this build is not queued'], f'Y10 a missing exe is said ONCE per process: {said}')
        setenv(WWE_ADMIT_EXE=stub, STUB_MODE='grant 2', STUB_LOG=log)
        try:
            with admit_py.hold('cc.tool', 4, timeout_s=300):
                raise KeyError('boom')
        except KeyError:
            pass
        ok(stub_calls(log)[-1:] == ['released'], 'Y5 an exception in the block still releases the hold')
        setenv(WWE_ADMIT_EXE=stub, STUB_MODE='hang', STUB_LOG=log)
        old = admit_py.GRACE_S
        t0 = time.time()
        with admit_py.hold('q.parse_worker', 8, timeout_s=1, interactive=True) as g:
            pass
        dt = time.time() - t0
        spid = Path(log + '.pid').read_text().strip() if Path(log + '.pid').exists() else ''
        ok(g == 8 and not g.admitted and old + 2 - 1 <= dt < old + 2 + 20 and spid and not pid_alive(spid),
           f'Y6 a HUNG admit: N after the bound (ceil(1.1 x 1) + {old} = {old + 2} s; took {dt:.0f} s), the stub (pid {spid}) and its .cmd are dead')
        ok(admit_py.min_of(1) == 1 and admit_py.min_of(8) == 2 and admit_py.min_of(12) == 3 and admit_py.min_of(16) == 4 and admit_py.min_of(5) == 2,
           'Y7 the minimum is a quarter of the want, rounded up, at least 1')
        src = (HERE / 'admit_py.py').read_text(encoding='utf-8')
        ok("'--on-timeout'" not in src and 'def hold(kind, want, timeout_s, interactive=False)' in src, 'Y8 hold takes a required timeout and never asks admit to run after a timeout')
        # S106 0CF (review B5): a 20 s caller waits the queue's own limit; an interactive one keeps its 20 s
        cfg900 = make_cfg(tmp, 900)
        setenv(WWE_ADMIT_EXE=stub, STUB_MODE='grant 3', STUB_LOG=log, WWE_ADMIT_CONFIG=cfg900)
        with admit_py.hold('q.parse_worker', 8, timeout_s=20):
            pass
        a = stub_calls(log)
        setenv(WWE_ADMIT_EXE=stub, STUB_MODE='grant 3', STUB_LOG=log, WWE_ADMIT_CONFIG=cfg900)
        with admit_py.hold('q.parse_worker', 8, timeout_s=20, interactive=True):
            pass
        b = stub_calls(log)
        ok(a[0][a[0].index('--timeout') + 1] == '20' and b[0][b[0].index('--timeout') + 1] == '20',
           f'Y11 (S109 GATE) every caller\'s limit is passed as given (admit cuts it to the queue\'s): --timeout 20 both: {a[0][-2:]} {b[0][-2:]}')
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v


def test_rebuild(tmp):
    """S109 GATE: the exe is judged by CONTENT and rebuilt on ask. A temp main tree (<t>\\out\\tools\\admit\\admit.exe + <t>\\tools\\admit\\admit.cpp):
    R1 exe_current: no stamp False, equal True, other False, no source None; R2 admit_exe() runs <t>\\tools\\admit\\build_admit.ps1 -IfStale ONCE per process
    (a fake script that only logs; WWE_ADMIT_EXE unset, the tree found through a patched exe_path); R3 the real build_admit.ps1 -IfStale with a current stamp
    exits 0 and compiles nothing; R4 the real build_admit.ps1 -IfStale whose own ask is REFUSED (the worktree's admit.exe in test mode, fake memory 1 GB,
    timeout_s 2) keeps the old exe, says so, exits 3, and compiles nothing (the coordinator's note 2: a rebuild is never a wait on memory)."""
    import hashlib
    t = Path(tmp) / 'rb_main'
    (t / 'out' / 'tools' / 'admit').mkdir(parents=True)
    (t / 'tools' / 'admit').mkdir(parents=True)
    exe = t / 'out' / 'tools' / 'admit' / 'admit.exe'
    src = t / 'tools' / 'admit' / 'admit.cpp'
    exe.write_bytes(b'not a real exe')
    src.write_bytes(b'// admit.cpp v1\n')
    h1 = hashlib.sha256(src.read_bytes()).hexdigest()
    r = [admit_py.exe_current(str(exe))]
    Path(str(exe) + '.sha256').write_text(h1 + '\n', encoding='ascii')
    r.append(admit_py.exe_current(str(exe)))
    src.write_bytes(b'// admit.cpp v2\n')
    r.append(admit_py.exe_current(str(exe)))
    src.rename(src.with_suffix('.bak'))
    r.append(admit_py.exe_current(str(exe)))
    src.with_suffix('.bak').rename(src)
    ok(r == [False, True, False, None], f'R1 exe_current by content: no stamp / equal / changed source / no source -> False True False None: {r}')
    marker = Path(tmp) / 'rb_called.txt'
    (t / 'tools' / 'admit' / 'build_admit.ps1').write_text(f"param([switch]$IfStale)\nAdd-Content -LiteralPath '{marker}' \"called IfStale=$IfStale\"\nWrite-Host '[admit] fake rebuild'\nexit 3\n",
                                                          encoding='utf-8')
    saved_path, saved_env = admit_py.exe_path, os.environ.pop('WWE_ADMIT_EXE', None)
    admit_py._SAID_CACHE.pop('rebuilt', None)
    try:
        admit_py.exe_path = lambda: str(exe)
        import io
        import contextlib as cl
        buf = io.StringIO()
        with cl.redirect_stderr(buf):
            a = admit_py.admit_exe()
            b = admit_py.admit_exe()
    finally:
        admit_py.exe_path = saved_path
        admit_py._SAID_CACHE.pop('rebuilt', None)
        if saved_env is not None:
            os.environ['WWE_ADMIT_EXE'] = saved_env
    calls = marker.read_text(encoding='utf-8').splitlines() if marker.exists() else []
    said = buf.getvalue()
    ok(a == b == str(exe) and calls == ['called IfStale=True'] and '[admit] fake rebuild' in said and 'still older than admit.cpp' in said,
       f'R2 a stale stamp runs build_admit.ps1 -IfStale once per process, then the old exe is used and said: calls {calls}, said {said.strip()[:160]!r}')
    # R2b (GATE review): a main checkout whose build_admit.ps1 predates -IfStale (a worktree of this lane before the merge) is NOT run: that script
    # ignores the switch and compiles unconditionally, without asking the queue, over the machine's production exe
    marker.unlink(missing_ok=True)
    (t / 'tools' / 'admit' / 'build_admit.ps1').write_text(f"Add-Content -LiteralPath '{marker}' 'called'\nexit 0\n", encoding='utf-8')
    admit_py._SAID_CACHE.pop('rebuilt', None)
    try:
        admit_py.exe_path = lambda: str(exe)
        buf = io.StringIO()
        with cl.redirect_stderr(buf):
            c2 = admit_py.admit_exe()
    finally:
        admit_py.exe_path = saved_path
        admit_py._SAID_CACHE.pop('rebuilt', None)
        if saved_env is not None:
            os.environ['WWE_ADMIT_EXE'] = saved_env
    ok(c2 == str(exe) and not marker.exists() and 'predates' in buf.getvalue(),
       f'R2b a pre-GATE build_admit.ps1 (no -IfStale) is never run by a caller: called {marker.exists()}, said {buf.getvalue().strip()[:160]!r}')
    real = HERE / 'build_admit.ps1'
    shutil.copy(real, t / 'tools' / 'admit' / 'build_admit.ps1')
    Path(str(exe) + '.sha256').write_text(hashlib.sha256(src.read_bytes()).hexdigest() + '\n', encoding='ascii')
    p = subprocess.run(['pwsh', '-NoProfile', '-File', str(t / 'tools' / 'admit' / 'build_admit.ps1'), '-IfStale'], capture_output=True, text=True, timeout=120)
    ok(p.returncode == 0 and not (t / 'out' / 'tools' / 'admit' / 'admit.new.exe').exists() and exe.read_bytes() == b'not a real exe',
       f'R3 build_admit.ps1 -IfStale with a current stamp: exit 0, nothing compiled: rc {p.returncode} {(p.stdout + p.stderr).strip()[:160]!r}')
    own = HERE.parent.parent / 'out' / 'tools' / 'admit' / 'admit.exe'   # this checkout's exe (the GATE build), as the tree's "old" exe
    if not own.is_file():
        ok(False, f'R4 needs this checkout\'s admit.exe ({own}): build it with tools\\admit\\build_admit.ps1')
        return
    shutil.copy(own, exe)
    src.write_bytes(b'// admit.cpp v3: the stamp is now stale\n')
    state = Path(tmp) / 'rb_state'
    state.mkdir()
    (state / 'fake_mem.txt').write_text('1 1', encoding='ascii')
    cfg = json.loads((HERE / 'admit.json').read_text(encoding='utf-8'))
    cfg['timeout_s'] = 2
    cfgp = Path(tmp) / 'rb_cfg.json'
    cfgp.write_text(json.dumps(cfg), encoding='utf-8')
    env = env_with(WWE_ADMIT_DIR=str(state), WWE_ADMIT_TEST='1', WWE_ADMIT_MODE='enforce', WWE_ADMIT_CONFIG=str(cfgp))
    before = exe.read_bytes()
    t0 = time.time()
    p = subprocess.run(['pwsh', '-NoProfile', '-File', str(t / 'tools' / 'admit' / 'build_admit.ps1'), '-IfStale'], capture_output=True, text=True, env=env, timeout=120)
    out = p.stdout + p.stderr
    ok(p.returncode == 3 and 'its rebuild was not admitted' in out and 'REFUSED' in out and exe.read_bytes() == before and not (t / 'out' / 'tools' / 'admit' / 'admit.new.exe').exists()
       and time.time() - t0 < 60, f'R4 a rebuild whose own ask is refused keeps the old exe, says so, exit 3, nothing compiled: rc {p.returncode} {out.strip()[:220]!r}')


def isolate_alone(tmp, plant=False):
    """S106 C5F note 7: a real `run_list -Alone` holds a pause the wrappers honour: the hung-admit cases (bound = timeout + 30 s) would wait it out. WWE_ALONE_FILE is
    pointed at a path that does not exist for the whole run (the plant leaves the ambient value)."""
    if not plant:
        os.environ['WWE_ALONE_FILE'] = str(Path(tmp) / 'no_alone_here' / 'alone.lock')


def test_alone_isolated(tmp):
    import msvcrt
    held = Path(tmp) / 'ambient_alone.lock'
    fd = os.open(str(held), os.O_RDWR | os.O_CREAT)
    try:
        os.write(fd, b'.' + json.dumps({'pid': os.getpid(), 'token': 'ambient', 'name': 'a real alone run', 'until': int(time.time()) + 600}).encode())
        os.lseek(fd, 0, 0)
        msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)   # the ambient pause: held, a live record
        saved = os.environ.get('WWE_ALONE_FILE')
        try:
            os.environ['WWE_ALONE_FILE'] = str(held)
            isolate_alone(tmp, plant=True)
            ok(admit_py._pause_left() > 0, 'C5F plant: with the ambient pause left in place the wrappers see a live pause (the detector can fail)')
            isolate_alone(tmp)
            ok(admit_py._pause_left() == 0 and not os.path.exists(os.environ['WWE_ALONE_FILE']), 'C5F note 7: after isolate_alone the wrappers see no pause (WWE_ALONE_FILE -> a missing path)')
        finally:
            if saved is None:
                os.environ.pop('WWE_ALONE_FILE', None)
            else:
                os.environ['WWE_ALONE_FILE'] = saved
            os.lseek(fd, 0, 0)
            msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
    finally:
        os.close(fd)


def main():
    tmp = tempfile.mkdtemp(prefix='cqw_wiring_')
    CFG['path'] = make_cfg(tmp, 300)
    test_alone_isolated(tmp)
    isolate_alone(tmp)
    try:
        test_ps(tmp)
        test_py(tmp)
        test_rebuild(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(('FAIL: %d case(s): %s' % (len(FAILS), '; '.join(f[:60] for f in FAILS))) if FAILS else 'PASS wiring_test: all cases', flush=True)
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
