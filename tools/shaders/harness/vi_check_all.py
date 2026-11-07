#!/usr/bin/env python3
"""Session 95 (vertex input, Lane A): runs vs_harness.py --vi --opt for every vertex shader of src/scene_vs_swap.h's table (all 69 rows): the plain shader and its
_vi twin, both as shipped (glslang + spirv-opt -O), on random vertices and constants; bit-equal is the bar. The vertex inputs are the raw guest dwords.
No game files are read: the inputs are random.

  py tools/shaders/harness/vi_check_all.py [--jobs 4] [--plain-only] [--vs-glslang] [name ...]
  --vs-glslang  the reference side is the plain shader unoptimised (as shipped), the _vi twin optimised: tests spirv-opt -O and the vertex input at once
"""
import os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(HERE, '..'))
import build_vertex_input_vs as bv


def dynamic_rows():
    out = set()
    for line in open(bv.TABLE, encoding='utf-8').read().splitlines():
        m = re.search(r'\{0x[0-9A-Fa-f]+ull,\s*(\w+)_vert_spv,.*,\s*kDynamic,\s*\d+,\s*\d+\}', line)
        if m:
            out.add(m.group(1))
    return out


def one(name, dyn, work):
    cmd = [sys.executable, os.path.join(HERE, 'vs_harness.py'), '--mine', os.path.join(R, 'src', 'scene', name + '.vert'), '--vi', '--opt'] + (['--plain-noopt'] if '--vs-glslang' in sys.argv else []) + [
           '--rounds', '8', '--verts', '2048', '--work', work]
    if dyn:
        cmd.append('--dynamic')
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    res = [l for l in r.stdout.splitlines() if l.startswith('RESULT')]
    return name, (res[0] if res else (r.stdout + r.stderr)[-300:].replace('\n', ' | '))


def main():
    args = sys.argv[1:]
    jobs = 4
    if '--jobs' in args:
        i = args.index('--jobs')
        jobs = int(args[i + 1])
        del args[i:i + 2]
    names = [a for a in args if not a.startswith('--')]
    rows = sorted(n for n in bv.table_rows() if not n.startswith('own_crowd') and (not names or n in names))
    dyn = dynamic_rows()
    base = os.path.join(R, 'out', 'harness_vs_vi')
    bad = 0
    with ThreadPoolExecutor(jobs) as ex:
        futs = [ex.submit(one, n, n in dyn, os.path.join(base, n)) for n in rows]
        for f in futs:
            name, res = f.result()
            ok = 'PASS' in res and 'worst 0.000e+00' in res
            bad += not ok
            print('%-30s %s%s' % (name, res, '' if ok else '   <-- NOT BIT-EQUAL'))
    print('rows %d, failures %d' % (len(rows), bad))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
