#!/usr/bin/env python3
"""gate_wait: ONE blocking call that runs the gate and returns when it ends (S103 H3, audit 1.3 item 3; B2F and L1C spent 43-58 min in sleep-and-grep loops
on a gate log, each wait a model turn at full context).

  py -I tools\\gate\\gate_wait.py [--timeout S] [--log FILE] [gate.py arguments...]      e.g.  --only s103_X1 --root .
  py -I tools\\gate\\gate_wait.py --attach LOGFILE [--timeout S]                          wait for a gate someone else started (its log ends with the `gate:` line)

Runs tools\\gate\\gate.py in the foreground (all arguments pass through; --no-cache is refused: builders never pass it), writes the full output to --log (default
out\\gate\\wait_<time>.log under the main checkout) and prints only what a builder needs: every row that is not PASS (full text), the cache-hit count, the final
`gate:` line and the log path. Exit code = the gate's (0 all PASS / SKIP, 1 any FAIL / BUILD, 2 timed out: the gate is killed, 4 green with needs-main rows OWED: tools\\gate\\needs_main.py). A hard timeout (default 1800 s)
means it can never hold a lock for hours."""
import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def summarise(lines, logpath):
    rows = [ln for ln in lines if re.match(r'^(PASS|FAIL|SKIP|BUILD|NEEDS-MAIN|NOT RUN)\s', ln)]
    hits = sum('cache hit' in ln for ln in rows)
    for ln in lines:
        if re.match(r'^(FAIL|BUILD|SKIP|NEEDS-MAIN|NOT RUN)\b', ln) or ln.startswith('[gate]') and ('BUILD FAILED' in ln or 'stale' in ln or '--only leaves out' in ln):
            print(ln)
    print(f'[gate_wait] {len(rows)} check rows, {hits} cache hits; full log: {logpath}')
    nm = next((ln for ln in lines[::-1] if ln.startswith('needs-main:')), '')
    for ln in lines[::-1]:
        if ln.startswith('gate:'):
            print(ln)
            if nm:   # S103 WTG: rows this tree cannot run; the hand-back lists them for the main session. They are not failures: "green with needs-main N"
                n = len([x for x in nm[len('needs-main:'):].split(',') if x.strip()])
                print(nm)
                print(f'[gate_wait] needs-main {n}: not run here, not failed, OWED (exit code 4 = green with needs-main); hand this list back, the main session runs them: py -I tools\\gate\\gate_main.py --rev <commit> --only <rows>')
            if not re.search(r' 0 fail', ln):
                mf, mn = re.search(r', (\d+) fail', ln), re.search(r'\((\d+) of them NOT RUN', ln)
                if mf and mn and mf.group(1) == mn.group(1):   # S103 GST review 10: the only non-pass rows are NOT RUN (the --deadline): the gate ran out of time, nothing failed: 2 = timed out
                    print(f'[gate_wait] timed out: {mn.group(1)} check(s) NOT RUN (the --deadline passed); nothing failed; run the same command again, the finished rows are cached')
                    return 2
                return 1
            return 4 if nm else 0   # 4 = green with needs-main: the listed rows are OWED, never a clean 0
    print('[gate_wait] no final `gate:` line in the output (the gate died early): read the log')
    return 1


def deadline_margin(timeout):
    """Seconds kept back for the gate to wind down (running checks are timed out at the deadline; the summary and cache are written after)."""
    return max(5, min(120, timeout // 10))


def selftest():
    import tempfile
    d = Path(tempfile.mkdtemp(prefix='gate_wait_'))
    ok = True
    log = d / 'g.log'
    log.write_text('PASS  l  a  1.0 s  cache hit (same lane json)\nFAIL  l  b  2.0 s  boom\ngate: 1 of 2 pass or skip, 1 fail, 3.0 s wall\n')
    p = subprocess.run([sys.executable, '-I', str(Path(__file__).resolve()), '--attach', str(log), '--timeout', '20'], capture_output=True, text=True)
    good = p.returncode == 1 and 'FAIL  l  b' in p.stdout and '1 cache hits' in p.stdout and 'gate: 1 of 2' in p.stdout
    print(('ok   ' if good else 'FAIL ') + '--attach returns when the gate line is there, prints only the FAIL row + the cache count + the final line, exit 1')
    ok &= good
    log.write_text('PASS  l  a  1.0 s  x\nNEEDS-MAIN l  c  0.0 s  not run here\ngate: 2 of 2 pass or skip, 0 fail, 1 needs-main, 1.0 s wall\nneeds-main: l.c\n')
    p = subprocess.run([sys.executable, '-I', str(Path(__file__).resolve()), '--attach', str(log)], capture_output=True, text=True)
    good = p.returncode == 4 and 'needs-main: l.c' in p.stdout and 'needs-main 1' in p.stdout and 'NEEDS-MAIN l  c' in p.stdout
    print(('ok   ' if good else 'FAIL ') + '--attach on a gate with only needs-main rows exits 4 (green with needs-main, never 0) and lists them')
    ok &= good
    p = subprocess.run([sys.executable, '-I', str(Path(__file__).resolve()), '--only', 'x', '--no-cache'], capture_output=True, text=True)
    good = p.returncode == 2 and 'refused' in p.stdout
    print(('ok   ' if good else 'FAIL ') + '--no-cache is refused')
    ok &= good
    good = deadline_margin(1800) == 120 and deadline_margin(60) == 6 and deadline_margin(10) == 5 and max(10, 1800 - deadline_margin(1800)) == 1680
    print(('ok   ' if good else 'FAIL ') + 'the gate gets --deadline = timeout minus a margin (1680 s of 1800), never below 10 s')
    ok &= good
    p = subprocess.run([sys.executable, '-I', str(Path(__file__).resolve()), '--only', 'x', '--no-cache', '--deadline', '5'], capture_output=True, text=True)
    good = p.returncode == 2 and 'refused' in p.stdout
    print(('ok   ' if good else 'FAIL ') + 'an explicit --deadline is passed through (and --no-cache is still refused)')
    ok &= good
    log.write_text('PASS  l  a  1.0 s  x\nNOT RUN l  b  0.0 s  --deadline 5 s passed\ngate: 1 of 2 pass or skip, 1 fail (1 of them NOT RUN: the --deadline of 5 s passed; run those again, the finished rows are cached), 5.0 s wall\n')
    p = subprocess.run([sys.executable, '-I', str(Path(__file__).resolve()), '--attach', str(log)], capture_output=True, text=True)
    good = p.returncode == 2 and 'NOT RUN l  b' in p.stdout and 'timed out' in p.stdout
    print(('ok   ' if good else 'FAIL ') + '--attach on a gate whose only non-pass rows are NOT RUN exits 2 (timed out), prints the NOT RUN row, never a plain 1')
    ok &= good
    log.write_text('PASS  l  a  1.0 s  x\nNOT RUN l  b  0.0 s  x\nFAIL  l  c  1.0 s  boom\ngate: 1 of 3 pass or skip, 2 fail (1 of them NOT RUN: the --deadline of 5 s passed), 5.0 s wall\n')
    p = subprocess.run([sys.executable, '-I', str(Path(__file__).resolve()), '--attach', str(log)], capture_output=True, text=True)
    good = p.returncode == 1
    print(('ok   ' if good else 'FAIL ') + '... but a real FAIL beside a NOT RUN row is still exit 1')
    ok &= good
    q = d / 'never.log'
    p = subprocess.run([sys.executable, '-I', str(Path(__file__).resolve()), '--attach', str(q), '--timeout', '1'], capture_output=True, text=True)
    good = p.returncode == 2 and 'timed out' in p.stdout
    print(('ok   ' if good else 'FAIL ') + 'a gate that never ends times out (exit 2) instead of waiting forever')
    ok &= good
    return 0 if ok else 1


def main():
    if '--selftest' in sys.argv[1:]:
        return selftest()
    if any(x in ('-h', '--help') for x in sys.argv[1:]):   # S103: `--help` used to start a real gate run (two builders hit it)
        print(__doc__ or 'gate_wait.py [--timeout S] [--log F] [--attach LOG] <gate.py args, e.g. --root . --only lane,lane.check>')
        return 0
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument('--timeout', type=int, default=1800)
    ap.add_argument('--log', default='')
    ap.add_argument('--attach', default='')
    a, rest = ap.parse_known_args()
    if a.attach:
        t0 = time.time()
        while time.time() - t0 < a.timeout:
            try:
                text = Path(a.attach).read_text(encoding='utf-8', errors='replace')
            except OSError:
                text = ''
            if re.search(r'^gate:', text, re.M):
                return summarise(text.splitlines(), a.attach)
            time.sleep(5)
        print(f'[gate_wait] timed out after {a.timeout} s waiting for {a.attach}')
        return 2
    if '--only' in rest and not any(x == '--tier' or x.startswith('--tier=') for x in rest):
        rest = rest + ['--tier', 'cheap']   # S103 GST: the per-piece gate is the CHEAP checks (CLAUDE.md "The gate is CHEAP checks"); a milestone passes --tier all; --only lane.check always runs
    if '--no-cache' in rest:
        print('[gate_wait] refused: builders never pass --no-cache (the cache key already holds every input; a miss means something changed)')
        return 2
    logpath = a.log or str(ROOT / 'out' / 'gate' / time.strftime('wait_%Y%m%d_%H%M%S.log'))
    Path(logpath).parent.mkdir(parents=True, exist_ok=True)
    if not any(x == '--deadline' or x.startswith('--deadline=') for x in rest):
        # S103 GST: the gate stops dequeuing BEFORE this wrapper's kill and still writes times.csv / cache.json / its summary line: the kill is never blind
        rest = rest + ['--deadline', str(max(10, a.timeout - deadline_margin(a.timeout)))]
    cmd = [sys.executable, '-I', str(HERE / 'gate.py')] + rest
    try:
        p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=a.timeout)
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b'')
        out = out.decode('utf-8', 'replace') if isinstance(out, bytes) else out
        Path(logpath).write_text(out, encoding='utf-8')
        print(f'[gate_wait] TIMEOUT after {a.timeout} s; the gate was killed; partial log: {logpath}')
        return 2
    text = p.stdout + p.stderr
    Path(logpath).write_text(text, encoding='utf-8')
    rc = summarise(text.splitlines(), logpath)
    if p.returncode == 5:   # the gate's own code for "only NOT RUN rows" (S103 GST review 10)
        return 2
    return p.returncode or rc


if __name__ == '__main__':
    sys.exit(main())
