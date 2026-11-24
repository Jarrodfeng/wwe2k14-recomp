r"""S110 MEDIT review: selftest of tools\harness\tool_calls.py's medit count on a made-up transcript, and of PLANTED TWINS of it.

    py -I tools\harness\tool_calls_selftest.py

The adoption of medit is read from the runs themselves: a Bash / PowerShell call that runs medit.py (also by a relative path) is a run; the
blocks are the ones the run's own result says it APPLIED ("medit: N block(s) in M file(s) applied"); a "NOTHING written" result is a failed
run, a "--check" result a check. An editfile written under any name counts the same; a script that prints medit's lines (medit_known_case.py)
is not a run. Exit 0 = every check passes and every twin fails at least one. Writes only a temp folder; 0 game runs.
"""
import importlib.util
import json
import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, 'tool_calls.py')
sys.path.insert(0, HERE)        # a twin in a temp folder still imports lane_time from here

PLANTS = [
    ('blocks_not_from_the_result', r"medit\['blocks'\] \+= ", "medit['blocks'] += 0 * "),
    ('failed_runs_not_counted', r"medit\['failed'\] \+= ", "medit['failed'] += 0 * "),
    ('results_of_any_call_counted', r" and b\.get\('tool_use_id'\) in medit_ids", ""),
    ('relative_run_missed', r"MEDIT_RUN = re\.compile\(", "MEDIT_RUN = re.compile(r'tools[\\\\/]edit[\\\\/]medit\\.py') or re.compile("),
    # S110 PROVE: the prove.py column
    ('prove_commands_not_from_the_result', r"prove\['commands'\] \+= ", "prove['commands'] += 0 * "),
    ('prove_failed_not_counted', r"prove\['failed'\] \+= ", "prove['failed'] += 0 * "),
    ('prove_results_of_any_call_counted', r"pid = tid if tid in prove_ids else prove_reads\.get\(tid\)", "pid = tid"),
    ('prove_selftest_counted_as_a_run', r"\[\\\\/\]\)\?prove\\\.py\\b", "[\\\\/])?prove"),
    # S110 PROVE review: a background run's verdict, read later; a read twice; a NONE; a git add that names the file
    ('prove_background_read_missed', r"prove_reads\[b\.get\('id'\)\] = pid", "pass"),
    ('prove_read_twice_counted_twice', r"prove_last\[pid\] = ", "prove_last[(pid, len(prove_last))] = "),
    ('prove_none_not_counted', r"prove\['failed'\] \+= bad \+ none", "prove['failed'] += bad"),
    ('prove_git_add_counted_as_a_run', r"PROVE_RUN = re\.compile\(", "PROVE_RUN = re.compile(r'(?:^|[\\s\"\\'\\\\/])prove\\.py\\b') or re.compile("),
    ('prove_redirect_file_missed', r"prove_files\[os\.path\.normcase\(to\)\] = b\.get\('id'\)", "pass"),
]


def use(i, name, inp, mid):
    return {'message': {'role': 'assistant', 'id': mid, 'content': [{'type': 'tool_use', 'id': i, 'name': name, 'input': inp}]}}


def result(i, text, as_list=False):
    c = [{'type': 'text', 'text': text}] if as_list else text
    return {'message': {'role': 'user', 'content': [{'type': 'tool_result', 'tool_use_id': i, 'content': c}]}}


def transcript():
    P = '%%medit '
    two = P + 'REPLACE a.py\n' + P + 'FIND\nx\n' + P + 'WITH\ny\n' + P + 'END\n'
    return [
        {'message': {'role': 'user', 'content': 'Lane MTEST: build it'}},
        use('w1', 'Write', {'file_path': 'C:\\s\\fix.txt', 'content': two + two}, 'm1'),           # an editfile NOT named *.medit
        use('b1', 'Bash', {'command': 'cd /c/wt && py -I tools\\edit\\medit.py C:\\s\\fix.txt'}, 'm2'),
        result('b1', 'tools\\a.py: 2 block(s) applied, +1 -1 lines\r\nmedit: 2 block(s) in 1 file(s) applied\r\n'),
        use('w2', 'Write', {'file_path': 'C:\\s\\y.medit', 'content': two * 3}, 'm3'),
        use('b2', 'PowerShell', {'command': 'py -I medit.py C:\\s\\y.medit'}, 'm4'),                # run from tools\edit by a relative path
        result('b2', 'medit: NOTHING written: 1 block(s) failed\n  block 3 (REPLACE a.py, editfile line 9): 0 matches\n', as_list=True),
        use('e1', 'Edit', {'file_path': 'C:\\s\\y.medit', 'old_string': 'x', 'new_string': 'z'}, 'm5'),
        use('b3', 'Bash', {'command': 'py -I tools/edit/medit.py C:\\s\\y.medit'}, 'm6'),
        result('b3', 'a.py: 3 block(s) applied, +3 -3 lines\nmedit: 3 block(s) in 2 file(s) applied\n'),
        use('b4', 'Bash', {'command': 'py -I tools\\edit\\medit.py C:\\s\\y.medit --check'}, 'm7'),
        result('b4', 'medit --check: 3 block(s) in 2 file(s) would apply (nothing written)\n'),
        use('b5', 'Bash', {'command': 'py -I tools\\edit\\medit_known_case.py'}, 'm8'),            # prints medit's lines: not a run
        result('b5', 'medit: 25 block(s) in 6 file(s) applied\nknown case: OK\n'),
        use('p1', 'Bash', {'command': 'py -I tools\\handback\\prove.py C:\\s\\proof.txt'}, 'm9'),     # S110 PROVE: 5 commands, 1 failed
        result('p1', 'PASS  1.0 s  x\nprove: 4 passed, 1 failed, total 9.0 s (wall 4.0 s) -> exit 1\n'),
        use('p2', 'PowerShell', {'command': 'py -I prove.py --rerun-failed C:\\s\\proof.txt'}, 'm10'),   # by a relative path; the rerun of the one
        result('p2', 'prove: 1 passed, 0 failed, total 2.0 s (wall 2.0 s) -> exit 0\n', as_list=True),
        use('p3', 'Bash', {'command': 'py -I tools\\handback\\prove_selftest.py'}, 'm11'),           # the selftest of the tool: not a run
        result('p3', 'prove: 9 passed, 0 failed\nprove_selftest: OK\n'),
        # S110 PROVE review: the prescribed way is a BACKGROUND run: its own result only names the output file; the verdict is read from that
        # file later (a Read, or a shell command naming it), maybe twice: one run, counted once
        use('p4', 'PowerShell', {'command': 'py -I tools\\handback\\prove.py C:\\s\\proof.txt', 'run_in_background': True}, 'm12'),
        result('p4', 'Command running in background with ID: bx1. Output is being written to: C:\\T\\tasks\\bx1.output. You will be notified when it completes.'),
        use('r1', 'Read', {'file_path': 'C:\\T\\tasks\\bx1.output'}, 'm13'),
        result('r1', '     1\tprove: 3 command(s), up to 3 at once, logs out\\prove\\x\n     5\tprove: 2 passed, 1 failed, total 9.0 s (wall 4.0 s) -> exit 1\n'),
        use('r2', 'PowerShell', {'command': 'Get-Content C:\\T\\tasks\\bx1.output -Tail 1'}, 'm14'),
        result('r2', 'prove: 2 passed, 1 failed, total 9.0 s (wall 4.0 s) -> exit 1\n'),
        use('g1', 'Bash', {'command': 'git add tools/handback/prove_selftest.py tools/handback/prove.py'}, 'm15'),   # names the file: not a run
        result('g1', ''),
        use('p5', 'Bash', {'command': 'py -I tools\\handback\\prove.py C:\\s\\p2.txt'}, 'm16'),                # a command that checked nothing
        result('p5', 'NONE  2.0 s  compile  |  NOTHING CHECKED\nprove: 1 passed, 0 failed, 1 NONE (checked nothing: not a pass), total 3.0 s (wall 2.0 s) -> exit 3\n'),
        # the builder's own way (S110 PROVE): a background run whose stdout goes to a file of its own, read later by that file's path
        use('p6', 'Bash', {'command': 'cd /c/wt && timeout 2700 py -I tools/handback/prove.py "C:/s/p.txt" > out/known_case.txt 2>&1', 'run_in_background': True}, 'm17'),
        result('p6', 'Command running in background with ID: bx2. Output is being written to: C:\\T\\tasks\\bx2.output. You will be notified when it completes.'),
        use('r3', 'Read', {'file_path': 'C:\\wt\\out\\known_case.txt'}, 'm18'),
        result('r3', '    40\tprove: 6 passed, 0 failed, total 900.0 s (wall 490.0 s) -> exit 0\n'),
    ]


def load(path, tag):
    spec = importlib.util.spec_from_file_location(tag, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def suite(path, tmp, tag):
    res = []
    try:
        mod = load(path, tag)
        tp = os.path.join(tmp, 'agent-test.jsonl')
        with open(tp, 'w', encoding='utf-8') as f:
            for r in transcript():
                f.write(json.dumps(r) + '\n')
        a = mod.audit(tp)
    except Exception as e:      # a crash is a FAIL line
        return [('the audit runs', False, 'Traceback: %r' % e)]
    kinds, edits = a[1], a[4]
    m = a[5] if isinstance(a[5], dict) else {'blocks': a[5]}
    res.append(('runs: every medit.py call, also by a relative path (4)', kinds.get('medit', 0) == 4 and m.get('runs') == 4, '%r %r' % (dict(kinds), m)))
    res.append(('blocks: what the runs printed as APPLIED (2 + 3), an editfile of any name, not a script that prints medit lines', m.get('blocks') == 5, repr(m)))
    res.append(('failed runs: the "NOTHING written" results (1)', m.get('failed') == 1, repr(m)))
    res.append(('checks: the --check results (1)', m.get('check') == 1, repr(m)))
    res.append(('the Edit of the editfile is still an Edit', len(edits) == 1, repr(edits)))
    pv = a[6] if len(a) > 6 and isinstance(a[6], dict) else {}
    res.append(('prove runs: every prove.py call (5), also by a relative path and in the background, not prove_selftest.py, not a git add of it',
                kinds.get('prove', 0) == 5 and pv.get('runs') == 5, '%r %r' % (dict(kinds), pv)))
    res.append(('prove commands: what the runs printed on their last line (5 + 1 + 3 + 2 + 6), a background run\'s from the read of its output file '
                '(or of the file its stdout was sent to), once', pv.get('commands') == 17, repr(pv)))
    res.append(('prove failed: the commands those lines say did not PASS, a NONE included (1 + 0 + 1 + 1)', pv.get('failed') == 3, repr(pv)))
    return res


def main():
    src = open(TOOL, encoding='utf-8', newline='').read()
    tmp = tempfile.mkdtemp(prefix='toolcalls_t_')
    problems = []
    try:
        res = suite(TOOL, tmp, 'tool_calls_shipped')
        for name, ok, detail in res:
            print('%s  %s%s' % ('PASS' if ok else 'FAIL', name, '' if ok else '\n      ' + detail))
            if not ok:
                problems.append('shipped tool fails: ' + name)
        for name, pat, rep in PLANTS:
            new, n = re.subn(pat, lambda _m: rep, src, count=1)
            if n != 1:
                problems.append('plant %s: the pattern did not match (tool_calls.py changed under it)' % name)
                continue
            p = os.path.join(tmp, name + '.py')
            with open(p, 'w', encoding='utf-8', newline='') as f:
                f.write(new)
            failed = [x for x, ok, _ in suite(p, tmp, 'twin_' + name) if not ok]
            print('twin %-30s %s' % (name, 'caught: ' + failed[0] if failed else 'NOT CAUGHT'))
            if not failed:
                problems.append('plant %s: NOT CAUGHT' % name)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    for p in problems:
        print('PROBLEM: ' + p)
    print('tool_calls_selftest: %s (%d checks, %d twins)' % ('OK' if not problems else '%d problem(s)' % len(problems), len(res), len(PLANTS)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
