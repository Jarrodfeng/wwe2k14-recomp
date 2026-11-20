r"""A stand-in for admit.exe in the wiring units (tools\admit\wiring_test.py), run through a .cmd the test writes and names in WWE_ADMIT_EXE.
Behaviour from env STUB_MODE: 'grant N [WORD]' (print `granted N [WORD]`; STUB_SEQ 'grant 5 held;grant 5 nested' answers successive calls in turn), 'fail' (no output, exit 3), 'garbage' (print nonsense, exit 0), 'timeout75' (no output,
exit 75), 'hang' (print nothing and never answer: writes its pid to STUB_LOG + '.pid', then blocks; the helper under test must kill it).
Every call appends its argv (one line, JSON) to env STUB_LOG; a stdin-form hold (no --detach) then waits for stdin EOF and appends 'released'.
A `release` call always answers at once (exit 0)."""
import json
import os
import sys
import threading


def log(x):
    p = os.environ.get('STUB_LOG')
    if p:
        with open(p, 'a', encoding='utf-8') as f:
            f.write(json.dumps(x) + '\n')


def main():
    argv = sys.argv[1:]
    log(argv)
    if argv[:1] == ['release']:
        return 0
    mode = os.environ.get('STUB_MODE', 'grant 1').split()
    seq = os.environ.get('STUB_SEQ')   # 'grant 5 held;grant 5 nested': one answer per hold call, in order (a counter file beside STUB_LOG)
    if seq:
        cf = os.environ['STUB_LOG'] + '.n'
        i = int(open(cf).read()) if os.path.exists(cf) else 0
        with open(cf, 'w') as f:
            f.write(str(i + 1))
        steps = seq.split(';')
        mode = steps[min(i, len(steps) - 1)].split()
    if mode[0] == 'fail':
        return 3
    if mode[0] == 'timeout75':
        return 75
    if mode[0] == 'garbage':
        print('what is this', flush=True)
        return 0
    if mode[0] == 'hang':
        with open(os.environ['STUB_LOG'] + '.pid', 'w') as f:
            f.write(str(os.getpid()))
        threading.Event().wait()   # blocks forever without using the CPU
        return 0
    print('granted %s' % ' '.join(mode[1:]), flush=True)   # 'grant 5' -> `granted 5` (an older admit); 'grant 5 observed' -> `granted 5 observed`
    if argv[:1] == ['hold'] and '--detach' not in argv:
        sys.stdin.read()
        log('released')
    return 0


if __name__ == '__main__':
    sys.exit(main())
