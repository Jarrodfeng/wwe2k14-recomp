"""S102 W3: build and run the differential test (w3_diff.cpp), the planted twins and the microbenchmark. 0 game runs: no game exe, no Surface.

    py -I tools\\w3\\diff\\run.py [--states N] [--seed S] [--plants 1,2,...] [--bench] [--no-plants]

  1. extract_ref.py writes the reference (the recompiled functions) from the local generated code
  2. w3_diff.cpp is compiled with the game's flags (build.ninja of the main checkout) into out\\s102\\w3\\: the clean test and one exe per planted twin
  3. the clean test must exit 0 (every state equal); every planted twin must exit 1 (caught): a twin that survives is a FAIL of this check
Exit 0 = clean test equal AND every twin caught; 1 = otherwise; 2 = an input missing.
"""
import argparse
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.dirname(HERE))
import game_tu_check as gtc  # noqa: E402  (flags from the main checkout's build.ninja)

MAIN = gtc.MAIN
GEN = os.path.join(MAIN, 'generated', 'default')
OUT = os.path.join(MAIN, 'out', 's102', 'w3')
PLANTS = {
    1: 'dispatcher: the last two table ops (103, 104) treated as "above the table"',
    2: 'dispatcher: op 35 calls the op 36 handler',
    3: 'loop: the slot is not read again after the dispatch',
    4: 'loop: the position advances by the length without the +1',
    5: 'loop: a missed write (the list start is not stored in the position word)',
    6: 'loop: op 104 ends the whole replay, not the list',
    7: 'loop: the stack pointer is not restored',
    8: 'loop: op 81 skips the hook (the freed-model check) on the fast path',
    9: 'loop: one list too many (<= count)',
    10: 'loop: no early return on a list count of 0',
    11: 'loop: the bookkeeping that is on (the replay guard) is ignored: the fast path runs anyway',
    12: 'dispatcher: the lr of the case is not set before a direct handler call',
    13: 'loop: the back chain and the saved lr of the frame are not written',
}


def build(plant, tag):
    args, pch_src = gtc.flags(MAIN)
    out = []
    for x in args:
        if x.replace('\\', '/').lower() == '-i' + (MAIN + '\\src').replace('\\', '/').lower():
            continue  # the harness includes the checkout's headers by relative path
        out.append(x)
    exe = os.path.join(OUT, 'w3_diff%s.exe' % tag)
    cmd = [gtc.CLANG, '-O3', '-g0', '-Wno-deprecated-declarations', '-Wno-unknown-escape-sequence', *[a for a in out if a not in ('-DNDEBUG',)], '-I' + OUT,
           '-DW3_PLANT=%d' % plant, os.path.join(HERE, 'w3_diff.cpp'), '-o', exe, '-luser32']
    p = subprocess.run(cmd, capture_output=True, text=True)
    return exe, p


def run(exe, extra):
    p = subprocess.run([exe, *extra], capture_output=True, text=True, timeout=900)
    return p.returncode, p.stdout + p.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--states', type=int, default=6000)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--plants', default=','.join(str(k) for k in PLANTS))
    ap.add_argument('--no-plants', action='store_true')
    ap.add_argument('--bench', action='store_true')
    a = ap.parse_args()
    r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'extract_ref.py'), '--gen', GEN, '--out', OUT])
    if r.returncode:
        return 2
    plants = [] if a.no_plants else [int(x) for x in a.plants.split(',') if x]
    jobs = [(0, '')] + [(k, '_p%d' % k) for k in plants]
    with ThreadPoolExecutor(max_workers=4) as ex:
        built = list(ex.map(lambda j: (j, *build(*j)), jobs))
    for (j, exe, p) in built:
        if p.returncode:
            print('BUILD FAILED for plant %d:\n%s' % (j[0], '\n'.join(p.stderr.splitlines()[:40])))
            return 2
    status = 0
    (_, clean_exe, _) = built[0]
    if a.bench:
        code, text = run(clean_exe, ['--bench'])
        print(text)
        return code
    code, text = run(clean_exe, ['--states', str(a.states), '--seed', str(a.seed)])
    print('[clean]', text.strip())
    if code != 0:
        print('FAIL: the native code differs from the recompiled functions')
        status = 1
    # the fault-comparison twin (F4): when both sides fault the calls before the fault are compared; one call changed just before the walk leaves memory must be caught
    code, text = run(clean_exe, ['--states', str(a.states), '--seed', str(a.seed), '--plant-fault-trace'])
    first = [l for l in text.splitlines() if l.startswith('MISMATCH')]
    if code == 1 and first and 'both faulted' in first[0]:
        print('[fault twin] caught   (one handler call changed just before both sides fault) -> %s' % first[0][:170])
    else:
        print('[fault twin] NOT CAUGHT, exit %d: %s' % (code, text.strip()[:200]))
        status = 1
    for (j, exe, _) in built[1:]:
        code, text = run(exe, ['--states', str(a.states), '--seed', str(a.seed)])
        first = text.strip().splitlines()[0] if text.strip() else ''
        if code == 1:
            print('[plant %2d] caught   (%s) -> %s' % (j[0], PLANTS[j[0]], first[:150]))
        else:
            print('[plant %2d] NOT CAUGHT (%s), exit %d: %s' % (j[0], PLANTS[j[0]], code, text.strip()[:200]))
            status = 1
    print('w3 differential: %s' % ('PASS' if status == 0 else 'FAIL'))
    return status


if __name__ == '__main__':
    sys.exit(main())
