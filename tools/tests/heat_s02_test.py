"""Session 102 lane S0-2 offline test (0 game runs, nothing on screen): heat and power on the record.
  py -I tools\\tests\\heat_s02_test.py [--root DIR]
Parts:
  A  the steady rule on made-up power.csv files (steady, throttled, flag change, drifting, too short, no record), Python and PowerShell
     (tools\\surface\\heat_steady.ps1, the Surface's copy) must agree on every one;
  B  planted twins that must be REFUSED: a leftover power.csv from "another run" (old mtime), a fixed-rate counter fed as a clock
     (cpu_nominal_mhz / a cycle counter), and the 1600-in-every-row file R5 found;
  C  the real sampler: perf_sys_test.exe --seconds 10 (the game's own [perf][sys] writer) alongside tools\\perf\\thread_busy.ps1 on that process:
     the lines carry perf_pct as a real number (not n/a, never 0) and the thread CSV carries a start-module column naming a module for every row,
     with the named thread named; its planted twin (a CSV with an empty start column) must fail the same check.
Exit 0 only when every real check passes AND every twin is caught."""
import csv
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if '--root' in sys.argv:
    ROOT = Path(sys.argv[sys.argv.index('--root') + 1]).resolve()
sys.path.insert(0, str(ROOT / 'tools' / 'surface'))
import heat_rules as hr  # noqa: E402

OUT = ROOT / 'out' / 's02'
OUT.mkdir(parents=True, exist_ok=True)
fails = []


def check(cond, msg):
    print(('ok   ' if cond else 'FAIL ') + msg)
    if not cond:
        fails.append(msg)


def make_csv(name, n=60, perf=lambda i: 150.0, limit=lambda i: 88.0, flags=lambda i: 2.0, pkg=lambda i: 12000.0, with_perf=True):
    p = OUT / name
    cols = ['time', 't_s'] + (['cpu_perf'] if with_perf else []) + ['perf_limit', 'perf_limit_flags', 'rapl_pkg', 'cpu_nominal_mhz']
    with open(p, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(cols)
        for i in range(n):
            row = ['00:00:00.000', f'{i + 1.0:.3f}'] + ([f'{perf(i):.2f}'] if with_perf else []) + [f'{limit(i):.2f}', f'{flags(i):.2f}', f'{pkg(i):.2f}', '1600.00']
            w.writerow(row)
    return p


HEAT_PS1 = ROOT / 'tools' / 'surface' / 'heat_steady.ps1'


def ps(expr):
    """Runs the Surface's copy under Windows PowerShell 5.1 (powershell.exe), the Surface's shell (review F4b)."""
    cmd = ['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', f". '{HEAT_PS1}'; {expr}"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=90)
    return (r.stdout + r.stderr).strip()


def ps_verdict(p, prev=None):
    return ps(f"(Get-HeatVerdict '{p}'{(' ' + repr(str(prev))) if prev else ''}).Reason")


ver = ps('$PSVersionTable.PSVersion.Major')
check(ver == '5', f'the PowerShell copy runs under Windows PowerShell 5.1 (powershell.exe major version {ver})')


# ---- A: the rule, both implementations
cases = {
    'steady': (make_csv('a_steady.csv', perf=lambda i: 150 + (i % 5)), True),
    'throttled': (make_csv('a_throttled.csv', limit=lambda i: 88 if i < 40 else 60), False),
    'flag_change': (make_csv('a_flags.csv', flags=lambda i: 2 if i < 30 else 8), False),
    'drift': (make_csv('a_drift.csv', perf=lambda i: 170 if i < 35 else 120), False),
    'pkg_drift': (make_csv('a_pkg.csv', pkg=lambda i: 20000 if i < 35 else 9000), False),
    'too_short': (make_csv('a_short.csv', n=15), False),
    'no_record': (make_csv('a_norec.csv', with_perf=False), False),
}
for name, (p, want) in cases.items():
    ok, why = hr.verdict(hr.read_rows(p))
    check(ok == want, f'rule[{name}] python: {why}')
    pv = ps_verdict(p)
    check((pv == 'steady') == want, f'rule[{name}] powershell: {pv}')
    # same first words: both name the same cause (the number formats may differ in the last digit)
    check(pv.split('(')[0].split(' fell')[0][:34] == why.split('(')[0].split(' fell')[0][:34] or pv.startswith(why[:30]), f'rule[{name}] both name the same cause')

# ---- A2: review fixes F1 / F2 / F3 / F4 / vs-previous (both copies; the PowerShell one under 5.1)
def write_rows(name, rows, header='time,t_s,cpu_perf,perf_limit,perf_limit_flags,rapl_pkg'):
    p = OUT / name
    p.write_text(header + '\n' + '\n'.join(rows) + '\n')
    return p


# F1: nan / inf are not readings. A throttle dip with NaN rows scattered through perf_limit must still be "not steady" (a NaN min used to hide it);
# a cpu_perf column that is mostly nan has no heat record.
nan_lim = write_rows('nan_lim.csv', [f'0,{i + 1},150,{"nan" if i % 7 == 0 else (88 if i < 40 else 60)},2,12000' for i in range(60)])
nan_perf = write_rows('nan_perf.csv', [f'0,{i + 1},{"nan" if i % 3 else 150},88,2,12000' for i in range(60)])
inf_lim = write_rows('inf_lim.csv', [f'0,{i + 1},150,{"Infinity" if i % 5 == 0 else (88 if i < 40 else 60)},2,12000' for i in range(60)])
for nm, p in (('nan_lim', nan_lim), ('nan_perf', nan_perf), ('inf_lim', inf_lim)):
    ok, why = hr.verdict(hr.read_rows(p))
    pv = ps_verdict(p)
    check(not ok, f'F1 {nm} python: {why}')
    check(pv.startswith('not steady'), f'F1 {nm} powershell 5.1: {pv}')
# F2: a header row in the middle of the file (real: a power.csv appended across steps) gives a verdict, not an exception.
midhdr_rows = [f'0,{i + 1},150,88,2,12000' for i in range(40)] + ['time,t_s,cpu_perf,perf_limit,perf_limit_flags,rapl_pkg'] + [f'0,{i + 41},150,88,2,12000' for i in range(30)]
midhdr = write_rows('midhdr.csv', midhdr_rows)
ok, why = hr.verdict(hr.read_rows(midhdr))
pv = ps_verdict(midhdr)
check(why.startswith('steady') or why.startswith('not steady'), f'F2 mid-file header row python: {why}')
check(pv.startswith('steady') or pv.startswith('not steady'), f'F2 mid-file header row powershell 5.1 gives a verdict, no throw: {pv}')
check(ok == (pv == 'steady'), 'F2 both copies agree on the mid-header file')
# F3: retry only on heat reasons; never for an A/B arm; never for a crashed / timed-out game; exactly one retry.
stub15 = cases['too_short'][0]
v_stub = ps(f"(Get-HeatVerdict '{stub15}').Retryable")
v_drift = ps(f"(Get-HeatVerdict '{cases['drift'][0]}').Retryable")
v_norec = ps(f"(Get-HeatVerdict '{cases['no_record'][0]}').Retryable")
check(v_stub == 'False' and v_norec == 'False', f'F3 too short / no record are not retryable ({v_stub}, {v_norec})')
check(v_drift == 'True', f'F3 drifting is retryable ({v_drift})')
check(hr.retryable('not steady: too short (6 rows)') is False and hr.retryable('not steady: no heat record') is False
      and hr.retryable('not steady: refused: x missing') is False and hr.retryable('not steady: cpu_perf drifted -28 %') is True, 'F3 python retryable() agrees')


def decide(csvp, step='frames_bryan', is_retry='$false', exit_code=0, timed_out='$false'):
    return ps(f"$v = Get-HeatVerdict '{csvp}'; Test-HeatRetry $v '{step}' {is_retry} {exit_code} {timed_out}")


check(decide(stub15) == 'False', 'F3 15-row stub: no retry')
check(decide(cases['drift'][0]) == 'True', 'F3 drifting: one retry')
check(decide(cases['drift'][0], is_retry='$true') == 'False', 'F3 drifting but already the retry: no second retry (exactly one)')
check(decide(cases['drift'][0], step='ab_1_A') == 'False' and decide(cases['drift'][0], step='abwarm') == 'False', 'F3 A/B arms and abwarm: no retry')
check(decide(cases['drift'][0], exit_code=-1) == 'False', 'F3 crashed game (exit code != 0): no retry')
check(decide(cases['drift'][0], timed_out='$true') == 'False', 'F3 timed-out game: no retry')
check(decide(cases['steady'][0]) == 'False', 'F3 steady run: no retry')
# F4: a leftover threads.csv (appended by thread_busy.ps1) is removed with the other stale files before a step.
sd = OUT / 'stale_step'
sd.mkdir(exist_ok=True)
for f in ('power.csv', 'counters.txt', 'heat_verdict.txt', 'threads.csv', 'wwe2k14_001.log'):
    (sd / f).write_text('old header\n')
removed = ps(f"(Clear-StaleStepFiles '{sd}') -join ','")
check(set(removed.split(',')) == {'power.csv', 'counters.txt', 'heat_verdict.txt', 'threads.csv'}, f'F4 stale files removed: {removed}')
check(not (sd / 'threads.csv').exists() and (sd / 'wwe2k14_001.log').exists(), 'F4 old-header threads.csv gone, the log untouched')
# vs previous step (review item 6): the absolute reference, both copies.
prev_hot = make_csv('prev_hot.csv', perf=lambda i: 150.0, pkg=lambda i: 12000.0)
cur_cool = make_csv('cur_cool.csv', perf=lambda i: 120.0, pkg=lambda i: 9000.0)
vp_py = hr.vs_previous(hr.read_rows(cur_cool), hr.read_rows(prev_hot))
vp_ps = ps_verdict(cur_cool, prev_hot)
check('cpu_perf -20 %' in vp_py and 'package -25 %' in vp_py, f'vs previous (python): {vp_py}')
check('vs previous step: cpu_perf -20 %, package -25 %' in vp_ps, f'vs previous (powershell 5.1): {vp_ps}')
# bench.ps1 parses under 5.1 and ships the pieces (zip carries _rejected folders; retry note; threads.csv in the stale list).
bench_txt = (ROOT / 'tools' / 'surface' / 'bench.ps1').read_text()
check('_rejected' in bench_txt and 'first try:' in bench_txt and 'Test-HeatRetry' in bench_txt and 'Clear-StaleStepFiles' in bench_txt, 'bench.ps1 uses the tested helpers, writes the retry note and zips _rejected folders')
parse = ps(f"$e=$null; [void][System.Management.Automation.Language.Parser]::ParseFile('{ROOT / 'tools' / 'surface' / 'bench.ps1'}',[ref]$null,[ref]$e); $e.Count")
check(parse == '0', f'bench.ps1 parses under 5.1 ({parse} errors)')

# ---- B: planted twins that must be refused
log = OUT / 'wwe2k14_999.log'
log.write_text('x')
fresh = make_csv('b_fresh.csv')
os.utime(fresh, (time.time(), time.time()))
os.utime(log, (time.time(), time.time()))
try:
    hr.check_fresh(str(fresh), str(log))
    check(True, 'a power.csv written by this run is accepted')
except hr.Refused as e:
    check(False, f'fresh power.csv wrongly refused: {e}')
stale = make_csv('b_stale.csv')  # "another run's file": 6 days old
old = time.time() - 6 * 86400
os.utime(stale, (old, old))
try:
    hr.check_fresh(str(stale), str(log))
    check(False, 'TWIN: a power.csv from another run (6 days old) was accepted')
except hr.Refused as e:
    check(True, f'TWIN refused: stale power.csv ({e})')
try:
    hr.check_fresh(str(OUT / 'does_not_exist.csv'), str(log))
    check(False, 'TWIN: a missing power.csv was accepted')
except hr.Refused:
    check(True, 'TWIN refused: missing power.csv')
nominal = [float(r['cpu_nominal_mhz']) for r in hr.read_rows(fresh)]
try:
    hr.check_clock_series(nominal, 'cpu_nominal_mhz')
    check(False, 'TWIN: the fixed 1600 MHz column was accepted as a clock')
except hr.Refused as e:
    check(True, f'TWIN refused: fixed-rate counter as a clock ({e})')
try:
    # R5 section 4.1: 1.75 "GHz" at 88 % of a core, 1.80 at 545 %, the same at every load in between.
    hr.check_clock_vs_load([(88, 1.75), (157, 1.77), (206, 1.85), (372, 1.80), (459, 1.82), (491, 1.79), (545, 1.78), (532, 1.80)], 'thread cycles / cpu_ms')
    check(False, 'TWIN: a thread-cycle "GHz" flat against load (R5) was accepted as a clock')
except hr.Refused as e:
    check(True, f'TWIN refused: cycle-rate series flat against load ({e})')
try:
    r = hr.check_clock_vs_load([(80, 2.9), (100, 2.8), (150, 2.6), (300, 2.2), (450, 1.9), (500, 1.8)], 'a real clock')
    check(r < 0.95, f'a series that sags with load is accepted (ratio {r:.2f})')
except hr.Refused as e:
    check(False, f'a real moving clock was refused: {e}')
try:
    m = hr.check_clock_series([a['cpu_perf'] and float(a['cpu_perf']) for a in hr.read_rows(cases['drift'][0])], 'cpu_perf')
    check(m > 100, f'a moving series (cpu_perf, mean {m:.0f}) is accepted as a clock proxy')
except hr.Refused as e:
    check(False, f'cpu_perf wrongly refused: {e}')

# ---- C: the real sampler and the start-module column
exe = ROOT / 'out' / 'perf_sys_test' / 'perf_sys_test.exe'
if not exe.exists():
    subprocess.run(['pwsh', '-NoProfile', '-File', str(ROOT / 'tools' / 'tests' / 'perf_sys_test.ps1')], capture_output=True, timeout=280)
check(exe.exists(), f'sampler exe built: {exe}')
sys_txt, thr = OUT / 'sys.txt', OUT / 'threads.csv'
for f in (sys_txt, thr):
    if f.exists():
        f.unlink()
proc = subprocess.Popen([str(exe), '--seconds', '10', '--out', str(sys_txt)], stdout=subprocess.DEVNULL, creationflags=0x08000000)
r = subprocess.run(['pwsh', '-NoProfile', '-File', str(ROOT / 'tools' / 'perf' / 'thread_busy.ps1'), '-ProcessName', 'perf_sys_test', '-Out', str(thr),
                    '-WaitForStart', '15'], capture_output=True, text=True, timeout=90)
proc.wait(timeout=60)
lines = sys_txt.read_text().splitlines() if sys_txt.exists() else []
check(len(lines) >= 8, f'sampler wrote {len(lines)} [perf][sys] rows in 10 s (need >= 8)')
perfs = [re.search(r'perf_pct=(\S+)', l) for l in lines]
vals = [m.group(1) for m in perfs if m]
nums = [float(v) for v in vals if v != 'n/a']
check(len(vals) == len(lines) and len(nums) >= 7 and all(0 < v < 1000 for v in nums), f'perf_pct is a real number in the rows (n/a rows: {len(vals) - len(nums)}); first {vals[:3]}')
check(all(' perf_pct=0 ' not in l + ' ' for l in lines), 'no row prints perf_pct=0')
check(all(re.search(r'pkg_w=(n/a|\d+\.\d)', l) for l in lines), 'pkg_w is a number or n/a, never 0 / missing')
check(all('limit_flags=' in l and 'perf_limit=' in l for l in lines), 'perf_limit and limit_flags present')


def thread_check(path):
    rows = list(csv.DictReader(open(path, newline='')))
    if not rows or 'start' not in rows[0]:
        return False, 'no start column'
    if 'mcycles' in rows[0]:
        return False, 'the old mcycles column is still there'
    nostart = [x for x in rows if x['start'] in ('', '-')]
    if nostart:
        return False, f'{len(nostart)} of {len(rows)} rows have no start module'
    if not any(x['name'] == 'heat_burn_named' for x in rows):
        return False, 'the named thread is not in the CSV'
    if not any('perf_sys_test.exe+' in x['start'] for x in rows):
        return False, 'no thread starts in the test exe'
    return True, f'{len(rows)} rows, every one names a start module; modules: ' + ', '.join(sorted({x["start"].split("+")[0] for x in rows}))


ok, why = thread_check(thr) if thr.exists() else (False, 'threads.csv missing')
check(ok, f'thread CSV start column: {why}')
bad = OUT / 'threads_planted.csv'
bad.write_text('time,t_s,tid,name,cpu_ms,interval_ms,mcycles_fixedrate,start\n00:00:01.000,1.0,1,heat_burn_named,10.0,1000.0,3500.00,-\n')
ok2, why2 = thread_check(bad)
check(not ok2, f'TWIN caught: a thread CSV with no start module fails ({why2})')

print('RESULT: ' + ('FAIL (%d)' % len(fails) if fails else 'PASS'))
sys.exit(1 if fails else 0)
