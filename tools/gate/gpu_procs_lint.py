#!/usr/bin/env python3
r"""Lint: a gate check whose script starts several GPU processes at once says how many (S103 GSP2, review "GPU slot accounting").

  py -I tools\gate\gpu_procs_lint.py [--root DIR]

The gate gives a GPU check ONE slot of the PC-wide pool (10). A script that runs several rig exes at the same time (a thread pool of subprocesses) puts that many processes on the GPU for one slot, and
two or three such checks overrun the pool the slots exist to protect. Rule: a lane check whose command runs a .py script that BOTH names a GPU rig exe (sched.RIG_GPU) AND starts processes in parallel
(a ThreadPoolExecutor / ProcessPoolExecutor / threading.Thread) declares `"gpu_procs": N` (N >= 1; the gate then takes N slots at once and hands the script N in the env GATE_GPU_PROCS), and a script
of a check that declares gpu_procs > 1 must read GATE_GPU_PROCS (so the number it runs is the number it holds). Exit 0 = clean.
The checks that already ran parallel arms when this lint arrived (S103 GSP2; their worker counts were never reviewed) are listed in tools\gate\gpu_procs_baseline.txt with that reason: they are NOT
declared yet, so they still hold one slot (the old behaviour); the lint fails for any NEW one, and for a baseline entry that has since declared gpu_procs (remove it). Declaring one = read its worker count, add
"gpu_procs": N to the lane, make the script read GATE_GPU_PROCS."""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sched as gsched   # noqa: E402  (tools/gate/sched.py; the stdlib `sched` is shadowed on purpose by the script directory)

ALL_CHECKS = []
PARALLEL = re.compile(r'ThreadPoolExecutor|ProcessPoolExecutor|threading\.Thread\(')   # (a bare Popen is how most scripts run ONE exe at a time: not parallel by itself)


def script_of(cmd):
    for t in cmd:
        if isinstance(t, str) and t.endswith('.py') and not t.startswith('-'):
            return t.replace('\\', '/')
    return None


def lint(root):
    problems, seen = [], 0
    base_file = os.path.join(root, 'tools', 'gate', 'gpu_procs_baseline.txt')
    baseline = {ln.split('|')[0].strip() for ln in open(base_file, encoding='utf-8')} if os.path.isfile(base_file) else set()
    baseline.discard('')
    names, found = set(), set()
    lanes = os.path.join(root, 'tools', 'gate', 'lanes')
    for f in sorted(os.listdir(lanes)):
        if not f.endswith('.json'):
            continue
        try:
            data = json.load(open(os.path.join(lanes, f), encoding='utf-8'))
        except Exception as e:
            problems.append('%s: unreadable (%s)' % (f, e))
            continue
        for c in data.get('checks', []):
            ALL_CHECKS.append(c)
            sc = script_of(c.get('cmd', []))
            if not sc or c.get('gpu') is False or sc.endswith('gpu_procs_lint.py'):   # (this file names a rig exe and a pool in its own selftest text)
                continue
            p = os.path.join(root, sc)
            if not os.path.isfile(p):
                continue
            text = open(p, encoding='utf-8', errors='replace').read()
            n = c.get('gpu_procs')
            names.add(c.get('name'))
            if gsched.RIG_GPU.search(text) and PARALLEL.search(text):
                seen += 1
                found.add(c.get('name'))
                if n is None and c.get('name') not in baseline:
                    problems.append('%s %s: %s starts several GPU rig processes at once but the check declares no "gpu_procs"' % (f, c.get('name'), sc))
            if n is not None:
                if not isinstance(n, int) or n < 1:
                    problems.append('%s %s: gpu_procs must be an integer >= 1 (got %r)' % (f, c.get('name'), n))
                elif n > 1 and 'GATE_GPU_PROCS' not in text:
                    problems.append('%s %s: gpu_procs %d but %s never reads GATE_GPU_PROCS (the number it runs must be the number it holds)' % (f, c.get('name'), n, sc))
    for b in sorted(baseline):
        if b not in names:
            problems.append('baseline entry %s names no check any more (remove it from gpu_procs_baseline.txt)' % b)
        elif b not in found or any(c.get('name') == b and c.get('gpu_procs') for c in ALL_CHECKS):
            problems.append('baseline entry %s no longer needs to be there (it declares gpu_procs or is not parallel: remove it)' % b)
    return problems, seen


def selftest():
    """Planted twins on a scratch tree: a parallel-GPU script with no declaration, a declaration > 1 whose script never reads the env, and the clean pair must read FAIL, FAIL, PASS."""
    import tempfile
    ok = True
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, 'tools', 'gate', 'lanes'))
        os.makedirs(os.path.join(d, 'tools', 'x'))
        par = "import concurrent.futures\nEXE = 'frame_rig.exe'\nex = concurrent.futures.ThreadPoolExecutor(max_workers=4)\n"
        open(os.path.join(d, 'tools', 'x', 'bare.py'), 'w').write(par)
        open(os.path.join(d, 'tools', 'x', 'env.py'), 'w').write(par + "import os\nN = os.environ.get('GATE_GPU_PROCS')\n")

        def lane(check):
            json.dump({'checks': [check]}, open(os.path.join(d, 'tools', 'gate', 'lanes', 'l.json'), 'w'))
            del ALL_CHECKS[:]
            return lint(d)[0]
        cases = (('parallel GPU script, no declaration', {'name': 'l.a', 'cmd': ['py', 'tools/x/bare.py']}, True),
                 ('gpu_procs 4 but the script never reads GATE_GPU_PROCS', {'name': 'l.b', 'cmd': ['py', 'tools/x/bare.py'], 'gpu_procs': 4}, True),
                 ('declared and read', {'name': 'l.c', 'cmd': ['py', 'tools/x/env.py'], 'gpu_procs': 4}, False))
        for label, check, want_fail in cases:
            got = bool(lane(check))
            print('%s selftest: %s -> %s' % ('PASS' if got == want_fail else 'FAIL', label, 'flagged' if got else 'clean'))
            ok &= got == want_fail
    del ALL_CHECKS[:]
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    ap.add_argument('--write-baseline', action='store_true', help='write tools/gate/gpu_procs_baseline.txt from the checks that fail now (once, when the lint arrives)')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    if a.write_baseline:
        problems, seen = lint(root)
        names = sorted(m.group(1) for m in (re.match(r'^\S+ (\S+): ', p) for p in problems) if m)
        open(os.path.join(root, 'tools', 'gate', 'gpu_procs_baseline.txt'), 'w', encoding='utf-8').write(''.join('%s | parallel GPU arms from before the lint; worker count never reviewed (declare gpu_procs when touched)\n' % n for n in names))
        print('baseline: %d checks' % len(names))
        return 0
    if not selftest():
        return 1
    problems, seen = lint(root)
    for p in problems:
        print('FAIL ' + p)
    print('%s gpu_procs lint: %d parallel-GPU check(s) found, %d problem(s)' % ('FAIL' if problems else 'PASS', seen, len(problems)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
