"""S110 ELIMFIN: the check of src/event_bus_finish_core.h (the pure part of the bus's decision lines `match what=finish | fall | scene`).
No game, no game data.

    py -I tools/bus/finish_core_selftest.py

Compiles tools/bus/finish_core_test.cpp against src/ (clang++ -std=c++17) and runs it: one-fall rules keep the old lines read for read,
a multi-decision rule (rule byte G+13 or G+22) says `fall` for each decision and `finish` once the game commits the end (G+14580), and
the old rule (kept in the unit) is shown to say `finish` at a fall the match goes on past.
TEXT CHECKS pin the wiring: src/event_bus.h feeds the core from the record, the rule bytes and G+14580, and keeps no copy of the old
rule; src/event_wait_core.h lets a route wait on `fall`.
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
    ('src/event_bus.h', '#include "event_bus_finish_core.h"', 'event_bus.h includes the core'),
    ('src/event_bus.h', 'finish_core::Step(g_last.fin, fr)', 'Tick asks the core which line a read makes'),
    ('src/event_bus.h', 'fr.multi = finish_core::MultiDecision(R8(kRules + 13), R8(kRules + 22));', 'the rule bytes G+13 / G+22 feed the core'),
    ('src/event_bus.h', 'fr.ended = R32(kFinishTime) != 0;', 'G+14580 feeds the core'),
    ('src/event_bus.h', 'constexpr uint32_t kFinishTime = 0x8376E3E8;', 'G+14580 is 0x8376E3E8'),
    ('src/event_bus.h', 'finish_core::What(fin_line)', 'the line says the core\'s what word'),
    # S110 ELIMFIN fix 2 (run 293): a fall is a rise of the game's tally, int16 at G+14588 + 2*slot
    ('src/event_bus.h', 'fr.falls[s] = int16_t(R16(kRules + finish_core::kTallyOffset + 2 * uint32_t(s)));', 'the tally feeds the core'),
    ('src/event_bus.h', 'finish_core::Falls(g_last.fin, fr)', 'Tick asks the core which falls a read scored'),
    ('src/event_bus.h', '"\\"what\\": \\"fall\\", \\"scorer_slot\\": {}, \\"scorer_id\\": {}, \\"falls\\": {}, {}"', 'the fall line names its scorer and count'),
    ('src/event_wait_core.h', '"finish", "fall", "eliminated"', 'a route may wait on `fall` (kMatchWhats)'),
]
ABSENT = [
    ('src/event_bus.h', 'fin_emitted_key', 'a copy of the old rule left in event_bus.h'),
    ('src/event_bus.h', 'kSceneFinishType', 'a second scene constant beside finish_core::kSceneType'),
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
    tmp = tempfile.mkdtemp(prefix='finish_core_')
    try:
        exe = os.path.join(tmp, 'finish_core_test.exe')
        r = subprocess.run([clang(), '-std=c++17', '-O0', '-I', os.path.join(REPO, 'src'), os.path.join(HERE, 'finish_core_test.cpp'),
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
    print('finish_core selftest: %d problems' % len(problems))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
