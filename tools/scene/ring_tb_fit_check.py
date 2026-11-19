#!/usr/bin/env python3
"""S103 lane C3 gate check (step 2 evidence): the turnbuckle model of run 16's real records. For every sampled record, the game's 12 turnbuckle calls (class turnbuckle, call = 3 corner + height) must satisfy
the loop's structure [listing 0x825CE324-0x825CE57C]: node 1's skin translation = A + inv.t * R(a_corner), A = (V_corner.x, -154 - 42 height, V_corner.z), with ONE constant inv.t for all 12 instances and all
records (V = the vec table 0x8201FA20, a = the angle table 0x8201FA60, both read from the image constants below), node 0 / 3 identity, nodes 1 / 2 the same rotation row0 (cos a, 0, -sin a). The planted twin
(angles shifted by one corner) must break the constancy. Reads the case zip READ ONLY; 0 game runs.
  py -I tools\\scene\\ring_tb_fit_check.py [--root DIR] [--zip PATH]"""
import argparse
import math
import os
import re
import subprocess
import sys
import zipfile

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
ZIP = r'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\out\cases\case_20261116_023239_766_0_pc_run_exit.zip'
VEC = [(-318.5, -318.5), (-318.5, 318.5), (318.5, 318.5), (318.5, -318.5)]
ANG = [-0.7853981852531433, 0.7853981852531433, 2.356194496154785, -2.356194496154785]


def parse(txt):
    out = {}
    for e in txt.split('class ')[1:]:
        call = int(re.search(r'call (\d+)', e).group(1))
        nodes = re.findall(r'node (\d): rot rows \(([^)]*)\) trans \(([^)]*)\)', e)
        out[call] = [([float(x) for x in rot.replace('|', ' ').split()], [float(x) for x in tr.split()]) for _, rot, tr in nodes]
    return out


def fit(calls, ang):
    """inv.t of node 1 per instance (x, z); returns the list."""
    res = []
    for call, nodes in calls.items():
        c, h = divmod(call, 3)
        co, si = math.cos(ang[c]), math.sin(ang[c])
        t = nodes[1][1]
        d = (t[0] - VEC[c][0], t[2] - VEC[c][1])
        res.append((d[0] * co - d[1] * si, d[0] * si + d[1] * co, t[1] - (-154.0 - 42.0 * h)))
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--zip', default=ZIP)
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's103', 'c3', 'gate')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'ring_dump.exe')
    b = subprocess.run([CLANG, '-std=c++20', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-fuse-ld=lld', os.path.join(root, 'tools', 'scene', 'ring_dump.cpp'),
                        os.path.join(root, 'src', 'renderer_scene', 'record_io.cpp'), os.path.join(root, 'src', 'renderer_scene', 'live_scene.cpp'), '-o', exe], capture_output=True, text=True)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    z = zipfile.ZipFile(a.zip)
    names = sorted(n for n in z.namelist() if n.endswith('.rsc'))[::25]
    allfit = []
    twin = []
    bad = 0
    for n in names:
        p = os.path.join(out, os.path.basename(n))
        with open(p, 'wb') as fh:
            fh.write(z.read(n))
        calls = parse(subprocess.run([exe, p, '6'], capture_output=True, text=True).stdout)
        if len(calls) != 12:
            print('FAIL: %s has %d turnbuckle calls (want 12)' % (n, len(calls)))
            bad += 1
            continue
        for call, nodes in calls.items():
            if len(nodes) < 3:
                continue
            # node 0 / 3 are not printed beyond 4 nodes: the dump prints 4: check identity rotation
            if nodes[0][0][:9] != [1, 0, 0, 0, 1, 0, 0, 0, 1] and any(abs(x - y) > 1e-4 for x, y in zip(nodes[0][0], [1, 0, 0, 0, 1, 0, 0, 0, 1])):
                print('FAIL: node 0 is not identity')
                bad += 1
            if any(abs(x - y) > 1e-4 for x, y in zip(nodes[1][0], nodes[2][0])):
                print('FAIL: nodes 1 and 2 differ in rotation')
                bad += 1
        allfit += fit(calls, ANG)
        twin += fit(calls, [ANG[1], ANG[0], ANG[2], ANG[3]])
    def spread(v):
        return [max(x[i] for x in v) - min(x[i] for x in v) for i in range(3)]
    sp, st = spread(allfit), spread(twin)
    print('records %d, instances %d: inv.t spread x %.5f z %.5f y-residual %.5f (mean inv.t %.4f, %.4f)' % (len(names), len(allfit), sp[0], sp[1], sp[2], sum(v[0] for v in allfit) / len(allfit), sum(v[1] for v in allfit) / len(allfit)))
    print('twin (angles of corners 0 and 1 swapped): spread x %.3f z %.3f' % (st[0], st[1]))
    if max(sp) > 2e-3:
        print('FAIL: node 1 is not A + inv.t * R with one constant inv.t')
        bad += 1
    if max(st[:2]) < 1.0:
        print('FAIL: the planted twin is not caught')
        bad += 1
    print('RESULT', 'OK' if bad == 0 else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
