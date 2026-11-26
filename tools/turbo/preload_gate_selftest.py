"""S111 PRELOADGATE: the host unit of src/load_turbo_preload_gate.h and its planted twins. No game runs.

    py -I tools/turbo/preload_gate_selftest.py   compiles tools/turbo/preload_gate_unit.cpp against src/load_turbo_preload_gate.h with clang++,
                                                 runs it (every check must PASS; the unit also runs the MENULOAD keying as a twin and
                                                 requires it to miss the non-D waits), then compiles it against COPIES of the header that
                                                 each carry one planted fault and requires the unit to FAIL on each.

Exit 0 = the unit passes on the shipped header and fails on every planted twin. Plant patterns use \\r?\\n where they span a line.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
HEADER = os.path.join(REPO, 'src', 'load_turbo_preload_gate.h')
UNIT = os.path.join(HERE, 'preload_gate_unit.cpp')

PLANTS = [
    ('reopens_after_cap', r'latched = true;\r?\n    return Act::kOpen;', 'return Act::kOpen;'),
    ('done_does_not_close', r'return preload_open \? Act::kClose : Act::kNone;', 'return Act::kNone;'),
    ('done_keeps_latch', r'latched = false;\r?\n      return preload_open', 'return preload_open'),
    ('sequence_keeps_latch', r'void OnSequenceChanged\(\) \{ latched = false; \}', 'void OnSequenceChanged() {}'),
    ('opens_over_other_window', r'if \(any_window \|\| latched\)', 'if (latched)'),
    # S111 PRELOADGATE review: the answer 1 must close only the "preload" window, never another one ("ingame" / "match"
    # / "menu": OnSequenceExitAnswer's kClose stops whatever window_ is open).
    ('done_closes_other_window', r'return preload_open \? Act::kClose : Act::kNone;', 'return any_window ? Act::kClose : Act::kNone;'),
]


def find_clang():
    for c in (shutil.which('clang++'), r'C:\Program Files\LLVM\bin\clang++.exe', '/usr/bin/clang++'):
        if c and os.path.isfile(c):
            return c
    raise RuntimeError('no clang++ found (tools\\env.ps1 puts the project\'s LLVM on PATH)')


def build(header_text, tmp, tag):
    inc = os.path.join(tmp, tag)
    os.makedirs(inc, exist_ok=True)
    with open(os.path.join(inc, 'load_turbo_preload_gate.h'), 'w', encoding='utf-8', newline='') as f:
        f.write(header_text)
    exe = os.path.join(inc, 'preload_gate_unit.exe')
    r = subprocess.run([find_clang(), '-std=c++17', '-Wall', '-I', inc, UNIT, '-o', exe], capture_output=True, text=True, timeout=180)
    if r.returncode != 0 or not os.path.isfile(exe):
        raise RuntimeError('preload_gate_unit did not compile (%s): %s' % (tag, (r.stdout + r.stderr)[-800:]))
    return exe


def run(exe):
    try:
        r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return 1, 'FAIL timeout\n'
    return r.returncode, r.stdout


def main():
    text = open(HEADER, encoding='utf-8', newline='').read()
    problems = []
    tmp = tempfile.mkdtemp(prefix='preload_gate_unit_')
    try:
        rc, out = run(build(text, tmp, 'shipped'))
        sys.stdout.write(out)
        if rc != 0:
            problems.append('the unit FAILS on the shipped header')
        for name, pat, rep in PLANTS:
            planted, n = re.subn(pat, rep, text)
            if n != 1:
                problems.append('plant %s: the pattern matched %d times (load_turbo_preload_gate.h changed under the plant)' % (name, n))
                continue
            try:
                prc, pout = run(build(planted, tmp, name))
            except RuntimeError as e:
                problems.append('plant %s: %s' % (name, e))
                continue
            failed = [l for l in pout.splitlines() if l.startswith('FAIL')]
            if prc == 0:
                problems.append('plant %s: the unit still PASSES' % name)
            elif not failed:
                problems.append('plant %s: the unit ended with code %d without a FAIL line (a crash, not a caught fault)' % (name, prc))
            print('plant %-26s unit %s (%d failing checks)%s' % (name, 'FAILS' if prc else 'PASSES', len(failed), ': ' + failed[0][5:70].strip() if failed else ''))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    for p in problems:
        print('PROBLEM: ' + p)
    print('preload_gate_selftest: %s' % ('OK' if not problems else '%d problem(s)' % len(problems)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
