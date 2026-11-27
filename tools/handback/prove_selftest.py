r"""S110 PROVE: selftest of tools\handback\prove.py on fixtures built in temp folders (tiny python scripts as the "commands"), and of PLANTED
TWINS of the tool (the pattern of tools\edit\medit_selftest.py): each rule is first seen to FAIL against a copy of prove.py with one planted fault.

    py -I tools\handback\prove_selftest.py

Exit 0 = every check passes on the shipped tool and at least one check fails on every planted twin (a twin seen only through a crash of the
tool is NOT caught). The twins run in child processes (`--suite PATH`), 3 at a time. The shorthands are resolved with `--print`: the real
compile and lints never run here. Real short waits only (a 4.5 s sleeper, 1 s stamps); no busy loop. 0 game runs, writes only a temp folder.
"""
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROVE = os.path.join(HERE, 'prove.py')

# (name, [(pattern in prove.py (\r?\n for a line break), replacement)]): one planted fault each
PLANTS = [
    ('serial_runs_in_parallel', [(r"        if not c\.serial:", "        if True:")]),
    ('jobs_forced_to_one', [(r"max_workers=max\(1, jobs\)", "max_workers=1")]),
    ('exit_code_ignored', [(r"c\.verdict = 'PASS' if c\.rc == 0 else 'FAIL'", "c.verdict = 'PASS'")]),
    ('failures_exception_dropped', [(r"if c\.verdict == 'PASS' and any\(r\.search\(c\.summary\) for r in FAIL_COUNTS\):", "if False:")]),
    ('summary_rules_ignored', [(r"for rule in SUMMARY_RULES:", "for rule in ():")]),
    ('no_failure_excerpt', [(r"c\.excerpt = \[x\[:200\] for x in pick if x\.strip\(\)\]\[:12\]", "c.excerpt = []")]),
    ('timeout_not_enforced', [(r"c\.rc = p\.wait\(timeout=c\.timeout\)", "c.rc = p.wait()")]),
    ('kill_not_the_tree', [(r"\['taskkill', '/T', '/F'", "['taskkill', '/F'")]),
    ('timeout_prefix_ignored', [(r"timeout, rest = int\(m\.group\(1\)\), rest\[m\.end\(\):\]", "timeout, rest = DEFAULT_TIMEOUT, rest[m.end():]")]),
    ('compile_shorthand_wrong', [(r"--changed%s --build off'", "--changed%s'")]),
    ('compile_shorthand_back_to_both', [(r"--changed%s --build off'", "--changed%s --build both'")]),
    ('base_not_passed_to_lints', [(r"' --base ' \+ base if base else ''", "''")]),
    ('rerun_runs_everything', [(r"keep = \[c for c in cmds if last\.get\(c\.raw, \('', ''\)\)\[0\] != 'PASS'\]", "keep = list(cmds)")]),
    ('rerun_without_state_silent', [(r"note = 'prove: --rerun-failed: no earlier run[^\r\n]*", "note = ''")]),
    ('stop_on_fail_ignored', [(r"if stop_on_fail and c\.verdict != 'PASS':", "if False:")]),
    ('skipped_counted_as_passed', [(r"ok = c\.verdict == 'PASS'", "ok = c.verdict in ('PASS', 'SKIP')")]),
    ('command_not_cut', [(r"return s if len\(s\) <= n else s\[:n - 3\] \+ '\.\.\.'", "return s")]),
    ('tool_exit_always_zero', [(r"code = 1 if failed else \(3 if none else 0\)", "code = 0")]),
    ('stderr_not_captured', [(r"stderr=subprocess\.STDOUT", "stderr=subprocess.DEVNULL")]),
    ('comment_lines_run', [(r"if not s or s\.startswith\('#'\):", "if not s:")]),
    ('no_log_path_under_a_fail', [(r"print\('    log: ' \+ c\.log\)", "pass")]),
    ('empty_proof_file_accepted', [(r"holds no command'\)\r?\n        return 2", "holds no command')\n        return 0")]),
    # S110 PROVE review
    ('serial_not_a_barrier', [(r"    for c in cmds:              # file order", "    for c in sorted(cmds, key=lambda c: c.serial):              # file order")]),
    ('nothing_checked_reads_pass', [(r"        c\.verdict = 'NONE'", "        pass")]),
    ('none_exits_zero', [(r"\(3 if none else 0\)", "0")]),
    ('nothing_checked_beats_the_exit_code', [(r"if c\.verdict == 'PASS' and NOTHING_CHECKED\.search", "if NOTHING_CHECKED.search")]),
    ('nothing_checked_anywhere_in_a_line', [(r"re\.compile\(r'\^\\s\*NOTHING CHECKED", "re.compile(r'NOTHING CHECKED")]),
    ('two_commands_on_a_line_run', [(r"        if rest and two_commands\(rest\):", "        if False:")]),
    ('rerun_new_command_not_run', [(r"last\.get\(c\.raw, \('', ''\)\)\[0\] != 'PASS'", "c.raw in last and last[c.raw][0] != 'PASS'")]),
    ('rerun_newest_run_only', [(r"        if os\.path\.normcase\(data\.get\('proof', ''\)\) == os\.path\.normcase\(proof_abs\):",
                                "        if os.path.normcase(data.get('proof', '')) == os.path.normcase(proof_abs) and not last.clear():")]),
    ('rerun_not_said_on_the_total', [(r", rerun, total, wall, code\)", ", '', total, wall, code)")]),
    # S111 PIECELINK review: expect-fail:
    ('expect_fail_prefix_ignored', [(r"expect, rest = True, re\.sub", "expect, rest = False, re.sub")]),
    ('expect_fail_crash_is_caught', [(r"crashed = any\(x\.startswith\('Traceback'\) for x in lines\)", "crashed = False")]),
    ('expect_fail_any_output_is_caught', [(r"caught = not crashed and any\(r\.search\(x\) for x in nonempty for r in FAIL_COUNTS \+ \(CHECK_FAIL,\)\)", "caught = not crashed")]),
]

FIXTURES = {
    'ok.py': "print('step one')\nprint('selftest: OK (3 checks)')\nprint('done')\n",
    'say.py': "import sys\nfor a in sys.argv[2:]:\n    print(a)\nsys.exit(int(sys.argv[1]))\n",
    'boom.py': "print('starting')\nraise RuntimeError('boom')\n",
    'sleeper.py': "import time\ntime.sleep(4.5)\nopen('slept.marker', 'w').write('x')\n",
    'stamp.py': "import sys, time\nt0 = time.time()\ntime.sleep(1.0)\nopen(sys.argv[1] + '.stamp', 'w').write('%r %r' % (t0, time.time()))\n",
    'count.py': "import sys\nopen(sys.argv[1] + '.count', 'a').write('x')\nprint('selftest: OK')\n",
    'flaky.py': ("import os, sys\nn = sys.argv[1] + '.flag'\nif not os.path.exists(n):\n    open(n, 'w').write('x')\n    print('FAIL flaky: first time')\n"
                 "    sys.exit(1)\nprint('selftest: OK (flaky, second time)')\n"),
    'mark.py': "import sys\nopen(sys.argv[1] + '.marker', 'w').write('x')\nprint('selftest: OK')\n",
}


class T:
    def __init__(self, prove):
        self.prove, self.res, self.tmp, self.n = prove, [], tempfile.mkdtemp(prefix='prove_t_'), 0

    def root(self):
        self.n += 1
        r = os.path.join(self.tmp, 'r%d' % self.n)
        os.makedirs(os.path.join(r, 'fx'))
        for name, src in FIXTURES.items():
            with open(os.path.join(r, 'fx', name), 'w', encoding='utf-8', newline='\n') as f:
                f.write(src)
        return r

    def run(self, root, text, *extra, name='proof.txt'):
        pf = os.path.join(root, name)
        with open(pf, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
        r = subprocess.run([sys.executable, '-I', self.prove, pf, '--root', root] + list(extra), capture_output=True, text=True, timeout=300,
                           encoding='utf-8', errors='replace', stdin=subprocess.DEVNULL)
        return r.returncode, r.stdout + r.stderr

    def check(self, name, ok, detail=''):
        self.res.append((name, bool(ok), detail))


def line_of(out, word):
    for ln in out.splitlines():
        if word in ln and re.match(r'(PASS|FAIL|TIMEOUT|SKIP|NONE)', ln):
            return ln
    return ''


def stamp(root, name):
    a, b = open(os.path.join(root, name + '.stamp')).read().split()
    return float(a), float(b)


def overlap(x, y):
    return x[0] < y[1] and y[0] < x[1]


def suite(prove):
    t = T(prove)
    try:
        cases(t)
    finally:
        shutil.rmtree(t.tmp, ignore_errors=True)
    return t.res


def cases(t):
    # --- the mix: pass, exit 0 with failures, crash, timeout (own prefix), a comment; verdict lines, logs, exit code
    r = t.root()
    t0 = time.time()
    rc, out = t.run(r, "# a comment line, not a command\n\ntimeout 2: py -I fx\\sleeper.py\npy -I fx\\ok.py\npy -I fx\\say.py 0 \"failures: 2\" bye\npy -I fx\\boom.py\n")
    wall = time.time() - t0
    t.check('the tool exits 1 when anything failed', rc == 1, out)
    t.check('a pass: PASS, seconds, the command, the generic summary line (selftest: OK beats the last line)',
            re.search(r'^PASS  \d+\.\d s  py -I fx\\ok\.py  \|  selftest: OK \(3 checks\)$', line_of(out, 'ok.py'), re.M) is not None, out)
    t.check('exit 0 but "failures: 2": FAIL, said so', re.search(r'^FAIL \(exit 0, but its summary says failures\)  \d+\.\d s  .*say\.py.*\|  failures: 2$', line_of(out, 'say.py')) is not None, out)
    t.check('a crash: FAIL and the Traceback / error lines under it', re.search(r'^FAIL  ', line_of(out, 'boom.py')) is not None and 'Traceback' in out and 'RuntimeError: boom' in out, out)
    t.check('a fail names the full log, which exists and holds the stderr', 'log: ' in out and any(
        os.path.isfile(x.split('log: ', 1)[1]) and 'RuntimeError' in open(x.split('log: ', 1)[1], encoding='utf-8', errors='replace').read()
        for x in out.splitlines() if 'boom' in x and 'log: ' in x), out)
    t.check('a timeout: TIMEOUT, ended at its own 2 s limit (not the 4.5 s sleep)', re.search(r'^TIMEOUT  [\d.]+ s  .*sleeper', line_of(out, 'sleeper.py')) is not None and wall < 4.4, '%.1f s\n%s' % (wall, out))
    t.check('the last line counts and exits', re.search(r'^prove: 1 passed, 3 failed, total [\d.]+ s \(wall [\d.]+ s\) -> exit 1$', out, re.M) is not None, out)
    t.check('the comment is not a command (4 commands)', 'prove: 4 command(s)' in out, out)
    logs = sorted(os.listdir(os.path.join(r, 'out', 'prove', os.listdir(os.path.join(r, 'out', 'prove'))[0])))
    t.check('logs on disk: one NN_slug.txt per command, in file order, and the result file', len(logs) == 5 and logs[0].startswith('01_') and logs[3].startswith('04_') and 'result.json' in logs, str(logs))
    t.check('verdict lines come in the proof file order', [out.find(w) for w in ('sleeper.py', 'ok.py', 'say.py', 'boom.py')] == sorted(out.find(w) for w in ('sleeper.py', 'ok.py', 'say.py', 'boom.py')), out)
    sleeper_start = t0

    # --- the other generic summary rules and the exit-code rule
    r = t.root()
    rc, out = t.run(r, "py -I fx\\say.py 0 \"total: 4 PASS, 0 FAIL\" bye\npy -I fx\\say.py 0 \"7 checks, 0 failed\" bye\npy -I fx\\say.py 0 \"PASS: 3 unit-builds\" bye\n"
                       "py -I fx\\say.py 0 \"just the last\" \"line here\"\npy -I fx\\say.py 0 \"total: 3 PASS, 1 FAIL\" bye\npy -I fx\\say.py 0 \"2 checks, 1 failed\"\n"
                       "py -I fx\\say.py 3 \"selftest: OK\"\n", '--jobs', '2')
    t.check('summary rule: total: N PASS, N FAIL', '|  total: 4 PASS, 0 FAIL' in out, out)
    t.check('summary rule: N checks, N failed', '|  7 checks, 0 failed' in out, out)
    t.check('summary rule: PASS: N unit-builds', '|  PASS: 3 unit-builds' in out, out)
    t.check('summary rule: else the last non-empty line', '|  line here' in out, out)
    t.check('exit 0 with "3 PASS, 1 FAIL" / "1 failed" in the summary is FAIL, said so', out.count('FAIL (exit 0, but its summary says failures)') == 2, out)
    t.check('the exit code decides: exit 3 with "selftest: OK" is a FAIL', re.search(r'^FAIL  .*\|  selftest: OK$', line_of(out, 'say.py 3'), re.M) is not None, out)
    t.check('4 passed, 3 failed counted', 'prove: 4 passed, 3 failed' in out, out)

    # --- a long command is cut; all pass -> exit 0
    r = t.root()
    rc, out = t.run(r, "py -I fx\\ok.py " + 'a' * 120 + "\n")
    t.check('a long command is cut to about 70 characters with ...', '...' in line_of(out, 'ok.py') and len(line_of(out, 'ok.py').split('  |  ')[0]) < 100, out)
    t.check('every command passed: exit 0 and "-> exit 0"', rc == 0 and out.rstrip().endswith('-> exit 0'), out)

    # --- side by side (default 3): two 1 s stamps overlap
    r = t.root()
    t1 = time.time()
    rc, out = t.run(r, "py -I fx\\stamp.py a\npy -I fx\\stamp.py b\n", '--jobs', '2')
    w = time.time() - t1
    t.check('two commands run side by side with --jobs 2 (their 1 s intervals overlap)', rc == 0 and overlap(stamp(r, 'a'), stamp(r, 'b')), out)

    # --- serial: alone, nothing overlaps it
    r = t.root()
    rc, out = t.run(r, "serial: py -I fx\\stamp.py s1\npy -I fx\\stamp.py n1\npy -I fx\\stamp.py n2\nserial: timeout 60: py -I fx\\stamp.py s2\n", '--jobs', '4')
    st = {k: stamp(r, k) for k in ('s1', 's2', 'n1', 'n2')}
    t.check('two serial: commands overlap neither each other nor any other command (prefixes combine)',
            rc == 0 and not any(overlap(st[s], st[o]) for s in ('s1', 's2') for o in ('s1', 's2', 'n1', 'n2') if s != o), out + str(st))
    t.check('the normal ones still run side by side', overlap(st['n1'], st['n2']), str(st))

    # --- S110 PROVE review: serial: is a BARRIER in file order (a build step, then the test that runs what it built, below it)
    r = t.root()
    rc, out = t.run(r, "py -I fx\\stamp.py p1\nserial: py -I fx\\stamp.py b1\npy -I fx\\stamp.py t1\nserial: py -I fx\\stamp.py b2\npy -I fx\\stamp.py t2\n", '--jobs', '4')
    st = {k: stamp(r, k) for k in ('p1', 'b1', 't1', 'b2', 't2')}
    t.check('serial: is a barrier in file order: what is above it has ended before it starts, what is below starts after it ends (a build, then the test of its exe)',
            rc == 0 and st['p1'][1] < st['b1'][0] and st['b1'][1] < st['t1'][0] and st['t1'][1] < st['b2'][0] and st['b2'][1] < st['t2'][0], out + str(st))

    # --- S110 PROVE review: a command that checked nothing (exit 0, its own line says NOTHING CHECKED: game_syntax --changed on a diff that
    # reaches no game unit) is NONE: never PASS, said under it, counted on the total line, exit 3 (not 0: not every command passed; not 1: none failed)
    r = t.root()
    rc, out = t.run(r, "py -I fx\\ok.py\npy -I fx\\say.py 0 \"note: x\" \"NOTHING CHECKED: 0 game units touched since abc (exit 0, but this is not a PASS)\"\n")
    t.check('exit 0 with a NOTHING CHECKED line: NONE, never PASS, NOT A PASS said under it', line_of(out, 'NOTHING').startswith('NONE  ') and 'NOT A PASS' in out, out)
    t.check('a NONE is counted on the total line and the tool exits 3', re.search(r'^prove: 1 passed, 0 failed, 1 NONE .*-> exit 3$', out, re.M) is not None and rc == 3, out)
    rc, out = t.run(r, "py -I fx\\say.py 1 \"NOTHING CHECKED: x\"\n")
    t.check('a non-zero exit with a NOTHING CHECKED line is a FAIL (the exit code decides first)', line_of(out, 'say.py').startswith('FAIL  ') and rc == 1, out)
    rc, out = t.run(r, "py -I fx\\say.py 0 \"twin x caught: a check named for NOTHING CHECKED\" \"selftest: OK\"\n")
    t.check('only a line that STARTS with NOTHING CHECKED is the verdict (a selftest naming it in a check is a PASS; first real use, S110)',
            line_of(out, 'say.py').startswith('PASS  ') and rc == 0, out)

    # --- S110 PROVE review: `a; b` on one line: PowerShell's exit code is b's alone, a failing `a` would read PASS: the file is refused
    r = t.root()
    rc, out = t.run(r, "py -I fx\\mark.py first\npy -I fx\\say.py 3 a; py -I fx\\say.py 0 b\n")
    t.check('a line with two commands (`a; b`) refuses the whole file: exit 2, the line named, nothing runs',
            rc == 2 and 'line 2' in out and not os.path.exists(os.path.join(r, 'first.marker')), out)
    rc, out = t.run(r, "py -I fx\\say.py 0 \"a; b\" 'c;d' \"selftest: OK\"\n")
    t.check('a ; inside quotes is part of an argument, not a second command', rc == 0, out)

    # --- S110 PROVE review: --rerun-failed is said to be a rerun on its total line; a command new in the file runs; an earlier PASS stands
    r = t.root()
    proof = "py -I fx\\count.py c\npy -I fx\\flaky.py f\n"
    t.run(r, proof)
    time.sleep(1.1)
    rc, out = t.run(r, proof + "py -I fx\\mark.py added\n", '--rerun-failed')
    t.check('--rerun-failed: a command added to the file since the last run has no verdict yet: it runs too',
            os.path.exists(os.path.join(r, 'added.marker')) and 'prove: 2 command(s)' in out, out)
    t.check('--rerun-failed: the total line says RERUN of 2 of 3, that the other PASS is from an earlier run (its stamp), and that the final proof is a full run',
            rc == 0 and re.search(r'^prove: 2 passed, 0 failed, RERUN of 2 of 3 commands: the other 1 PASS from run\(s\) \d{8}-\d{6}\S*, not this one; '
                                  r'the final proof before a hand-back is a full run, total .*-> exit 0$', out, re.M) is not None, out)
    time.sleep(1.1)
    rc, out = t.run(r, proof + "py -I fx\\mark.py added\n", '--rerun-failed')
    t.check('--rerun-failed after a partial rerun: a command whose PASS is from an earlier run is not run again; nothing to run',
            rc == 0 and 'nothing to run' in out and open(os.path.join(r, 'c.count')).read() == 'x', out)

    # --- shorthands resolved without running them (--print), the base line
    r = t.root()
    rc, out = t.run(r, "compile\nlints\nserial: timeout 60: py -I x.py\n", '--print')
    t.check('--print: compile and lints are their command lines, prefixes shown, nothing runs',
            rc == 0 and out.splitlines() == ['py -I tools\\compile\\game_syntax.py --changed --build off', 'py -I tools\\handback\\check.py', 'serial: timeout 60: py -I x.py'] and not os.path.exists(os.path.join(r, 'out')), out)
    rc, out = t.run(r, "base abc1234\nlints\ncompile\n", '--print')
    t.check('--print: a `base REF` line goes to compile --changed and to lints --base',
            out.splitlines() == ['py -I tools\\handback\\check.py --base abc1234', 'py -I tools\\compile\\game_syntax.py --changed abc1234 --build off'], out)

    # --- S111 PIECELINK review: expect-fail: a planted fault must be REPORTED (a failing check), whatever the exit code; a clean run or a crash is a FAIL
    r = t.root()
    rc, out = t.run(r, "expect-fail: py -I fx\\say.py 0 \"FAIL  PutGuest kept a copy\" \"FAIL (3 failed)\"\n"
                       "serial: expect-fail: py -I fx\\say.py 1 \"FAIL  a check\" \"FAIL  5 check(s) failed\"\n"
                       "expect-fail: py -I fx\\ok.py\nexpect-fail: py -I fx\\boom.py\n")
    t.check('expect-fail: exit 0 with "FAIL (3 failed)" (a unit whose plant exit 0 = caught) is a PASS, said so',
            re.search(r'^PASS  .*say\.py 0.*\|  planted fault caught \(exit 0\)', line_of(out, 'say.py 0'), re.M) is not None, out)
    t.check('expect-fail: exit 1 with a check\'s FAIL line is a PASS (prefixes combine with serial:)', line_of(out, 'say.py 1').startswith('PASS  '), out)
    t.check('expect-fail: a clean run (no failing check) is a FAIL: the plant was not caught', line_of(out, 'ok.py').startswith('FAIL (expect-fail: the planted fault was NOT caught'), out)
    t.check('expect-fail: a crash (Traceback) is a FAIL, never a caught plant', line_of(out, 'boom.py').startswith('FAIL (expect-fail: it crashed'), out)
    t.check('expect-fail: counted on the total line', re.search(r'^prove: 2 passed, 2 failed, .*-> exit 1$', out, re.M) is not None and rc == 1, out)
    rc, out = t.run(r, "serial: expect-fail: timeout 60: py -I x.py\n", '--print')
    t.check('--print: the expect-fail: prefix is shown', out.splitlines() == ['serial: timeout 60: expect-fail: py -I x.py'], out)

    # --- the proof file must hold a command
    rc, out = t.run(t.root(), "# nothing\n\n")
    t.check('a proof file without a command: exit 2', rc == 2, out)
    rc, out = t.run(t.root(), "py -I fx\\ok.py\n", '--print', name='x.txt')
    rc2 = subprocess.run([sys.executable, '-I', t.prove, os.path.join(t.tmp, 'no_such_file.txt')], capture_output=True, text=True).returncode
    t.check('a missing proof file: exit 2', rc2 == 2)

    # --- --stop-on-fail
    r = t.root()
    rc, out = t.run(r, "py -I fx\\boom.py\npy -I fx\\mark.py m\n", '--jobs', '1', '--stop-on-fail')
    t.check('--stop-on-fail: the command after the failure is SKIP and never started; a SKIP is not a pass',
            line_of(out, 'mark.py').startswith('SKIP') and not os.path.exists(os.path.join(r, 'm.marker')) and 'prove: 0 passed, 2 failed' in out, out)
    rc, out = t.run(r, "py -I fx\\boom.py\npy -I fx\\mark.py m\n", '--jobs', '1')
    t.check('the default keeps going past a failure', os.path.exists(os.path.join(r, 'm.marker')) and 'prove: 1 passed, 1 failed' in out, out)

    # --- --rerun-failed
    r = t.root()
    rc, out = t.run(r, "py -I fx\\count.py c\npy -I fx\\flaky.py f\n", '--rerun-failed')
    t.check('--rerun-failed with no earlier run: says so, runs everything', 'no earlier run' in out and 'running everything' in out and 'prove: 2 command(s)' in out and rc == 1, out)
    time.sleep(1.1)
    rc, out = t.run(r, "py -I fx\\count.py c\npy -I fx\\flaky.py f\n", '--rerun-failed')
    t.check('--rerun-failed: only the command that failed in the newest run runs again, and now passes',
            rc == 0 and 'prove: 1 command(s)' in out and 'flaky' in line_of(out, 'flaky.py') and open(os.path.join(r, 'c.count')).read() == 'x' and 'rerun' in out, out)
    time.sleep(1.1)
    rc, out = t.run(r, "py -I fx\\count.py c\npy -I fx\\flaky.py f\n", '--rerun-failed')
    t.check('--rerun-failed after an all-pass run: nothing to run, exit 0', rc == 0 and 'nothing to run' in out and open(os.path.join(r, 'c.count')).read() == 'x', out)

    # --- the timed-out command's whole process tree is killed (pwsh -> py -> python): the sleeper never writes its marker
    left = sleeper_start + 6.2 - time.time()
    if left > 0:
        time.sleep(left)
    t.check('a timeout kills the whole tree (the sleeper never reached its marker)', not os.path.exists(os.path.join(t.tmp, 'r1', 'slept.marker')))


def twin(src, plants):
    for pat, rep in plants:
        src, n = re.subn(pat, lambda m: rep, src, count=1)
        if n != 1:
            return None
    return src


def suite_in_child(path):
    r = subprocess.run([sys.executable, '-I', os.path.abspath(__file__), '--suite', path], capture_output=True, text=True, timeout=900, encoding='utf-8', errors='replace')
    res = []
    for x in r.stdout.splitlines():
        try:
            res.append(tuple(json.loads(x)))
        except ValueError:
            pass
    return res if r.returncode == 0 and res else None


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--suite':
        for row in suite(sys.argv[2]):
            print(json.dumps(list(row)))
        return 0
    src = open(PROVE, encoding='utf-8', newline='').read()
    problems = []
    res = suite(PROVE)
    for name, ok, detail in res:
        print('%s  %s' % ('PASS' if ok else 'FAIL', name))
        if not ok:
            print('      ' + detail.strip().replace('\n', '\n      ')[:700])
            problems.append('shipped tool fails: ' + name)
    if '--shipped' in sys.argv:         # while building: the shipped tool's checks only, no twins
        for p in problems:
            print('PROBLEM: ' + p)
        print('prove_selftest --shipped: %s (%d checks)' % ('OK' if not problems else '%d problem(s)' % len(problems), len(res)))
        return 1 if problems else 0
    tmp = tempfile.mkdtemp(prefix='prove_twin_')
    caught = 0
    try:
        jobs = []
        for name, plants in PLANTS:
            new = twin(src, plants)
            if new is None:
                problems.append('plant %s: a pattern did not match exactly once (prove.py changed under it)' % name)
                continue
            p = os.path.join(tmp, name + '.py')
            with open(p, 'w', encoding='utf-8', newline='') as f:
                f.write(new)
            jobs.append((name, p))
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as ex:
            results = list(ex.map(lambda j: suite_in_child(j[1]), jobs))
        for (name, p), rs in zip(jobs, results):
            if rs is None:
                problems.append('plant %s: the selftest child crashed' % name)
                print('twin %-32s CHILD CRASHED' % name)
                continue
            failed = [(n, d) for n, ok, d in rs if not ok]
            if failed:
                caught += 1
            else:
                problems.append('plant %s: NOT CAUGHT (every check still passes)' % name)
            print('twin %-32s %s' % (name, ('caught by %d check(s): %s' % (len(failed), failed[0][0])) if failed else 'NOT CAUGHT'))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    for p in problems:
        print('PROBLEM: ' + p)
    print('prove_selftest: %s (%d checks, %d/%d twins caught)' % ('OK' if not problems else '%d problem(s)' % len(problems), len(res), caught, len(PLANTS)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
