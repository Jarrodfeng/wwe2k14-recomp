r"""PreToolUse hook (Bash | PowerShell): refuse long sleep / poll waits in shell commands (Session 103, 2026-11-17).

Why: builders sleep-polled logs (GT: five 3-minute sleeps) although docs\agents\builder_brief.md says to run long jobs in the background,
not in a sleep-and-grep loop (docs\research\s103\BT_builder_time.md). A written rule did not hold; this makes it a mechanism.
Jarrod, 2026-11-17: "Do all 4 steps, and add the hook".

Reads the hook JSON on stdin; looks only at the command TEXT (a script that sleeps inside is not affected). Refuses (exit 2, reason on
stderr, which the agent reads) when the command holds:
  * `sleep N` / `Start-Sleep [-Seconds] N` / `Start-Sleep -Milliseconds N` / `time.sleep(N)` / `timeout /t N` with N > 30 s
  * a shell poll loop: `until ...; do ... sleep` or `while ...; do ... sleep` (any length)
  * `Wait-Process ... -Timeout N` with N > 60 s
Short waits (<= 30 s) stay allowed, and so does ANY wait run with run_in_background (it blocks nobody; the agent is notified when it ends). Anything unparseable is allowed (fail open: the hook must never block normal work).

    py -I tools\harness\no_long_sleep_hook.py --selftest
"""
import json
import os
import re
import sys

LIMIT_S = 30
WAIT_PROCESS_LIMIT_S = 60
MSG = ("Refused by tools\\harness\\no_long_sleep_hook.py: no long sleeps or poll loops (S103 rule). "
       "Waiting on a long job? Run it with run_in_background and a hard timeout, keep working, and read its log when you are notified; "
       "or make ONE blocking call with a timeout.")


def _num(s):
    try:
        return float(s)
    except ValueError:
        return None


def why_refused(cmd: str):
    c = cmd or ''
    for m in re.finditer(r'(?<![\w-])sleep\s+(\d+(?:\.\d+)?)([smh]?)\b', c):
        n = _num(m.group(1)) or 0
        n *= {'': 1, 's': 1, 'm': 60, 'h': 3600}[m.group(2)]
        if n > LIMIT_S:
            return f'sleep {m.group(1)}{m.group(2)}'
    for m in re.finditer(r'time\.sleep\(\s*(\d+(?:\.\d+)?)', c):
        if (_num(m.group(1)) or 0) > LIMIT_S:
            return f'time.sleep({m.group(1)})'
    for m in re.finditer(r'Start-Sleep\b([^;|&\n]*)', c, re.I):
        args = m.group(1)
        ms = re.search(r'-m(?:illi)?(?:seconds)?\s+(\d+)', args, re.I)
        if ms and (_num(ms.group(1)) or 0) / 1000.0 > LIMIT_S:
            return 'Start-Sleep -Milliseconds ' + ms.group(1)
        if not ms:
            s = re.search(r'(?:-s(?:econds)?\s+)?(\d+(?:\.\d+)?)', args, re.I)
            if s and (_num(s.group(1)) or 0) > LIMIT_S:
                return 'Start-Sleep ' + s.group(1)
    for m in re.finditer(r'\btimeout(?:\.exe)?\s+/t\s+(\d+)', c, re.I):
        if (_num(m.group(1)) or 0) > LIMIT_S:
            return 'timeout /t ' + m.group(1)
    if re.search(r'\b(until|while)\b[^\n]*?;\s*do\b[\s\S]*?\bsleep\b', c):
        return 'a poll loop (until/while ... do ... sleep)'
    for m in re.finditer(r'Wait-Process\b[^;|\n]*-Timeout\s+(\d+)', c, re.I):
        if (_num(m.group(1)) or 0) > WAIT_PROCESS_LIMIT_S:
            return 'Wait-Process -Timeout ' + m.group(1)
    # S103 2026-11-18 22:4x: busy-loop "load generators" pinned the 32-thread PC at 100 % and Jarrod had to restart it.
    # No agent or session ever creates artificial CPU / RAM / disk load on the shared PC (CLAUDE.md, Safety).
    busy = [r'while\s*\(?\s*(?:1|True|true|\$true)\s*\)?\s*:\s*(?:pass|continue|0|None|\.\.\.)',   # py -c "while 1: pass"
            r'while\s*\(\s*(?:1|true|\$true)\s*\)\s*\{\s*\}',                                       # while(1){} / while($true){}
            r'for\s*\(\s*;\s*;\s*\)\s*[{;]',                                                         # for(;;){}
            r'(?:^|[;&|(]\s*|\bstart\s+|\bStart-Process\s+)(?:stress-ng|stress|prime95|cpuburn|consume|cpustres)(?:\.exe)?\s+-',  # stress tools run as a command (not the word in prose)
            r'\b(?:load|cpu|stress|burn)[_-]?gen(?:erator)?\.(?:py|ps1|exe|bat)\b']                  # "load generator" scripts
    for pat in busy:
        if re.search(pat, c, re.I):
            return 'a CPU load generator / busy loop (never on the shared PC: CLAUDE.md Safety)'
    return None


CTRL_MSG = ("Refused by tools\\harness\\no_long_sleep_hook.py: the Python script this command runs holds a control character "
            "(usually 0x08: a `\\b` word boundary that went through a non-raw string or a shell on its way into the file, so the "
            "regex silently never matches; S109: it produced wrong numbers that were reported). Rewrite that line with the Edit / "
            "Write tool, then run it again.")
_CTRL = frozenset(range(0, 9)) | {11, 12} | frozenset(range(14, 32)) | {127}


def script_control_char(cmd: str, cwd: str = ''):
    """S109 (Jarrod: "we should ensure this doesn't happen"): 'FILE:LINE: 0xNN' for the first control character in a .py file
    the command names, else None. Runs for background commands too. Never raises (fail open)."""
    try:
        for tok in re.findall(r'''[^\s"'<>|;&()]+\.py\b''', cmd or ''):
            p = tok if os.path.isabs(tok) else os.path.join(cwd or os.getcwd(), tok)
            if re.match(r'^/[a-zA-Z]/', tok):                       # Git Bash drive path: /c/Users/... -> C:\Users\...
                p = tok[1].upper() + ':' + tok[2:]
            if not os.path.isfile(p) or os.path.getsize(p) > 4_000_000:
                continue
            data = open(p, 'rb').read()
            for i, b in enumerate(data):
                if b in _CTRL:
                    return f'{tok}:{data.count(b"\n", 0, i) + 1}: 0x{b:02X}'
    except Exception:
        return None
    return None


def main():
    if '--selftest' in sys.argv:
        return selftest()
    try:
        data = json.load(sys.stdin)
        ti = data.get('tool_input') or {}
        cmd = ti.get('command', '')
        bad = script_control_char(cmd, str(data.get('cwd') or ''))
        if bad:
            print(f'{CTRL_MSG} (found: {bad})', file=sys.stderr)
            return 2
        try:   # S110: the four shell habits the agent definitions forbid (tools\harness\habit_hook.py; fails open)
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            import habit_hook
            habit = habit_hook.message(cmd)
        except Exception:
            habit = None
        if habit:
            print(habit, file=sys.stderr)
            return 2
        if ti.get('run_in_background'):
            return 0   # a background wait blocks nobody: the agent keeps working and is notified when it ends (S103 SG operator, 21:4x)
    except Exception:
        return 0   # fail open
    why = why_refused(cmd)
    if why:
        print(f'{MSG} (found: {why})', file=sys.stderr)
        return 2
    # S103 (00:0x): chained short sleeps are the same poll ("many 29 s sleep calls" in UPS2). Sum the foreground sleep seconds per session
    # over a 10-minute window; past 90 s, refuse and point at run_in_background.
    try:
        secs = short_sleep_seconds(cmd)
        if secs > 0:
            import re as _re, tempfile, time
            sid = _re.sub(r'[^\w-]', '', str(data.get('session_id', 'x')))[:64] or 'x'
            path = os.path.join(tempfile.gettempdir(), 'wwe_sleepsum_' + sid + '.json')
            try:
                hist = json.load(open(path, encoding='utf-8'))
            except (OSError, ValueError):
                hist = []
            now = time.time()
            hist = [h for h in hist if now - h[0] < 600] + [[now, secs]]
            json.dump(hist, open(path, 'w', encoding='utf-8'))
            if sum(h[1] for h in hist) > 90:
                print(f'{MSG} (found: {int(sum(h[1] for h in hist))} s of chained foreground sleeps in 10 minutes)', file=sys.stderr)
                return 2
    except Exception:
        return 0
    return 0


def short_sleep_seconds(cmd):
    """Seconds of foreground sleep a command asks for (sleep N, Start-Sleep N / -Milliseconds, time.sleep(N), timeout /t N)."""
    c = cmd or ''
    total = 0.0
    for m in re.finditer(r'(?<![\w-])sleep\s+(\d+(?:\.\d+)?)([smh]?)\b', c):
        total += float(m.group(1)) * {'': 1, 's': 1, 'm': 60, 'h': 3600}[m.group(2)]
    for m in re.finditer(r'time\.sleep\(\s*(\d+(?:\.\d+)?)', c):
        total += float(m.group(1))
    for m in re.finditer(r'Start-Sleep\b([^;|&\n]*)', c, re.I):
        ms = re.search(r'-m(?:illi)?(?:seconds)?\s+(\d+)', m.group(1), re.I)
        s = re.search(r'(\d+(?:\.\d+)?)', m.group(1))
        total += (float(ms.group(1)) / 1000.0) if ms else (float(s.group(1)) if s else 0)
    for m in re.finditer(r'\btimeout(?:\.exe)?\s+/t\s+(\d+)', c, re.I):
        total += float(m.group(1))
    return total


def selftest():
    refuse = ['sleep 280', 'py -c "import time;time.sleep(175)"; tail log', 'Start-Sleep -Seconds 120', 'Start-Sleep 45',
              'Start-Sleep -Milliseconds 60000', 'until grep -q PASS log; do sleep 5; done', 'while true; do sleep 10; done',
              'Wait-Process -Id 1 -Timeout 580', 'timeout /t 90', 'sleep 2m', 'cd x && sleep 31',
              'py -c "while 1: pass"', "python -c 'while True: pass' &", 'pwsh -c "while($true){}"', 'stress-ng --cpu 16',
              'py out/load_gen.py',
              r'while true; do py -I tools\harness\wait_commit.py --branch b --after a; sleep 60; done',
              r'until py -I tools\harness\wait_commit.py --branch b --after a; do sleep 5; done',
              r'py -I other\wait_commit.py --branch b --after a; sleep 60']
    allow = ['sleep 5', 'Start-Sleep -Seconds 10', 'time.sleep(0.5)', 'py -I tools/handback/check.py --root .',
             'Start-Sleep -Milliseconds 500', 'git log --oneline -5', 'timeout 900 py -I tools/handback/check.py --root .',
             'grep -n sleep src/x.cpp', 'Wait-Process -Id 1 -Timeout 30', 'echo "sleepy 100"', 'for f in *.txt; do wc -l $f; done',
             'git commit -m "no busy loops, stress tools or load tests on the PC"', 'grep -n "while True" tools/x.py',
             # S108 lane F: the commit wait is a marker wait with its own hard bound (<= 580 s); the old hook already allows it (no sleep in the command)
             r'py -I tools\harness\wait_commit.py --branch b --after abc --timeout 540',
             r'py -I C:\Users\Jarrod\wwe2k14-recomp\tools\harness\wait_commit.py --branch b --after abc --marker HAND-BACK']
    bad = [c for c in refuse if not why_refused(c)] + [c for c in allow if why_refused(c)]
    for c in bad:
        print('FAIL', repr(c), '->', why_refused(c))
    # S109: a script holding a control character is refused before it runs (planted twin: the same script with a real \b passes).
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        good, evil = os.path.join(d, 'ok_script.py'), os.path.join(d, 'bs_script.py')
        open(good, 'w', encoding='utf-8', newline='\n').write("import re\nre.search(r'\\bgit\\b', 'git')\n")
        open(evil, 'wb').write(b"import re\nx = 1\nre.search(r'\x08git\x08', 'git')\n")
        cases = [(f'py -I {evil} --section chains', True), (f'py {good}', False), ('py -I bs_script.py', True),
                 ('py ok_script.py && echo done', False), ('git log --oneline -3', False), ('py -I no_such_file.py', False)]
        for c, want in cases:
            got = script_control_char(c, d)
            if bool(got) != want or (want and not got.endswith(':3: 0x08')):
                bad.append(c)
                print('FAIL control-char case', repr(c), '->', got)
    print('PASS no_long_sleep_hook selftest' if not bad else f'FAIL {len(bad)} case(s)')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
