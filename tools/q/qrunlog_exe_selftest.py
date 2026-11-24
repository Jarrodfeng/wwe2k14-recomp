"""S106 G2L selftest: q runlog finds logs of out\\exe\\<folder>\\logs and takes FOLDER:NNN (log numbers repeat per folder). No game, no index.

    py -I tools\\q\\qrunlog_exe_selftest.py
"""
# entry: resolve_run
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qrunlog  # noqa: E402


def main():
    d = tempfile.mkdtemp(prefix='qrunlog_exe_')
    for folder in ('lane-g2l-abcdef123', 'abcdef123'):
        os.makedirs(os.path.join(d, 'out', 'exe', folder, 'logs'))
        with open(os.path.join(d, 'out', 'exe', folder, 'wwe2k14.exe'), 'wb') as f:
            f.write(b'MZ')
        with open(os.path.join(d, 'out', 'exe', folder, 'logs', 'wwe2k14_001.log'), 'w') as f:
            f.write('planted ' + folder)
    os.makedirs(os.path.join(d, 'out', 'exe', 'nologs'))
    os.makedirs(os.path.join(d, 'out', 'build', 'win-amd64-release', 'logs'))      # S106 G2LF F10: out\build's own wwe2k14_001.log (the same number as the exe folders')
    with open(os.path.join(d, 'out', 'build', 'win-amd64-release', 'logs', 'wwe2k14_001.log'), 'w') as f:
        f.write('planted out-build')
    os.makedirs(os.path.join(d, 'out', 'exe', 'onlyexe', 'logs'))
    with open(os.path.join(d, 'out', 'exe', 'onlyexe', 'logs', 'wwe2k14_009.log'), 'w') as f:
        f.write('planted onlyexe')
    qrunlog.REPO = d
    res = []
    found = qrunlog.find_logs()
    res.append(('find_logs sees the planted out\\exe\\x\\logs\\wwe2k14_001.log in both folders (+ out\\build\'s and onlyexe\'s)', len(found) == 4))
    p, note = qrunlog.resolve_run('lane-g2l-abcdef123:1')
    res.append(('FOLDER:NNN takes that folder\'s log, not the other folder\'s equal number', 'lane-g2l-abcdef123' in p and not note))
    res.append(('  ... the exe of that log is the one in its folder', qrunlog.exe_of(p) is not None and 'lane-g2l-abcdef123' in str(qrunlog.exe_of(p))))
    p2, note2 = qrunlog.resolve_run('1')
    res.append(('S106 G2LF F10 UNSAFE TWIN: a bare number resolves to out\\build\'s log although out\\exe logs carry the same number', os.sep + 'build' + os.sep in p2 and os.sep + 'exe' + os.sep not in p2 and not note2))
    try:
        qrunlog.resolve_run('9')
        res.append(('F10: a bare number only an out\\exe log carries is an error naming FOLDER:NNN, never a silent pick', False))
    except qrunlog.RunlogError as e:
        res.append(('F10: a bare number only an out\\exe log carries is an error naming FOLDER:NNN, never a silent pick', 'FOLDER:NNN' in str(e)))
    p9, _n9 = qrunlog.resolve_run('onlyexe:9')
    res.append(('F10: FOLDER:NNN still reaches that out\\exe log', 'onlyexe' in p9))
    try:
        qrunlog.resolve_run('nologs:1')
        res.append(('UNSAFE TWIN: FOLDER:NNN of a folder with no such log is an error', False))
    except qrunlog.RunlogError:
        res.append(('UNSAFE TWIN: FOLDER:NNN of a folder with no such log is an error', True))
    try:
        qrunlog.resolve_run('abcdef123:2')
        res.append(('UNSAFE TWIN: FOLDER:NNN with a number the folder lacks is an error', False))
    except qrunlog.RunlogError:
        res.append(('UNSAFE TWIN: FOLDER:NNN with a number the folder lacks is an error', True))
    fail = 0
    for n, ok in res:
        print(('ok   ' if ok else 'FAIL ') + n)
        fail += not ok
    return 1 if fail else 0


if __name__ == '__main__':
    sys.exit(main())
