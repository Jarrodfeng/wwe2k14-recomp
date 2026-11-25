"""S110 TABLEEVENT: the check of src/table_break_core.h (the pure part of the bus event `break what=table`). No game, no game data.

    py -I tools/bus/table_break_selftest.py

Compiles tools/bus/table_break_test.cpp against src/ (clang++ -std=c++17) and runs it: the latch edge, the hit path's return address,
the event's fields, finish type 2's name by the rule bits.
TEXT CHECKS pin the wiring (S110 TABLELATCH): the event's hooks are the latch writers sub_825DDEC0 / sub_825DDFA0 in src/table_break.h,
sub_825E1960 has ONE hook and it is the dev log's in src/table_act_log.h (no event there), kernel_overrides.h includes table_break.h, the
bus names the finish through the rule bits.
Exit 0 = all passed.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
problems = []

TEXT = [
    ('src/table_break.h', 'REX_HOOK_RAW(sub_825DDEC0)', 'the latch writer slot +88 is hooked in table_break.h'),
    ('src/table_break.h', 'REX_HOOK_RAW(sub_825DDFA0)', 'the latch writer slot +84 is hooked in table_break.h'),
    ('src/table_act_log.h', 'REX_HOOK_RAW(sub_825E1960)', 'the dev log hooks sub_825E1960 in table_act_log.h'),
    ('src/kernel_overrides.h', '#include "table_break.h"', 'kernel_overrides.h includes table_break.h'),
    ('src/event_bus.h', 'case 2: return table_break::Kind1Name(table_rules);', 'finish type 2 is named by the rule bits'),
    ('src/event_bus.h', 'FinishName(finish, TableRules())', 'the finish line passes the rule bits'),
]
ABSENT = [
    ('src/table_break.h', 'REX_HOOK_RAW(sub_825E1960)', 'the motion start hooked in table_break.h (the event has ONE emitter: the writers)'),
    ('src/table_act_log.h', 'Emit(', 'a bus emit in the dev log (the event has ONE emitter: the writers)'),
]


def check(ok, msg):
    if not ok:
        problems.append(msg)
        print('FAIL: ' + msg)


def clang():
    for c in (shutil.which('clang++'), r'C:\Program Files\LLVM\bin\clang++.exe', '/usr/bin/clang++'):
        if c and os.path.exists(c):
            return c
    raise RuntimeError('no clang++ found (tools\\env.ps1 puts the project\'s LLVM on PATH)')


def read(rel):
    with open(os.path.join(REPO, *rel.split('/')), encoding='utf-8') as f:
        return f.read()


def main():
    tmp = tempfile.mkdtemp(prefix='table_break_')
    try:
        exe = os.path.join(tmp, 'table_break_test.exe')
        r = subprocess.run([clang(), '-std=c++17', '-O0', '-I', os.path.join(REPO, 'src'), os.path.join(HERE, 'table_break_test.cpp'),
                            '-o', exe], capture_output=True, text=True, timeout=240)
        check(r.returncode == 0, 'compile failed: ' + r.stderr[-1500:])
        if r.returncode == 0:
            r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
            out = r.stdout.replace('\r\n', '\n').strip()
            print(out.splitlines()[-1] if out else '(no output)')
            check(r.returncode == 0, 'the unit failed:\n' + out)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    for rel, text, what in TEXT:
        check(read(rel).count(text) == 1, 'text check: %s (%r once in %s)' % (what, text, rel))
    for rel, text, what in ABSENT:
        check(text not in read(rel), 'text check: %s' % what)
    for fn in ('sub_825E1960', 'sub_825DDEC0', 'sub_825DDFA0'):
        hooks = 0
        for name in os.listdir(os.path.join(REPO, 'src')):
            if name.endswith('.h'):
                hooks += read('src/' + name).count('REX_HOOK_RAW(%s)' % fn)
        check(hooks == 1, 'REX_HOOK_RAW(%s) appears %d times in src/*.h (want 1)' % (fn, hooks))
    print('table_break selftest: %d problems' % len(problems))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
