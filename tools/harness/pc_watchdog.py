"""PC watchdog (S103, 2026-11-18): NOTICES and ALERTS, never kills, suspends or re-prioritises anything.

  py tools\\harness\\pc_watchdog.py            one pass (run by tidy.py --auto watch, the 10-minute task wwe2k14-tidy-watch)
  py tools\\harness\\pc_watchdog.py --dry      sample and print what it would alert; writes nothing
  py tools\\harness\\pc_watchdog.py --selftest fake samples for each rule, de-duplication and the never-kill lint (no real load)

Why: a busy-loop "load generator" pinned the PC for ~40 min and a background task ran for 6 h (Jarrod, 2026-11-18).
Rules (state: out\\harness\\watchdog\\state.json):
  a) CPU >= 95 % on two consecutive passes -> alert with the top 8 processes (CPU and command line)
  b) free RAM < 3 GB -> alert
  c) one of our processes older than its kind's limit with no log change (or no known log) -> alert; limits: game 30 min,
     build (clang / ninja / cmake / build.ps1) 90 min, any other python / pwsh of ours 3 h
  d) a command line matching the pre-command hook's busy-loop patterns (no_long_sleep_hook.why_refused) -> alert at once
Alerts: one line each in out\\harness\\watchdog\\alerts.log, the flag file ALERT.txt (read by session_start.py and the main
session's heartbeat; delete it to clear), and a Windows toast like tidy.py's (a scheduled task cannot reach the phone; no such
path exists, so the toast + the files are the whole path). The same finding alerts once per hour.
Sampling: ctypes GetSystemTimes / GlobalMemoryStatusEx and one PowerShell Get-CimInstance Win32_Process call (about 1 s).
"""
import csv
import io
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
WD = os.path.join(REPO, 'out', 'harness', 'watchdog')
HOUR = 3600
CPU_PCT = 95.0
RAM_MIN_GB = 3.0
OURS = re.compile(r'^(python|pythonw|py|pwsh|powershell|clang.*|ninja|cmake|wwe2k14.*|rplay.*|fview.*|host_rig.*|c_driver.*|.*_unit|git)\.exe$', re.I)
SCRIPTERS = re.compile(r'^(python|pythonw|py|pwsh|powershell)\.exe$', re.I)
LIMITS_MIN = {'game': 30, 'build': 90, 'other': 180}
NO_WINDOW = 0x08000000


def kind_of(name, cmd):
    n, c = name.lower(), (cmd or '').lower()
    if n.startswith('wwe2k14'):
        return 'game'
    if n.startswith(('clang', 'ninja', 'cmake')) or 'build.ps1' in c or 'fast_build' in c:
        return 'build'
    return 'other'


def sys_cpu_times():
    import ctypes
    from ctypes import wintypes
    i, k, u = wintypes.FILETIME(), wintypes.FILETIME(), wintypes.FILETIME()
    ctypes.windll.kernel32.GetSystemTimes(ctypes.byref(i), ctypes.byref(k), ctypes.byref(u))
    q = lambda f: (f.dwHighDateTime << 32) | f.dwLowDateTime
    idle, kern, user = q(i), q(k), q(u)      # kernel time includes idle
    return idle, kern + user


def free_ram_gb():
    import ctypes

    class MS(ctypes.Structure):
        _fields_ = [('l', ctypes.c_ulong), ('load', ctypes.c_ulong), ('tp', ctypes.c_ulonglong), ('ap', ctypes.c_ulonglong),
                    ('tv', ctypes.c_ulonglong), ('av', ctypes.c_ulonglong), ('x', ctypes.c_ulonglong), ('y', ctypes.c_ulonglong),
                    ('z', ctypes.c_ulonglong)]
    m = MS()
    m.l = ctypes.sizeof(MS)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
    return m.ap / 2 ** 30


def process_list():
    """-> [{pid, name, cmd, start (epoch s), cpu (cumulative CPU s)}] for our process names only."""
    ps = ("Get-CimInstance Win32_Process | Select-Object ProcessId,Name,CommandLine,"
          "@{n='Start';e={if($_.CreationDate){[int64]([DateTimeOffset]$_.CreationDate).ToUnixTimeSeconds()}else{0}}},"
          "@{n='Cpu';e={[double]($_.UserModeTime+$_.KernelModeTime)/1e7}} | ConvertTo-Csv -NoTypeInformation")
    try:
        out = subprocess.run(['powershell.exe', '-NoProfile', '-Command', ps], capture_output=True, text=True,
                             encoding='utf-8', errors='replace', timeout=60, creationflags=NO_WINDOW).stdout
    except (OSError, subprocess.TimeoutExpired):
        return []
    procs = []
    for r in csv.DictReader(io.StringIO(out)):
        try:
            if OURS.match(r['Name'] or '') and int(r['ProcessId']) != os.getpid():
                procs.append({'pid': int(r['ProcessId']), 'name': r['Name'], 'cmd': r['CommandLine'] or '',
                              'start': float(r['Start'] or 0), 'cpu': float(r['Cpu'] or 0)})
        except (ValueError, KeyError):
            pass
    return procs


def sample():
    idle, total = sys_cpu_times()
    return {'t': time.time(), 'idle': idle, 'total': total, 'ram_gb': free_ram_gb(), 'procs': process_list()}


def log_mtime(cmd):
    """(newest mtime, path) of any existing file named in the command line that looks like a log / output; (None, None) if none."""
    best, found = 0, None
    for m in re.finditer(r'[A-Za-z]:\\[^"\s]+\.(?:log|txt|csv|json|jsonl)', cmd or ''):
        try:
            mt = os.path.getmtime(m.group(0))
            if mt > best:
                best, found = mt, m.group(0)
        except OSError:
            pass
    return (best, found) if found else (None, None)


def evaluate(s, prev, state, hook_refused=None, mtime_of=log_mtime):
    """-> list of (key, text) findings. prev = the previous sample (or None); state['hot'] counts consecutive hot passes."""
    hook_refused = hook_refused or (lambda c: None)
    out = []
    pm = {p['pid']: p for p in (prev or {}).get('procs', [])}
    cpu_pct = None
    if prev and s['total'] > prev['total']:
        cpu_pct = 100.0 * (1 - (s['idle'] - prev['idle']) / float(s['total'] - prev['total']))
    deltas = {}
    for p in s['procs']:
        q = pm.get(p['pid'])
        if q and q['start'] == p['start'] and s['t'] > prev['t']:
            deltas[p['pid']] = (p['cpu'] - q['cpu']) / (s['t'] - prev['t']) * 100.0     # % of one core
    if cpu_pct is not None:
        state['hot'] = state.get('hot', 0) + 1 if cpu_pct >= CPU_PCT else 0
        if state['hot'] >= 2:
            top = sorted(s['procs'], key=lambda p: -deltas.get(p['pid'], 0))[:8]
            out.append(('cpu', 'CPU %.0f%% on %d passes in a row; top: %s' % (cpu_pct, state['hot'], ' || '.join(
                '%s pid %d %.0f%% %s' % (p['name'], p['pid'], deltas.get(p['pid'], 0), p['cmd'][:160]) for p in top))))
    if s['ram_gb'] < RAM_MIN_GB:
        out.append(('ram', 'free RAM %.1f GB (under %.0f GB)' % (s['ram_gb'], RAM_MIN_GB)))
    for p in s['procs']:
        age_min = (s['t'] - p['start']) / 60.0 if p['start'] else 0
        if SCRIPTERS.match(p['name']):
            why = hook_refused(p['cmd'])
            if why:
                out.append(('busy:%d' % p['pid'], 'busy-loop signature: %s pid %d, age %.0f min: %s' % (p['name'], p['pid'], age_min, p['cmd'][:200])))
                continue
        k = kind_of(p['name'], p['cmd'])
        # Long-lived by design (S103 02:2x: the session's MCP servers ran 188 min at 0 % CPU and were flagged): servers the
        # Claude session itself starts live as long as the session; only their CPU / busy-loop signature matters.
        if 'mcp_server.py' in p['cmd'] or (deltas.get(p['pid']) == 0 and p['name'].lower() == 'python.exe' and not p['cmd'].strip()):
            continue
        if age_min > LIMITS_MIN[k]:
            mt, lf = mtime_of(p['cmd'])
            quiet_min = (s['t'] - mt) / 60.0 if mt else None
            if quiet_min is None or quiet_min > 30:
                d = deltas.get(p['pid'])
                out.append(('old:%d' % p['pid'], '%s over its %d min limit and no log change: %s pid %d, age %.0f min, cpu %s, log %s: %s' % (
                    k, LIMITS_MIN[k], p['name'], p['pid'], age_min, ('%.0f%%' % d) if d is not None else '?',
                    ('%s (quiet %.0f min)' % (lf, quiet_min)) if lf else 'unknown', p['cmd'][:200])))
    return out


def dedupe(findings, state, now):
    """Only findings not alerted in the last hour; records the new ones in state['sent']."""
    sent = state.setdefault('sent', {})
    for k in [k for k, t in sent.items() if now - t > 24 * HOUR]:
        del sent[k]
    new = [(k, t) for k, t in findings if now - sent.get(k, 0) > HOUR]
    for k, _ in new:
        sent[k] = now
    return new


def stamp(t):
    return time.strftime('%Y-%m-%d %H:%M', time.localtime(t))


def toast(text):
    ps = ("[Windows.UI.Notifications.ToastNotificationManager,Windows.UI.Notifications,ContentType=WindowsRuntime]|Out-Null;"
          "$t=[Windows.UI.Notifications.ToastNotificationManager]::GetTemplateContent(1);"
          "$t.GetElementsByTagName('text')[0].AppendChild($t.CreateTextNode('WWE2K14 PC watchdog: %s'))|Out-Null;"
          "[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier('WWE2K14 watchdog').Show([Windows.UI.Notifications.ToastNotification]::new($t))"
          % text[:200].replace("'", "''"))
    try:
        subprocess.run(['powershell.exe', '-NoProfile', '-Command', ps], capture_output=True, timeout=30, creationflags=NO_WINDOW)
    except (OSError, subprocess.TimeoutExpired):
        pass


def write_alerts(new, now, wd=WD, notify=toast):
    os.makedirs(wd, exist_ok=True)
    lines = ['%s  %s' % (stamp(now), t) for _, t in new]
    for name in ('alerts.log', 'ALERT.txt'):
        with open(os.path.join(wd, name), 'a', encoding='utf-8') as f:
            f.write('\n'.join(lines) + '\n')
    notify('%d finding(s): %s' % (len(new), new[0][1]))


def run_pass(dry=False, wd=WD):
    from no_long_sleep_hook import why_refused
    sp = os.path.join(wd, 'state.json')
    try:
        with open(sp, encoding='utf-8') as f:
            state = json.load(f)
    except (OSError, ValueError):
        state = {}
    prev = state.get('prev')
    s = sample()
    findings = evaluate(s, prev, state, hook_refused=why_refused)
    new = dedupe(findings, state, s['t'])
    print('watchdog %s: ram %.1f GB free, %d of our processes, %d finding(s), %d new' % (
        stamp(s['t']), s['ram_gb'], len(s['procs']), len(findings), len(new)))
    for _, t in new:
        print('  ALERT' + (' (dry, not written)' if dry else '') + ': ' + t)
    if dry:
        return 0
    state['prev'] = s
    os.makedirs(wd, exist_ok=True)
    with open(sp, 'w', encoding='utf-8') as f:
        json.dump(state, f)
    if new:
        write_alerts(new, s['t'], wd)
    return 0


def selftest():
    import tempfile
    from no_long_sleep_hook import why_refused
    now = 1_000_000.0
    mk = lambda t, idle, total, procs, ram=16.0: {'t': t, 'idle': idle, 'total': total, 'ram_gb': ram, 'procs': procs}
    P = lambda pid, name, cmd, age_min, cpu=0.0: {'pid': pid, 'name': name, 'cmd': cmd, 'start': now - age_min * 60, 'cpu': cpu}
    nolog = lambda c: (None, None)
    # (a) CPU: needs two consecutive hot passes
    st = {}
    a = mk(now, 0, 0, [P(1, 'python.exe', 'python x.py', 5, 0)])
    b = mk(now + 600, 20, 1000, [P(1, 'python.exe', 'python x.py', 15, 590)])
    c = mk(now + 1200, 40, 2000, [P(1, 'python.exe', 'python x.py', 25, 1180)])
    assert evaluate(b, a, st, mtime_of=nolog) == [] and st['hot'] == 1
    f = evaluate(c, b, st, mtime_of=nolog)
    assert [k for k, _ in f] == ['cpu'] and 'python x.py' in f[0][1] and st['hot'] == 2
    cool = mk(now + 1800, 1900, 3000, [P(1, 'python.exe', 'python x.py', 35, 1200)])
    evaluate(cool, c, st, mtime_of=nolog)
    assert st['hot'] == 0
    # (b) RAM
    assert [k for k, _ in evaluate(mk(now, 0, 0, [], 2.0), None, {}, mtime_of=nolog)] == ['ram']
    assert evaluate(mk(now, 0, 0, [], 8.0), None, {}, mtime_of=nolog) == []
    # (c) over limit per kind; a fresh log change silences it; under the limit is silent
    procs = [P(10, 'wwe2k14.exe', 'wwe2k14.exe', 31), P(12, 'ninja.exe', 'ninja', 91), P(13, 'pwsh.exe', 'pwsh -File x.ps1', 181),
             P(14, 'wwe2k14.exe', 'wwe2k14.exe', 29), P(15, 'python.exe', 'python y.py', 179)]
    ks = sorted(k for k, _ in evaluate(mk(now, 0, 0, procs), None, {}, mtime_of=nolog))
    assert ks == ['old:10', 'old:12', 'old:13'], ks
    fresh = lambda c: (now - 60, 'C:\\x.log')
    assert evaluate(mk(now, 0, 0, procs), None, {}, mtime_of=fresh) == []
    old_log = lambda c: (now - 3600, 'C:\\x.log')
    assert len(evaluate(mk(now, 0, 0, procs[:1]), None, {}, mtime_of=old_log)) == 1
    # (d) busy-loop signature, at once, even for a young process (the pattern text is built, not written out whole)
    loop = 'while ' + '1: pass'
    bl = [P(20, 'python.exe', 'python -c "%s"' % loop, 1), P(21, 'python.exe', 'python tools\\q\\q.py fact x', 1)]
    f = evaluate(mk(now, 0, 0, bl), None, {}, hook_refused=why_refused, mtime_of=nolog)
    assert [k for k, _ in f] == ['busy:20'], f
    # de-duplication: the same finding once per hour
    sd = {}
    fd = [('ram', 'x')]
    assert len(dedupe(fd, sd, now)) == 1 and dedupe(fd, sd, now + 1800) == [] and len(dedupe(fd, sd, now + 3700)) == 1
    # alert files (temp dir, fake notifier)
    with tempfile.TemporaryDirectory() as d:
        calls = []
        write_alerts([('ram', 'free RAM 2 GB')], now, d, notify=calls.append)
        assert os.path.exists(os.path.join(d, 'ALERT.txt')) and 'free RAM' in open(os.path.join(d, 'alerts.log')).read() and len(calls) == 1
    # never-kill lint: the module above this function holds no kill / terminate / suspend / priority call
    src = open(os.path.abspath(__file__), encoding='utf-8').read()
    body = src.split('def selftest' + '():')[0]
    pats = [r'\.kill\(', r'\.terminate\(', 'Terminate' + 'Process', 'task' + 'kill', 'Stop-' + 'Process', 'Suspend' + 'Thread',
            'NtSuspend', 'SetPriority' + 'Class', r'os\.kill', 'Priority' + 'Class', r'\bnice\(']
    bad = [p for p in pats if re.search(p, body)]
    assert not bad, bad
    print('pc_watchdog selftest OK (cpu x2 passes, ram, 3 limits + fresh-log + under-limit, busy-loop, dedupe, alert files, never-kill lint)')
    return 0


if __name__ == '__main__':
    if '--selftest' in sys.argv:
        sys.exit(selftest())
    sys.exit(run_pass(dry='--dry' in sys.argv))
