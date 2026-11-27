"""Selftest for `q ours text` (S111 QTEXT): a literal with any characters (system[9]), --lit, a regex alternation (-E / --re), --in on a folder, several --in.
0 game runs. Planted twins on a throwaway git repo. Exit 1 = a check failed."""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
Q = os.path.join(HERE, 'q.py')
fails = []


def check(what, ok, detail=''):
    print('%s %s%s' % ('ok  ' if ok else 'FAIL', what, '' if ok else '  ' + str(detail)[:300]))
    if not ok:
        fails.append(what)


def q(*args):
    r = subprocess.run([sys.executable, '-I', Q, 'ours', 'text'] + list(args), capture_output=True, text=True, encoding='utf-8', errors='replace')
    return r.returncode, r.stdout, r.stderr


def write(root, rel, text):
    p = os.path.join(root, *rel.split('/'))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'w', encoding='utf-8', newline='') as f:
        f.write(text)


def git(root, *a):
    r = subprocess.run(['git', '-C', root, '-c', 'user.name=t', '-c', 'user.email=t@t', '-c', 'commit.gpgsign=false'] + list(a), capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError('git %s: %s' % (a, r.stderr))


def main():
    with tempfile.TemporaryDirectory(prefix='qourstext_', ignore_cleanup_errors=True) as tmp:
        write(tmp, 'src/rs/a.cpp', 'dyn.system[9] = 7.f;\ndyn.system9 = 1;\nrdr::DrawState st{};\nb.Dynamic(dyn);\nint abc = 1;\n')
        write(tmp, 'src/rs/b.cpp', 'd.system[9] = 6.f;\nDrawState other;\n')
        write(tmp, 'src/other/c.cpp', 'x.system[9] = 0;\nint a_c = 2;\nint a.c = 3;\n')
        write(tmp, 'tools/t.py', 'y = system[9]\nz = zz[1]\n')
        for i in range(6):      # six files so --limit 1 cuts the list
            write(tmp, 'src/many/m%d.cpp' % i, 'int v = zz[1];\n')
        git(tmp, 'init', '-q')
        git(tmp, 'add', '-A')
        git(tmp, 'commit', '-q', '-m', 'x')
        R = ['--root', tmp]
        rc, out, err = q('system[9]', '--in', 'src/rs', *R)
        check('a literal with brackets over a folder: 2 lines (a.cpp, b.cpp), not system9', rc == 0 and ': 2 lines in 2 files' in out and 'system9' not in out, out)
        rc, out, err = q('--lit', 'system[9]', '--in', 'src/rs', *R)
        check('--lit is accepted and gives the same answer', rc == 0 and ': 2 lines in 2 files' in out, (rc, out, err))
        rc, out, err = q('--lit', 'a.c', '--in', 'src/other', *R)
        check('--lit: the dot is a dot (a.c, not a_c)', rc == 0 and ': 1 lines' in out and 'a_c' not in out, out)
        rc, out, err = q('-E', 'a.c', '--in', 'src/other', *R)
        check('twin: -E the same text is a regex (a_c and a.c both)', rc == 0 and ': 2 lines' in out, out)
        rc, out, err = q('--lit', '-E', 'a.c', *R)
        check('--lit with -E is refused', rc == 2, (rc, out, err))
        rc, out, err = q('-E', r'Dynamic\(|DrawState \w+\{\}', '--in', 'src/rs', *R)
        check('regex alternation over a folder (space, \\w, braces): st{} and Dynamic( lines', rc == 0 and ': 2 lines in 1 files' in out and 'a.cpp:3' in out and 'a.cpp:4' in out, out)
        rc, out, err = q('--re', r'Dynamic\(|DrawState \w+\{\}', '--in', 'src/rs', *R)
        check('--re is -E', rc == 0 and ': 2 lines in 1 files' in out, out)
        rc, out, err = q('system[9]', '--in', 'src/rs', '--in', 'tools/t.py', *R)
        check('several --in: a folder and a file searched together (3 lines)', rc == 0 and ': 3 lines in 3 files' in out and 'only that path' in out, (rc, out, err))
        rc, out, err = q('system[9]', '--in', 'src/rs', '--in', 'no/such', *R)
        check('several --in: a path that does not exist is an error', rc == 2 and 'no/such' in err, (rc, out, err))
        rc, out, err = q('zz[1]', '--limit', '1', '--in', 'src/many', '--in', 'tools/t.py', *R)
        check('a cut answer names the total and a call that keeps both --in', rc == 0 and ': 7 lines in 7 files' in out and '--in tools/t.py' in out and '--in src/many' in out and '--all' in out, out)
        rc, out, err = q('system[9]', *R)
        check('no --in: all three code folders (4 lines)', rc == 0 and ': 4 lines' in out, out)
    print('qourstext_selftest: %d failed' % len(fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
