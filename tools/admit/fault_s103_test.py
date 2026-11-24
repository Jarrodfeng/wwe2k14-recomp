r"""The Session 103 fault re-found with the REAL admit.exe in its test mode, through the wiring helpers (lane CQW; docs\agents\admit_wiring.md).
    py -I tools\admit\fault_s103_test.py

S103: several builders each ran tool builds of 16 compiles at once and nothing limited them machine-wide ("LLVM out of memory"). Here the queue runs in
its test mode (WWE_ADMIT_TEST=1, a state folder under %TEMP%, fake memory numbers); the "builds" are pwsh processes that dot-source tools\admit\Admit.ps1,
call Request-BuildParallel -Want 16, keep their grant for 3 s and call Release-BuildParallel: they compile nothing (0 load).
  F0 the REAL setup: the helpers' own lookup (no WWE_ADMIT_EXE) finds the main checkout's admit.exe. FAILS while it is not installed: the logic rows
     below borrow this checkout's own exe, so they cannot show that the real setup does nothing. The output names both exes.
  F1 two wired builds in two separate processes, the second started about 1 s AFTER the first was granted, memory for ONE 16-wide build at the measured
     2 GB per tool compile (20 GB available, 4 GB floor): the grants x 2 GB never exceed 16 GB at any moment; both are admitted (history: two
     `admitted` rows, neither nested); the second waited for the first.
  F2 PLANTED, the S103 fault: the same with WWE_NO_ADMIT=1 (today): 16 + 16 at once, 64 GB > 16 GB: the same overlap test catches it.
  F3 nested: a build holding 8 starts a child build (its descendant) that wants 16: the child is granted 8, and history shows ONE reservation.
  F4 timeout: memory that never frees (another holder keeps it): Request-BuildParallel -TimeoutS 3 throws and admit_py.hold(timeout_s=3) raises Refused
     (S109 GATE: REFUSED at the bound, exit 75, the loud line), never N and never a forced slot.
admit.exe: env WWE_ADMIT_EXE, else this checkout's out\tools\admit\admit.exe, else the main checkout's.
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

ADMIT_PS1 = HERE / 'Admit.ps1'
GB_PER = 2.0
FAILS = []

BUILD = r"""
. '{admit}'
$n = Request-BuildParallel -Want 16 -TimeoutS {tmo}
$t0 = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
[IO.File]::WriteAllText('{log}.granted', "$n")
Start-Sleep -Milliseconds {hold_ms}
{child}
$t1 = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
Release-BuildParallel
[IO.File]::WriteAllText('{log}', "{name} $n $t0 $t1")
"""


def ok(cond, label):
    print(('PASS ' if cond else 'FAIL ') + label, flush=True)
    if not cond:
        FAILS.append(label)


def find_exe():
    e = os.environ.get('WWE_ADMIT_EXE')
    if e and os.path.isfile(e):
        return e
    own = HERE.parent.parent / 'out' / 'tools' / 'admit' / ('admit' + '.exe')
    return str(own) if own.is_file() else admit_py.admit_exe()


def script(tmp, name, want_child=False, hold_ms=3000, tmo=60):
    log = str(Path(tmp) / f'{name}.row')
    child = ''
    if want_child:
        cs = script(tmp, name + '_child', hold_ms=500)
        child = f"& pwsh -NoProfile -File '{cs}'"
    p = Path(tmp) / f'{name}.ps1'
    p.write_text(BUILD.format(admit=ADMIT_PS1, tmo=tmo, log=log, hold_ms=hold_ms, child=child, name=name), encoding='utf-8')
    return str(p)


def row(tmp, name):
    f = Path(tmp) / f'{name}.row'
    if not f.exists():
        return None
    n, p, t0, t1 = f.read_text().split()
    return (n, int(p), int(t0), int(t1))


def peak_gb(rows):
    return max(sum(p * GB_PER for _x, p, a, b in rows if a <= t0 + 1 < b) for _n, _p, t0, _t1 in rows)


def wait_file(f, s):
    end = time.time() + s
    while time.time() < end and not Path(f).exists():
        time.sleep(0.1)
    return Path(f).exists()


def history(state):
    h = Path(state) / 'history.tsv'
    lines = h.read_text(encoding='utf-8').splitlines() if h.exists() else []
    if not lines:
        return []
    head = lines[0].split('\t')
    return [dict(zip(head, ln.split('\t'))) for ln in lines[1:]]


def setup(tmp, name, avail, mode='enforce', config=None):
    """A test-mode state folder with fake memory; the queue mode is set EXPLICITLY (observe is the program's default)."""
    state = Path(tmp) / name
    state.mkdir()
    (state / 'fake_mem.txt').write_text(f'{avail:g} {avail:g}')
    env = {k: v for k, v in os.environ.items() if k not in ('WWE_NO_ADMIT', 'WWE_BUILD_PARALLEL', 'WWE_ADMIT_HELD')}
    env.update(WWE_ADMIT_EXE=find_exe(), WWE_ADMIT_TEST='1', WWE_ADMIT_DIR=str(state), WWE_ADMIT_CONFIG=str(config or (HERE / 'admit.json')),
               WWE_ADMIT_MODE=mode)
    return state, env


def two_builds(tmp, tag, env):
    a, b = script(tmp, tag + 'A'), script(tmp, tag + 'B')
    pa = subprocess.Popen(['pwsh', '-NoProfile', '-File', a], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    granted = wait_file(str(Path(tmp) / f'{tag}A.row.granted'), 60)
    time.sleep(1.0)   # the second build starts about a second AFTER the first was granted (the review's case: not the same instant)
    pb = subprocess.Popen(['pwsh', '-NoProfile', '-File', b], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    oa, ob = pa.communicate(timeout=180)[0], pb.communicate(timeout=180)[0]
    return granted, [r for r in (row(tmp, tag + 'A'), row(tmp, tag + 'B')) if r], oa + ob


def main():
    # S106 C5F note 7: a real `run_list -Alone` holds a pause the wrappers honour: point this test at a pause file that does not exist (same as wiring_test.isolate_alone)
    os.environ['WWE_ALONE_FILE'] = os.path.join(tempfile.gettempdir(), 'wwe_fault_s103_no_alone_here', 'alone.lock')
    # F0 the REAL setup: the exe the helpers themselves find (the main checkout's), not the one this test borrows for the logic rows below
    saved_exe = os.environ.pop('WWE_ADMIT_EXE', None)
    try:
        real = admit_py.exe_path()
    finally:
        if saved_exe is not None:
            os.environ['WWE_ADMIT_EXE'] = saved_exe
    exe = find_exe()
    print(f'  exe for the logic rows: {exe or "none"}; the helpers\' own lookup: {real} ({"present" if real and os.path.isfile(real) else "MISSING"})')
    # a NOTE, never the lane's verdict (a program not installed on the main checkout is not a builder's fault; the alarm is the helpers' not-installed
    # line and tools\admit\status.py); the verdict comes from the logic rows
    print(f'NOTE F0 real setup: the helpers\' own lookup finds {real}: {"PRESENT" if real and os.path.isfile(real) else "MISSING: every wired build runs unqueued until it is installed"}; '
          f'the logic rows below use {exe or "no exe"}', flush=True)
    if not exe:
        print('FAIL fault_s103_test: no admit.exe for the logic rows (build it: pwsh -NoProfile -File tools\\admit\\build_admit.ps1)')
        return 1
    tmp = tempfile.mkdtemp(prefix='cqw_fault_')
    try:
        avail, floor = 20.0, 4.0
        state, env = setup(tmp, 's1', avail)
        granted, rows, out = two_builds(tmp, 'on', env)
        pk = peak_gb(rows) if len(rows) == 2 else -1
        hist = [h for h in history(state) if h.get('kind') == 'cc.tool']
        outcomes = [h.get('outcome') for h in hist]
        print(f'  with the queue: {[(n, p) for n, p, _a, _b in rows]}, peak {pk:g} GB of {avail - floor:g}; history outcomes {outcomes}')
        a, b = sorted(rows, key=lambda r: r[2]) if len(rows) == 2 else (None, None)
        ok(granted and len(rows) == 2 and 0 < pk <= avail - floor and all(p <= 8 for _n, p, _a, _b in rows),
           f'F1 two builds in separate processes, the second 1 s after the first was granted: grants {[r[1] for r in rows]}, peak {pk:g} GB <= {avail - floor:g} GB')
        ok(outcomes.count('admitted') == 2 and not any(o and o.startswith('nested') for o in outcomes) and b and b[2] >= a[3] - 50,
           f'F1b both admitted, neither nested; the second started after the first let go ({(b[2] - a[3]) if b else "?"} ms): {outcomes}')
        env_off = dict(env, WWE_NO_ADMIT='1')
        _g, rows, _o = two_builds(tmp, 'off', env_off)
        pk = peak_gb(rows) if len(rows) == 2 else -1
        print(f'  PLANT, no queue: {[(n, p) for n, p, _a, _b in rows]}, peak {pk:g} GB')
        ok(len(rows) == 2 and all(p == 16 for _n, p, _a, _b in rows) and pk > avail - floor,
           f'F2 PLANTED (WWE_NO_ADMIT=1, today): 16 + 16 at once, {pk:g} GB > {avail - floor:g} GB: the S103 fault is caught by the same test')
        # F3 nested
        state3, env3 = setup(tmp, 's3', 40.0)
        outer = Path(tmp) / 'outer.ps1'
        child = script(tmp, 'nest_child', hold_ms=300)
        outer.write_text(f". '{ADMIT_PS1}'\n$n = Request-BuildParallel -Want 8 -TimeoutS 60\n[IO.File]::WriteAllText('{tmp}\\outer.n', \"$n\")\n"
                         f"& pwsh -NoProfile -File '{child}'\nRelease-BuildParallel\n", encoding='utf-8')
        p = subprocess.run(['pwsh', '-NoProfile', '-File', str(outer)], env=env3, capture_output=True, text=True, timeout=180)
        on = (Path(tmp) / 'outer.n').read_text().strip() if (Path(tmp) / 'outer.n').exists() else '?'
        cr = row(tmp, 'nest_child')
        h3 = [h for h in history(state3) if h.get('kind') == 'cc.tool']
        reserving = [h for h in h3 if h.get('outcome') == 'admitted']
        print(f'  nested: outer {on}, child {cr[1] if cr else "?"}; history outcomes {[h.get("outcome") for h in h3]}')
        ok(on == '8' and cr and cr[1] == 8 and len(reserving) == 1,
           f'F3 nested: a build holding 8 starts a child wanting 16: the child is granted {cr[1] if cr else "?"} (= the covering 8), ONE reservation in history ({len(reserving)})')
        # F4 timeout: another holder keeps the memory for the whole test
        # S109 GATE: the queue's limit (3 s here) caps every caller; memory that never frees is REFUSED at the bound (exit 75): the PowerShell build throws
        # and compiles nothing, admit_py raises Refused; never the whole want, never a forced slot past the floor
        cfg3 = json.loads((HERE / 'admit.json').read_text(encoding='utf-8'))
        cfg3['timeout_s'] = 3
        cfg3_file = Path(tmp) / 'admit_timeout3.json'
        cfg3_file.write_text(json.dumps(cfg3), encoding='utf-8')
        state4, env4 = setup(tmp, 's4', 20.0, config=cfg3_file)
        saved = {k: os.environ.get(k) for k in list(env4)}
        os.environ.update(env4)
        try:
            with admit_py.hold('cc.tool', 8, timeout_s=30) as occ:
                tps = script(tmp, 'starved', hold_ms=0, tmo=3)
                t0 = time.time()
                p = subprocess.run(['pwsh', '-NoProfile', '-File', tps], env=env4, capture_output=True, text=True, timeout=120)
                dt_ps = time.time() - t0
                sr = row(tmp, 'starved')
                t0 = time.time()
                g = refused_py = None
                try:
                    with admit_py.hold('cc.tool', 16, timeout_s=3) as g:
                        pass
                except admit_py.Refused as r:
                    refused_py = r
                dt_py = time.time() - t0
        finally:
            for k, v in saved.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
        loud = [ln for ln in (p.stdout + p.stderr).splitlines() if 'REFUSED after ' in ln]
        print(f'  timeout: occupier {int(occ)} ({occ.why}); PowerShell -> rc {p.returncode}, row {sr} in {dt_ps:.1f} s; Python -> '
              f'{"Refused " + str(refused_py.code) if refused_py else g} in {dt_py:.1f} s')
        ok(occ.admitted and int(occ) == 8 and sr is None and p.returncode != 0 and loud and dt_ps < 3 * 1.1 + 25,
           f'F4a memory that never frees: Request-BuildParallel -TimeoutS 3 is REFUSED at the 3 s bound (the occupier\'s 16 GB is pending, nothing is left above the floor): '
           f'the build throws and compiles nothing, with the loud REFUSED line: {loud[:1]}')
        ok(refused_py is not None and refused_py.code == 75 and g is None and dt_py < 3 * 1.1 + 25,
           f'F4b admit_py.hold(timeout_s=3) raises Refused (exit 75), the pool never starts: {refused_py.line[:80] if refused_py else g}')
        # F5 a hold kept PAST the pending window still counts for its whole life (CQA case 37's method: a config with pending_window_s 1 and a > 2 s gap)
        cfg = json.loads((HERE / 'admit.json').read_text(encoding='utf-8'))
        cfg['pending_window_s'] = 1
        cfg['timeout_s'] = 3   # S106 0CF: the queue limit is the floor of every caller's limit (B5): 3 s here so the second build times out quickly
        cfg_file = Path(tmp) / 'admit_window1.json'
        cfg_file.write_text(json.dumps(cfg), encoding='utf-8')
        for mode in ('enforce', 'observe'):
            st, ev = setup(tmp, 's5' + mode, 20.0, mode=mode, config=cfg_file)
            first = script(tmp, f'win_{mode}_A', hold_ms=9000)
            second = script(tmp, f'win_{mode}_B', hold_ms=0, tmo=3)
            pa = subprocess.Popen(['pwsh', '-NoProfile', '-File', first], env=ev, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            got_a = wait_file(str(Path(tmp) / f'win_{mode}_A.row.granted'), 60)
            time.sleep(2.5)   # past the 1 s pending window: the first hold's reservation must still count
            pb = subprocess.run(['pwsh', '-NoProfile', '-File', second], env=ev, capture_output=True, text=True, timeout=120)
            pa.communicate(timeout=120)
            ra, rb = row(tmp, f'win_{mode}_A'), row(tmp, f'win_{mode}_B')
            line_b = next((x for x in (pb.stdout + pb.stderr).splitlines() if x.startswith('[admit]')), '')
            hist = history(st)
            print(f'  past the window ({mode}): first {ra[1] if ra else "?"}, second {rb[1] if rb else "?"}: {line_b}; history '
                  f'{[(h.get("outcome"), h.get("workers"), h.get("would_grant")) for h in hist]}')
            if mode == 'enforce':
                # S109 GATE: the second waits its 3 s bound and is REFUSED (a `refused` row, never `admitted`; its script throws); on the old rule (the window
                # expiry) it was admitted at once
                ok(got_a and ra and ra[1] == 8 and rb is None and pb.returncode != 0 and 'REFUSED after ' in (pb.stdout + pb.stderr)
                   and any(h.get('outcome') == 'refused' for h in hist) and not any(h.get('outcome') == 'admitted' and h.get('workers') == '16' for h in hist),
                   'F5a enforce: a first hold of 8 kept 2.5 s past a 1 s pending window still fills the budget: the second (want 16, min 4) is REFUSED at its 3 s bound '
                   '(a `refused` row, the build throws; on the old rule it was admitted at once)')
            else:
                second_rows = [h for h in hist if h.get('queue_mode') == 'observe' and h.get('workers') == '16']
                ok(got_a and rb and rb[1] == 16 and '(observed: not limited' in line_b and any(h.get('would_grant') == '0' for h in second_rows),
                   f'F5b observe: the same second ask is granted its full 16 at once and its history row says would_grant 0: '
                   f'{[(h.get("outcome"), h.get("workers"), h.get("would_grant"), h.get("would_reason")) for h in second_rows]}')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(('FAIL: %d case(s)' % len(FAILS)) if FAILS else 'PASS fault_s103_test: all cases', flush=True)
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
