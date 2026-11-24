"""qm_selftest (S106 lane QM): proves the five q-miss closes of tools/q/qm.py on a fixture repo in a TEMP folder (no clang, no game, 0 runs):
  1 closed pipe   PLANTED TWIN FIRST: a stub that catches only BrokenPipeError (the old q behaviour) must be seen FAILING on a pipe closed after one line
                  (else the harness cannot see the crash); then the real `q.py show ...` and `q.py ours lit ...` into the same pipe: exit 0, no traceback.
  2 outline       a .py file: decorated function, class + methods, a def inside an `if __name__` block; a `def fake()` inside a docstring / comment is NOT listed
                  (planted); a .ps1 function; line spans exact.
  3 runs          a script naming out/tools/x_unit.exe, a lookalike name (`my_x_unit.exe`, planted) NOT matched; a script that invokes build_x.ps1 (kind invoke)
                  vs one that only mentions it in a comment.
  4 env           set / read / comment classification across py / ps1 / cpp / bat; FOO_BAR (planted lookalike) is not FOO.
  5 includers     a `.inc` file included by a .cpp in a folder named spv: single file, a quoted glob and two files given as separate arguments.
Run:  py -I tools/q/qm_selftest.py
"""
# entry: closed_pipe
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
Q = os.path.join(HERE, 'q.py')
ENV = {k: v for k, v in os.environ.items() if k not in ('WWE_Q_FRESH', 'WWE_Q_FRESH_BUDGET', 'WWE_Q_FRESH_PLANT_MS')}
ENV.update(WWE_Q_OURS_NO_AUTOUPDATE='1', PYTHONIOENCODING='utf-8')


class Fail(Exception):
    pass


def check(cond, what, out=''):
    if not cond:
        raise Fail(f'{what}\n--- output ---\n{str(out)[-1500:]}')
    print(f'PASS  {what}')


def write(root, rel, text):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def q(root, *a):
    r = subprocess.run([sys.executable, '-I', Q, 'ours', *a, '--root', root], capture_output=True, text=True, env=ENV, timeout=120)
    return r.returncode, r.stdout, r.stderr


def closed_pipe_run(argv, cwd):
    """start argv, read ONE line of stdout, close the pipe (the `| head -1`), -> (returncode, stderr)"""
    p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=ENV, cwd=cwd)
    p.stdout.readline()
    p.stdout.close()
    err = p.stderr.read()
    return p.wait(timeout=120), err


STUB_OLD = '''import sys
def main():
    for i in range(60000):
        print("line %d with some padding to fill the pipe buffer quickly ......................" % i)
try:
    main()
except BrokenPipeError:
    sys.exit(0)
'''


def main():
    with tempfile.TemporaryDirectory(prefix='qm_selftest_') as root:
        # ---- 1 closed pipe
        import errno
        sys.path.insert(0, HERE)
        import qm
        check(qm.closed_pipe(OSError(errno.EINVAL, 'x')) is True and qm.closed_pipe(OSError(errno.ENOENT, 'x')) is False,
              'closed_pipe: EINVAL (Windows closed pipe) is true, ENOENT is false')
        write(root, 'old_stub.py', STUB_OLD)
        rc, err = closed_pipe_run([sys.executable, '-I', os.path.join(root, 'old_stub.py')], root)
        check(rc != 0 or 'Traceback' in err, 'PLANTED twin: the old catch-BrokenPipeError-only stub is seen FAILING on a closed pipe (harness works)', f'rc={rc}\n{err}')
        rc, err = closed_pipe_run([sys.executable, '-I', Q, 'show', 'tools/q/qx6.py:1-1200'], os.path.dirname(os.path.dirname(HERE)))
        check(rc == 0 and 'Traceback' not in err and 'Errno' not in err, 'q show ... | head -1: exit 0, no traceback', f'rc={rc}\n{err}')
        rc, err = closed_pipe_run([sys.executable, '-I', Q, 'ours', 'lit', 'import', '--tools', '--all', '--limit', '4000'], os.path.dirname(os.path.dirname(HERE)))
        check(rc == 0 and 'Traceback' not in err and 'Errno' not in err, 'q ours lit ... | head -1 (the `ours` path): exit 0, no traceback', f'rc={rc}\n{err}')
        rc, err = closed_pipe_run([sys.executable, '-I', Q, 'ours', 'env', 'PATH', '--all', '--limit', '4000'], os.path.dirname(os.path.dirname(HERE)))
        check(rc == 0 and 'Traceback' not in err, 'q ours env ... | head -1: exit 0, no traceback', f'rc={rc}\n{err}')

        # ---- 2 outline
        write(root, 'tools/a.py', '''"""module doc
def fake_in_docstring():
    pass
"""
import os


# def fake_in_comment():
def top(x):
    def inner():
        pass
    return x


@deco
def decorated():
    return 1


class Box:
    def method(self):
        return 2

    @property
    def prop(self):
        return 3


if __name__ == '__main__':
    def under_if():
        pass
''')
        write(root, 'tools/b.ps1', 'param()\nfunction Get-Thing {\n    param($a)\n    if ($a) { 1 }\n}\n\nfunction Other-Thing { 2 }\n')
        rc, out, err = q(root, 'outline', 'tools/a.py')
        check(rc == 0 and 'top' in out and 'Box.method' in out and 'Box.prop' in out and 'decorated' in out and 'under_if' in out, 'outline a.py lists functions, class, methods, a def under `if __name__`', out + err)
        check('fake_in_docstring' not in out and 'fake_in_comment' not in out and 'inner' not in out, 'outline a.py does NOT list a def in a docstring / comment / nested (planted)', out)
        tl = [ln for ln in out.splitlines() if ln.rstrip().endswith(' top')][0]
        check(tl.split()[0].startswith('9-12'), 'outline spans are exact (top = 9-12)', tl)
        line = [ln for ln in out.splitlines() if ln.rstrip().endswith('decorated')][0]
        check(line.split()[0].startswith('15-17'), 'a decorated function starts at its decorator line (15-17)', line)
        rc, out, err = q(root, 'outline', 'tools/b.ps1')
        check(rc == 0 and '2-5' in out.replace(' ', '') and 'Get-Thing' in out and 'Other-Thing' in out, 'outline b.ps1 lists the ps1 functions with spans', out + err)

        # ---- 3 runs
        write(root, 'tools/mk_unit.py', 'EXE = "out/tools/x_unit.exe"\n')
        write(root, 'tools/lookalike.py', 'EXE = "out/tools/my_x_unit.exe"\n')
        write(root, 'tools/scene/build_x.ps1', '# builds out\\tools\\x_unit.exe\n$exe = "x_unit.exe"\n')
        write(root, 'tools/run_it.ps1', '& pwsh -NoProfile -File tools/scene/build_x.ps1\n')
        write(root, 'tools/note.py', '# see tools/scene/build_x.ps1 for how it builds\n')
        rc, out, err = q(root, 'runs', 'out/tools/x_unit.exe')
        check(rc == 0 and 'tools/mk_unit.py:1' in out and 'not a file in this checkout' in out, 'runs EXE (not a file here): the script that names it', out + err)
        check('lookalike' not in out and 'my_x_unit' not in out, 'runs EXE does NOT list a lookalike name (my_x_unit.exe, planted)', out)
        check('tools/scene/build_x.ps1:2' in out or 'tools/scene/build_x.ps1:1' in out, 'runs EXE: the script that builds it is listed', out)
        rc, out, err = q(root, 'runs', 'tools/scene/build_x.ps1')
        check('tools/run_it.ps1:1  [invoke]' in out and 'tools/note.py:1  [mentions]' in out, 'runs SCRIPT: an invoking script line is `invoke`, a comment is `mentions`', out)

        # ---- 4 env
        write(root, 'tools/e.py', 'import os\nx = os.environ.get("FOO")\nos.environ["FOO"] = "1"\ny = os.environ["FOO_BAR"]\n# FOO is documented here\n')
        write(root, 'tools/e.ps1', '$env:FOO = "2"\n$v = $env:FOO\n$env:FOO_BAR = "3"\n')
        write(root, 'src/e.cpp', 'void f(){ auto v = std::getenv("FOO"); SetEnvironmentVariableW(L"FOO", L"1"); }\n')
        write(root, 'tools/e.bat', 'set FOO=1\necho %FOO%\n')
        rc, out, err = q(root, 'env', 'FOO', '--json')
        rows = json.loads(out)['rows']
        by = {(r['file'], r['line']): r['kind'] for r in rows}
        check(by.get(('tools/e.py', 2)) == 'read' and by.get(('tools/e.py', 3)) == 'set', 'env: py environ.get = read, environ[...] = is set', out)
        check(by.get(('tools/e.ps1', 1)) == 'set' and by.get(('tools/e.ps1', 2)) == 'read', 'env: ps1 $env:X = set, plain $env:X read', out)
        check(by.get(('src/e.cpp', 1)) == 'set', 'env: cpp line with getenv + SetEnvironmentVariableW is set (set wins)', out)
        check(by.get(('tools/e.bat', 1)) == 'set' and by.get(('tools/e.bat', 2)) == 'read', 'env: bat set / %X%', out)
        check(by.get(('tools/e.py', 5)) == 'ref', 'env: a comment line is a ref, never a read / set', out)
        check(('tools/e.py', 4) not in by and ('tools/e.ps1', 3) not in by, 'env: FOO_BAR (planted lookalike) is not FOO', out)

        # ---- 5 includers on .inc
        write(root, 'src/spv/d_data.inc', '0x1,0x2,\n')
        write(root, 'src/spv/e_data.inc', '0x3,\n')
        write(root, 'src/spv/d.cpp', 'static const unsigned a[] = {\n#include "d_data.inc"\n};\nstatic const unsigned b[] = {\n#include "e_data.inc"\n};\n')
        rc, out, err = q(root, 'includers', 'src/spv/d_data.inc')
        check(rc == 0 and 'src/spv/d.cpp:2' in out and 'by 1 files' in out, 'includers FILE.inc: the .cpp in the spv folder that includes it', out + err)
        rc, out, err = q(root, 'includers', 'src/spv/*_data.inc')
        check(rc == 0 and 'src/spv/d.cpp:2' in out and 'src/spv/d.cpp:5' in out and 'by 2 files' in out, 'includers GLOB: both .inc includes in one answer', out + err)
        rc, out, err = q(root, 'includers', 'src/spv/d_data.inc', 'src/spv/e_data.inc')
        check(rc == 0 and 'by 2 files' in out, 'includers A B (shell-expanded glob): both', out + err)
    print('qm_selftest: ALL PASS')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Fail as e:
        print('FAIL  ' + str(e))
        sys.exit(1)
