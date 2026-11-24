#!/usr/bin/env python3
r"""S110 PROVE: run a lane's whole proof as ONE call and print one short verdict ("q multi for proof runs").
The idea of tools\q\q.py multi (reads) and tools\edit\medit.py (edits): do not ask the agent to batch, give it a call that IS a batch.

  py -I tools\handback\prove.py PROOFFILE [--jobs N] [--keep-going | --stop-on-fail] [--root DIR] [--print]
  py -I tools\handback\prove.py --rerun-failed PROOFFILE        (only what did not PASS in its newest run of that same file, and what is new in it)

Start it as ONE background call; read the verdict; open a full log only for a FAIL. Between changes run only the one command you are working on.

PROOFFILE (a small text file written with the Write tool: no shell quoting) holds, one per line:
  - ONE command exactly as typed in PowerShell (`py -I tools\seats\bind_selftest.py`; `.\out\build\x.exe --selftest`). A `;` outside quotes
    refuses the file (exit 2): PowerShell's exit code of `a; b` is b's alone, so a failing `a` would read PASS. `a && b` is one command.
  - `# comment` and blank lines (ignored)
  - the prefix `serial:` = a BARRIER in file order: every command above it has ended before it starts, it runs alone, the commands below it
    start after it ends (a build step, then below it the test that runs what it built); `timeout N:` = N seconds instead of the default
    1200; the two prefixes may be combined (`serial: timeout 60: py -I x.py`)
  - the word `compile` = `py -I tools\compile\game_syntax.py --changed --build both`; the word `lints` = `py -I tools\handback\check.py`
  - a line `base REF` = pass `--base REF` to `lints` and the REF to `compile --changed` (the only two shorthands that take a base)
This tool holds NO list of which checks belong to which files: the agent names the commands, the tool only runs them together and reports.

Each command's stdout + stderr is captured to out\prove\<stamp>\NN_<slug>.txt under the root (scratch); nothing is interactive.
Commands between two `serial:` lines run side by side (default 3 at a time: real compiles, not a load test). --stop-on-fail starts
nothing new after the first failure (the rest are SKIP); the default is --keep-going.

PASS / FAIL come from the EXIT CODE. The one exception: an exit 0 whose summary line says a non-zero failure count (`failures: 2`,
`3 FAIL`, `1 failed`, `selftest: FAIL`) is `FAIL (exit 0, but its summary says failures)`. TIMEOUT = killed after its limit.
NONE = exit 0 but its own line STARTS with `NOTHING CHECKED` (game_syntax --changed on a diff that reaches no game unit): not a pass, not a fail.
The summary line of a command, by generic rules in this order (the last line of the first rule that matches): `NOTHING CHECKED`,
`selftest: OK|PASS|FAIL`, `failures: N`, `total: N PASS, N FAIL`, `PASS: N unit-builds`, `N checks, N failed`, else the last non-empty line.

Verdict: one line per command `PASS|FAIL|TIMEOUT|SKIP|NONE  12.3 s  <command, cut>  |  <summary>`; under a FAIL up to 12 lines that carry the
failure (FAIL / error / PROBLEM / Traceback, and the two lines after the first) and the log path; last line
`prove: N passed, M failed[, K NONE (...)][, RERUN of R of C commands: ...], total T s (wall W s) -> exit 0|1|3`. Exit 0 only if every
command passed; 1 = a command did not pass; 3 = none failed but K checked nothing; 2 = the proof file is unusable.
--rerun-failed: a command's verdict is its newest one over this file's earlier runs; what is not PASS there (or new in the file) runs again, and
the total line says it was a RERUN and which runs the other PASS come from. The final proof before a hand-back is a full run.
The one piece of state: out\prove\<stamp>\result.json of each run, read only by --rerun-failed. 0 game runs."""
import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time

DEFAULT_TIMEOUT = 1200
CUT = 70
SHORTHAND_COMPILE = r'py -I tools\compile\game_syntax.py --changed%s --build both'
SHORTHAND_LINTS = r'py -I tools\handback\check.py%s'
NOTHING_CHECKED = re.compile(r'^\s*NOTHING CHECKED\b')       # the tool's own verdict line STARTS with it (a check that merely names it is not one)
SUMMARY_RULES = (
    NOTHING_CHECKED,
    re.compile(r'selftest:\s*(?:OK|PASS|FAIL)'),
    re.compile(r'\bfailures:\s*\d+'),
    re.compile(r'\btotal:\s*\d+ PASS,\s*\d+ FAIL'),
    re.compile(r'\bPASS:\s*\d+ unit-builds'),
    re.compile(r'\b\d+ checks?, \d+ failed'),
)
FAIL_COUNTS = (
    re.compile(r'\bfailures:\s*([1-9]\d*)'),
    re.compile(r'\b([1-9]\d*) FAIL\b'),
    re.compile(r'\b([1-9]\d*) failed\b'),
    re.compile(r'selftest:\s*(FAIL)'),
)
FAIL_LINE = re.compile(r'\bFAIL|(?i:error)|PROBLEM|Traceback')


class Cmd:
    def __init__(self, raw, line, serial, timeout, lineno, shorthand):
        self.raw, self.line, self.serial, self.timeout, self.lineno, self.shorthand = raw, line, serial, timeout, lineno, shorthand
        self.verdict, self.secs, self.rc, self.log, self.summary, self.excerpt = 'SKIP', 0.0, None, None, '', []


def two_commands(line):
    """a `;` outside quotes: PowerShell's exit code of `a; b` is b's alone (a failing `a` would read PASS)"""
    return ';' in re.sub(r'"(?:[^"`]|`.)*"|\'(?:[^\']|\'\')*\'', '', line)


def parse_proof(text):
    """-> (cmds, base, bad line numbers). A line's prefixes (serial:, timeout N:) in any order, then a command or a shorthand word."""
    cmds, base, rows, bad = [], None, [], []
    for no, ln in enumerate(text.splitlines(), 1):
        s = ln.strip()
        if not s or s.startswith('#'):
            continue
        m = re.match(r'base\s+(\S+)$', s)
        if m:
            base = m.group(1)
            continue
        serial, timeout, rest = False, DEFAULT_TIMEOUT, s
        while True:
            if re.match(r'serial:\s*', rest):
                serial, rest = True, re.sub(r'^serial:\s*', '', rest)
                continue
            m = re.match(r'timeout\s+(\d+)\s*:\s*', rest)
            if m:
                timeout, rest = int(m.group(1)), rest[m.end():]
                continue
            break
        if rest and two_commands(rest):
            bad.append(no)
        elif rest:
            rows.append((s, rest, serial, timeout, no))
    for raw, rest, serial, timeout, no in rows:
        short = rest if rest in ('compile', 'lints') else None
        if rest == 'compile':
            line = SHORTHAND_COMPILE % (' ' + base if base else '')
        elif rest == 'lints':
            line = SHORTHAND_LINTS % (' --base ' + base if base else '')
        else:
            line = rest
        cmds.append(Cmd(raw, line, serial, timeout, no, short))
    return cmds, base, bad


def git_top(path):
    try:
        r = subprocess.run(['git', '-C', path, 'rev-parse', '--show-toplevel'], capture_output=True, text=True, timeout=30)
        if r.returncode == 0 and r.stdout.strip():
            return os.path.normpath(r.stdout.strip())
    except (OSError, subprocess.TimeoutExpired):
        pass
    return None


def powershell():
    return shutil.which('pwsh') or shutil.which('powershell') or 'powershell'


def kill_tree(p):
    if os.name == 'nt':
        subprocess.run(['taskkill', '/T', '/F', '/PID', str(p.pid)], capture_output=True)
    else:
        p.kill()


def run_one(c, idx, outdir, root, shell):
    slug = re.sub(r'[^A-Za-z0-9]+', '_', c.line)[:40].strip('_') or 'cmd'
    c.log = os.path.join(outdir, '%02d_%s.txt' % (idx, slug))
    env = dict(os.environ, PYTHONIOENCODING='utf-8', PYTHONUTF8='1')
    t0 = time.time()
    with open(c.log, 'wb') as f:
        try:
            p = subprocess.Popen([shell, '-NoProfile', '-NonInteractive', '-Command', c.line], cwd=root, stdin=subprocess.DEVNULL,
                                 stdout=f, stderr=subprocess.STDOUT, env=env)
        except OSError as e:
            f.write(('could not start: %r\n' % e).encode())
            c.verdict, c.rc, c.secs = 'FAIL', -1, 0.0
            return
        try:
            c.rc = p.wait(timeout=c.timeout)
            c.verdict = 'PASS' if c.rc == 0 else 'FAIL'
        except subprocess.TimeoutExpired:
            kill_tree(p)
            p.wait()
            c.rc, c.verdict = None, 'TIMEOUT'
    c.secs = time.time() - t0


def read_log(path):
    try:
        with open(path, 'rb') as f:
            return f.read().decode('utf-8', 'replace').replace('\r\n', '\n').replace('\r', '\n').split('\n')
    except OSError:
        return []


def summarize(c):
    lines = [x.rstrip() for x in read_log(c.log)]
    nonempty = [x for x in lines if x.strip()]
    found = None
    for rule in SUMMARY_RULES:
        hit = [x for x in nonempty if rule.search(x)]
        if hit:
            found = hit[-1]
            break
    if found is None:
        found = nonempty[-1] if nonempty else '(no output)'
    c.summary = found.strip()[:160]
    if c.verdict == 'PASS' and NOTHING_CHECKED.search(c.summary):
        c.verdict = 'NONE'
        c.excerpt = ['NOT A PASS: its own line says it checked nothing (drop it from this proof, or name a check that reaches what changed)']
    if c.verdict == 'PASS' and any(r.search(c.summary) for r in FAIL_COUNTS):
        c.verdict = 'FAIL (exit 0, but its summary says failures)'
    if c.verdict.startswith('FAIL') or c.verdict == 'TIMEOUT':
        if c.verdict == 'TIMEOUT':
            c.excerpt = ['(killed after %d s) last output:' % c.timeout] + [x[:200] for x in nonempty[-5:]]
        else:
            idx = [i for i, x in enumerate(lines) if FAIL_LINE.search(x)]
            pick = []
            if idx:
                pick = lines[idx[0]:idx[0] + 3]
                pick += [lines[i] for i in idx[1:] if i > idx[0] + 2]
            else:
                pick = nonempty[-5:]
            c.excerpt = [x[:200] for x in pick if x.strip()][:12]


def run_all(cmds, outdir, root, jobs, stop_on_fail):
    shell = powershell()
    stop = threading.Event()
    idx = {id(c): i + 1 for i, c in enumerate(cmds)}

    def work(c):
        if stop.is_set():
            return
        run_one(c, idx[id(c)], outdir, root, shell)
        if stop_on_fail and c.verdict != 'PASS':
            stop.set()

    def batch(group):
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as ex:
            list(ex.map(work, group))
    group = []
    for c in cmds:              # file order; a serial: command is a barrier: what is above it ends first, it runs alone, what is below starts after it
        if not c.serial:
            group.append(c)
            continue
        batch(group)
        group = []
        work(c)
    batch(group)
    for c in cmds:
        if c.log and os.path.exists(c.log):
            summarize(c)


def cut(s, n=CUT):
    return s if len(s) <= n else s[:n - 3] + '...'


def newest_verdicts(proof_dir, proof_abs):
    """{raw line: (verdict, stamp)}: each command's NEWEST verdict over the earlier runs of this proof file (a rerun holds only its part)"""
    last = {}
    if not os.path.isdir(proof_dir):
        return last
    for d in sorted(os.listdir(proof_dir)):
        rp = os.path.join(proof_dir, d, 'result.json')
        try:
            with open(rp, encoding='utf-8') as f:
                data = json.load(f)
        except (OSError, ValueError):
            continue
        if os.path.normcase(data.get('proof', '')) == os.path.normcase(proof_abs):
            for r in data.get('results', []):
                last[r.get('raw')] = (r.get('verdict'), d)
    return last


def main(argv):
    try:
        sys.stdout.reconfigure(errors='backslashreplace')
    except (AttributeError, OSError):
        pass
    ap = argparse.ArgumentParser(prog='prove.py', description='run a lane\'s whole proof as one call')
    ap.add_argument('proof')
    ap.add_argument('--jobs', type=int, default=3)
    g = ap.add_mutually_exclusive_group()
    g.add_argument('--keep-going', action='store_true')
    g.add_argument('--stop-on-fail', action='store_true')
    ap.add_argument('--rerun-failed', action='store_true')
    ap.add_argument('--root', default=None)
    ap.add_argument('--print', dest='dry', action='store_true', help='print the resolved command lines and run nothing')
    a = ap.parse_args(argv)
    try:
        with open(a.proof, encoding='utf-8-sig') as f:
            text = f.read()
    except OSError as e:
        print('prove: cannot read the proof file: %s' % e)
        return 2
    cmds, base, bad = parse_proof(text)
    if bad:
        print('prove: %s: two commands on one line (`a; b`:PowerShell\'s exit code is the last one\'s alone, a failing `a` would read PASS): '
              'one command per line (`a && b` is fine)' % ', '.join('line %d' % n for n in bad))
        return 2
    if not cmds:
        print('prove: the proof file holds no command')
        return 2
    root = os.path.abspath(a.root) if a.root else (git_top(os.getcwd()) or os.getcwd())
    proof_abs = os.path.abspath(a.proof)
    if a.dry:
        for c in cmds:
            print('%s%s%s' % ('serial: ' if c.serial else '', 'timeout %d: ' % c.timeout if c.timeout != DEFAULT_TIMEOUT else '', c.line))
        return 0
    pdir = os.path.join(root, 'out', 'prove')
    note = rerun = ''
    if a.rerun_failed:
        last = newest_verdicts(pdir, proof_abs)
        if not last:
            note = 'prove: --rerun-failed: no earlier run of this proof file under %s: running everything' % pdir
        else:
            keep = [c for c in cmds if last.get(c.raw, ('', ''))[0] != 'PASS']
            stood = sorted({last[c.raw][1] for c in cmds if c not in keep})
            if not keep:
                print('prove: --rerun-failed: every command of the file PASSed in its newest run (%s): nothing to run (the final proof before a hand-back '
                      'is a full run)' % ', '.join(stood))
                return 0
            if len(keep) < len(cmds):
                rerun = (', RERUN of %d of %d commands: the other %d PASS from run(s) %s, not this one; the final proof before a hand-back is a full run'
                         % (len(keep), len(cmds), len(cmds) - len(keep), ', '.join(stood)))
            note = 'prove: --rerun-failed: %d of the file\'s %d command(s) run again (not PASS in their newest run, or new in the file)' % (len(keep), len(cmds))
            cmds = keep
    stamp = time.strftime('%Y%m%d-%H%M%S')
    outdir, n = os.path.join(pdir, stamp), 1
    while True:
        try:
            os.makedirs(outdir)
            break
        except FileExistsError:
            n += 1
            outdir = os.path.join(pdir, '%s-%d' % (stamp, n))
    print('prove: %d command(s), up to %d at once%s, logs %s' % (len(cmds), a.jobs, ', stop on first failure' if a.stop_on_fail else '', os.path.relpath(outdir, root)))
    if note:
        print(note)
    sys.stdout.flush()
    t0 = time.time()
    run_all(cmds, outdir, root, a.jobs, a.stop_on_fail)
    wall = time.time() - t0
    passed = failed = none = 0
    for c in cmds:
        ok = c.verdict == 'PASS'
        passed += ok
        none += c.verdict == 'NONE'
        failed += (not ok and c.verdict != 'NONE')
        shown = cut(c.line)
        if c.verdict == 'SKIP':
            print('SKIP  0.0 s  %s  |  not started (--stop-on-fail)' % shown)
            continue
        print('%s  %.1f s  %s  |  %s' % (c.verdict, c.secs, shown, c.summary))
        if not ok:
            for x in c.excerpt:
                print('    ' + x)
            print('    log: ' + c.log)
    total = sum(c.secs for c in cmds)
    with open(os.path.join(outdir, 'result.json'), 'w', encoding='utf-8') as f:
        json.dump({'proof': proof_abs, 'results': [{'raw': c.raw, 'verdict': c.verdict, 'secs': round(c.secs, 1), 'rc': c.rc} for c in cmds]}, f, indent=1)
    code = 1 if failed else (3 if none else 0)
    print('prove: %d passed, %d failed%s%s, total %.1f s (wall %.1f s) -> exit %d' % (
        passed, failed, ', %d NONE (checked nothing: not a pass)' % none if none else '', rerun, total, wall, code))
    return code


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
