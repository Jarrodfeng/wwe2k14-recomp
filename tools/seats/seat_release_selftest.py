"""S110 SEATREL (moved here from seats_proto_selftest.py by SEAT3 lane G-B): the host unit of src/seat_release_core.h and its planted twins. No game runs.

    py -I tools/seats/seat_release_selftest.py   compiles tools/seats/seat_release_unit.cpp against src/seat_release_core.h with clang++, runs it (every
                                                 check must PASS), then compiles it against COPIES of the header that each carry one planted fault and
                                                 requires the unit to FAIL on each (a crash with no FAIL line is a PROBLEM, not a caught fault).

Exit 0 = the unit passes on the shipped header and fails on every planted twin. Plant patterns use \\r?\\n where they span a line (a checkout may be CRLF).
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.append(os.path.join(REPO, 'tools', 'harness'))
import plant_runner as pr   # S110 PLANTFAST: the shared bounded pool of the planted builds
HEADER = os.path.join(REPO, 'src', 'seat_release_core.h')

PLANTS = [
    ('release_other_id_key', r'key / 100 == uint32_t\(to\)', 'key / 1000 == uint32_t(to) / 10'),
    ('release_ignores_loaded_flag', r'!read\(d \+ kDescLoaded, loaded\) \|\| loaded == 0', '!read(d + kDescLoaded, loaded)'),
    ('release_misses_last_slot', r'i < kDescriptors;', 'i < kDescriptors - 1;'),
    ('sequence_only_ingame_refused', r'if \(menu_home::IsFrontEndSeq\(seq\)\)', 'if (seq != 9)'),
    ('sequence_any_allowed', r'if \(menu_home::IsFrontEndSeq\(seq\)\)', 'if (true)'),
]


def find_clang():
    for c in (shutil.which('clang++'), r'C:\Program Files\LLVM\bin\clang++.exe', '/usr/bin/clang++'):
        if c and os.path.isfile(c):
            return c
    raise RuntimeError('no clang++ found (tools\\env.ps1 puts the project\'s LLVM on PATH)')


def build(header_text, tmp, tag):
    inc = os.path.join(tmp, tag)
    os.makedirs(inc, exist_ok=True)
    with open(os.path.join(inc, 'seat_release_core.h'), 'w', encoding='utf-8', newline='') as f:
        f.write(header_text)
    exe = os.path.join(inc, 'seat_release_unit.exe')
    # -I inc first (the planted copy wins), then src for menu_home_core.h (the front-end sequence list)
    r = subprocess.run([find_clang(), '-std=c++17', '-Wall', '-I', inc, '-I', os.path.join(REPO, 'src'), os.path.join(HERE, 'seat_release_unit.cpp'), '-o', exe],
                       capture_output=True, text=True, timeout=180)
    if r.returncode != 0 or not os.path.isfile(exe):
        raise RuntimeError('seat_release_unit did not compile (%s): %s' % (tag, (r.stdout + r.stderr)[-800:]))
    return exe


def run_test(exe):
    try:
        r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return 1, 'FAIL timeout\n'
    return r.returncode, r.stdout


def main():
    text = open(HEADER, encoding='utf-8', newline='').read()
    problems = []
    tmp = tempfile.mkdtemp(prefix='seat_release_unit_')
    try:
        # S110 PLANTFAST: the shipped build and every plant build run on tools\harness\plant_runner's bounded pool (each in its own folder under tmp);
        # the verdicts below are the serial script's, in plant order
        once = pr.Once(pr.make_spec(os.path.join(HERE, 'seat_release_unit.cpp'), {os.path.basename(HEADER): HEADER}, ['-std=c++17', '-Wall'], [os.path.join(REPO, 'src')]),
                       pr.fail_lines(None, 'FAIL'), os.path.basename(HEADER))
        sh, outs = pr.run_plants(text, PLANTS, lambda t, tag: build(t, tmp, tag), lambda exe, tag: run_test(exe), pr.parse_jobs(sys.argv[1:]), once)
        if sh.error:
            raise RuntimeError(sh.error)
        rc, out = sh.rc, sh.out
        sys.stdout.write(out)
        if rc != 0:
            problems.append('the unit FAILS on the shipped header')
        for o in outs:
            name, prc, pout = o.name, o.rc, o.out
            if o.matches != 1:
                problems.append('plant %s: the pattern matched %d times (seat_release_core.h changed under the plant)' % (name, o.matches))
                continue
            if o.error:
                problems.append('plant %s: %s' % (name, o.error))
                continue
            failed = [l for l in pout.splitlines() if l.startswith('FAIL')]
            if prc == 0:
                problems.append('plant %s: the unit still PASSES' % name)
            elif not failed:
                problems.append('plant %s: the unit ended with code %d without a FAIL line (a crash, not a caught fault)' % (name, prc))
            print('plant %-30s unit %s (%d failing checks)%s' % (name, 'FAILS' if prc else 'PASSES', len(failed), ': ' + failed[0][6:70].strip() if failed else ''))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    pr.once_line()
    for p in problems:
        print('PROBLEM: ' + p)
    print('seat_release_selftest: %s' % ('OK' if not problems else '%d problem(s)' % len(problems)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
