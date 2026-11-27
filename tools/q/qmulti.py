"""q multi (S108 lane FD): several q questions in ONE call (one process, one tool call; agents issued ~1 call per reply).

    py tools/q/q.py multi "ours callers F" "show tools/q/q.py" "decomp sub_82CB1F50 --fields" [--json]

Each argument is one q command line, exactly as it would follow `q` (a leading `q` or `py tools\\q\\q.py` is dropped; quotes group words,
backslashes are kept: Windows paths survive). The questions run in order; each answer is printed whole (never trimmed harder than alone)
between `== [k/n] q <question>` and `-- [k/n] exit E  T s`; a failing question does not stop the others. The last line names the failures.
Exit: 0 when every question exited 0, else the highest exit code. --json: a list of {question, argv, exit, seconds, output (the parsed --json
answer when the question asked for --json and it parses), stderr}.
Refused inside multi: multi itself and the `update` verbs (ours / sdk / asset update: run them alone). The q timeout (WWE_Q_TIMEOUT) is
multiplied by min(number of questions, 5) for the whole call.
"""
import json
import shlex
import sys
import tempfile
import time
import traceback

REFUSE = (('multi',), ('ours', 'update'), ('sdk', 'update'), ('asset', 'update'))


def split_question(s):
    lx = shlex.shlex(s, posix=True)
    lx.whitespace_split = True
    lx.escape = ''          # a backslash is a path separator here, not an escape
    av = list(lx)
    j = 1
    while j < len(av) and av[j].startswith('-'):       # review: `py -I tools\q\q.py ...` (the interpreter's own options before the script)
        j += 1
    if av and av[0].lower() in ('py', 'python', 'python3') and j < len(av) and av[j].replace('\\', '/').endswith('q.py'):
        av = av[j + 1:]
    elif av and (av[0] == 'q' or av[0].replace('\\', '/').endswith('/q.py') or av[0] == 'q.py'):
        av = av[1:]
    if av and av[0] == '-I':
        av = av[1:]
    return av


def run_one(qmain, av):
    """-> exit code; SystemExit / exceptions of one question never end the batch"""
    try:
        rc = qmain(av)
        return 0 if rc is None else int(rc)
    except SystemExit as e:
        if e.code is None:
            return 0
        if isinstance(e.code, int):
            return e.code
        print(e.code, file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        raise
    except BaseException:      # noqa: BLE001 - a bug in one question is that question's exit 3
        traceback.print_exc()
        return 3


def main(argv, qmain):
    as_json = '--json' in argv
    qs = [a for a in argv if a != '--json']
    if not qs or qs[0] in ('-h', '--help'):
        print(__doc__)
        return 0 if qs else 2
    n = len(qs)
    t_all = time.time()
    res = []
    for k, s in enumerate(qs, 1):
        refuse = None
        try:
            av = split_question(s)
        except ValueError as e:      # review: an unclosed quote ended the whole batch with a traceback
            av, refuse = [], f'the question does not split into words ({e}): check its quotes'
        head = f'[{k}/{n}] q {s}'
        if refuse is None and not av:
            refuse = 'an empty question'
        elif refuse is None and any(tuple(av[:len(r)]) == r for r in REFUSE):
            refuse = f'`{" ".join(av[:2])}` is not run inside multi (run it alone)'
        t0 = time.time()
        if as_json:
            out, err = tempfile.TemporaryFile('w+', encoding='utf-8', errors='replace'), tempfile.TemporaryFile('w+', encoding='utf-8', errors='replace')
            so, se = sys.stdout, sys.stderr
            sys.stdout, sys.stderr = out, err
            try:
                if refuse:
                    print(f'q multi: {refuse}', file=sys.stderr)
                    rc = 2
                else:
                    rc = run_one(qmain, av)
            finally:
                sys.stdout.flush()
                sys.stderr.flush()
                sys.stdout, sys.stderr = so, se
            out.seek(0)
            err.seek(0)
            o, e = out.read(), err.read()
            out.close()
            err.close()
            parsed = None
            if '--json' in av and o.strip().startswith(('{', '[')):
                try:
                    parsed = json.loads(o)
                except ValueError:
                    parsed = None
            res.append({'question': s, 'argv': av, 'exit': rc, 'seconds': round(time.time() - t0, 2), 'output': parsed if parsed is not None else o, 'stderr': e})
            continue
        print(f'== {head}', flush=True)
        se = sys.stderr
        sys.stderr = sys.stdout       # one stream: a question's notes and errors stay inside its own == / -- block (a shell merges two pipes in any order)
        try:
            if refuse:
                print(f'q multi: {refuse}', file=sys.stderr, flush=True)
                rc = 2
            else:
                rc = run_one(qmain, av)
        finally:
            sys.stderr = se
        try:
            sys.stdout.flush()
        except (OSError, ValueError):
            pass
        print(f'-- [{k}/{n}] exit {rc}  {time.time() - t0:.2f} s', flush=True)
        res.append({'question': s, 'exit': rc})
    failed = [(i, r['exit']) for i, r in enumerate(res, 1) if r['exit'] != 0]
    worst = max([r['exit'] for r in res] + [0])
    if as_json:
        print(json.dumps(res, indent=1, default=str))
        return worst
    print(f'multi: {n} questions in {time.time() - t_all:.2f} s: {n - len(failed)} exit 0'
          + (f'; failed: {", ".join(f"#{i} (exit {e})" for i, e in failed)}' if failed else ''))
    return worst
