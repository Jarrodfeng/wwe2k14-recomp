#!/usr/bin/env python3
"""S103 lane C6 gate check: the offline replay of the ring's covers and apron skirts over run 26's case (a mode-1 mirror of a CPU vs CPU match, ropes moving; tools\\scene\\ring_replay.cpp).
For every sampled record the SHIPPED contributor (readers + rules + the whole-ring verdict) runs on a fake guest built at the listing literals from the record's own stream draws; what it emits must equal what the game
drew: the 12 cover instances (call set + rows) and the visible skirts (count + rows). Planted faults (the two address misreads of run 26, a rule that never hides a skirt, a dropped corner) must be caught.
Also the LISTING LITERAL LINT (review finding of lane C5/C6 made a check): the addresses the shipped constants name are recomputed from the generated listing of sub_82609060 / sub_825CE668 (lis + lwz/lbz, lis | ori)
and must equal the constants in ring_recompute_core.h. Reads the case zip READ ONLY; 0 game runs.
  py -I tools\\scene\\ring_replay_check.py [--root DIR] [--zip PATH] [--every N]"""
import argparse
import glob
import os
import re
import subprocess
import sys
import zipfile

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
RX = re.compile(r'records (\d+) \(skipped (\d+)\): covers (\d+) instances: equal (\d+), differ (\d+) \(worst ([0-9.e+-]+)\); skirts (\d+) instances: equal (\d+), differ (\d+), records with the same skirt count (\d+); '
                r'whole-ring clean (\d+) of (\d+); rope-derived corner y vs the grid record: worst gap ([0-9.e+-]+); the case\'s recorded OURS \(the code of run 26\): cover rows (\d+) of the game\'s (\d+), skirt rows (\d+) of the game\'s (\d+)')
KEYS = ('recs', 'skipped', 'cov', 'cov_eq', 'cov_diff', 'cov_worst', 'sk', 'sk_eq', 'sk_diff', 'sk_cnt', 'clean', 'of', 'gap', 'old_cov', 'old_cov_g', 'old_sk', 'old_sk_g')


def func_body(root, name):
    pat = 'DEFINE_REX_FUNC(%s)' % name
    gen = os.path.join(root, 'generated', 'default')
    if not os.path.isdir(gen):   # a worktree has no generated\ (gitignored): the main checkout's
        gen = r'C:\Users\Jarrod\wwe2k14-recomp\generated\default'
    for fn in sorted(glob.glob(os.path.join(gen, 'wwe2k14_recomp.*.cpp'))):
        with open(fn, encoding='utf-8', errors='replace') as f:
            t = f.read()
        i = t.find(pat)
        if i >= 0:
            j = t.find('DEFINE_REX_FUNC(', i + 10)
            return t[i:j if j > 0 else len(t)]
    return None


def literals(body):
    """The addresses a listing names: lis/li value in a register, then lwz/lbz base+off and ori."""
    regs = {}
    loads = set()
    oris = set()
    for ln in body.splitlines():
        ln = ln.strip()
        m = re.match(r'ctx\.r(\d+)\.s64 = (-?\d+);$', ln)
        if m:
            regs[int(m.group(1))] = int(m.group(2)) & 0xFFFFFFFF
            continue
        m = re.match(r'ctx\.r(\d+)\.u64 = ctx\.r(\d+)\.u64 \| (\d+);$', ln)
        if m and int(m.group(2)) in regs:
            v = regs[int(m.group(2))] | int(m.group(3))
            regs[int(m.group(1))] = v
            oris.add(v)
            continue
        m = re.match(r'ctx\.r(\d+)\.u64 = REX_LOAD_U(?:32|8)\(ctx\.r(\d+)\.u32 \+ (-?\d+)\);$', ln)
        if m:
            if int(m.group(2)) in regs:
                loads.add((regs[int(m.group(2))] + int(m.group(3))) & 0xFFFFFFFF)
            regs.pop(int(m.group(1)), None)
            continue
        m = re.match(r'ctx\.r(\d+)\.[us]64 = ', ln)
        if m:
            regs.pop(int(m.group(1)), None)
    return loads, oris


def lint(root):
    bad = 0
    core = open(os.path.join(root, 'src', 'scene_live', 'ring_recompute_core.h'), encoding='utf-8').read()

    def const(name):
        m = re.search(r'\b%s = (0x[0-9A-Fa-f]+|\d+)' % name, core)
        return int(m.group(1), 0) if m else None
    sk = func_body(root, 'sub_82609060')
    cv = func_body(root, 'sub_825CE668')
    if sk is None or cv is None:
        print('FAIL: lint: the generated listing of sub_82609060 / sub_825CE668 is not found under generated\\default')
        return 1
    loads, _ = literals(sk)
    for nm in ('kSkirtViewGlobal', 'kSkirtDrawWord', 'kRingGlobal'):
        v = const(nm)
        ok = v in loads
        print('lint %-18s %s %s' % (nm, '0x%08X' % v if v is not None else '?', 'in the listing' if ok else 'NOT in the listing of sub_82609060 (%s)' % ', '.join('0x%08X' % x for x in sorted(loads) if x >> 16 in (0x836F, 0x8376))))
        bad += 0 if ok else 1
    _, oris = literals(cv)
    d = const('kCvDescOff')
    ok = d is not None and (d + 8) in oris
    print('lint %-18s %s %s' % ('kCvDescOff', d, 'n at r16 - 8 (lis | ori %s)' % sorted(oris) if ok else 'NOT r16 - 8: the ori results are %s' % sorted(oris)))
    bad += 0 if ok else 1
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--zip', default=r'C:\Users\Jarrod\wwe2k14-recomp\out\s103\run26_case.zip')
    ap.add_argument('--every', type=int, default=3)
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's103', 'c6g')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'ring_replay.exe')
    b = subprocess.run([CLANG, '-std=c++20', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-fuse-ld=lld', os.path.join(root, 'tools', 'scene', 'ring_replay.cpp'),
                        os.path.join(root, 'src', 'renderer_scene', 'record_io.cpp'), os.path.join(root, 'src', 'renderer_scene', 'live_scene.cpp'), '-o', exe], capture_output=True, text=True, cwd=out)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    zp = a.zip if os.path.isfile(a.zip) else os.path.join(root, 'out', 's103', 'run26_case.zip')
    if not os.path.isfile(zp):
        print('FAIL: the run 26 case zip is missing: ' + a.zip)
        return 1
    z = zipfile.ZipFile(zp)
    names = sorted(n for n in z.namelist() if n.endswith('.rsc'))[::a.every]
    files = []
    for n in names:
        p = os.path.join(out, os.path.basename(n))
        if not os.path.isfile(p):
            with open(p, 'wb') as fh:
                fh.write(z.read(n))
        files.append(p)

    def run(plant=None):
        r = subprocess.run([exe] + (['--plant', plant] if plant else []) + files, capture_output=True, text=True, cwd=out)
        m = RX.search(r.stdout)
        if not m:
            print('FAIL: no summary' + (' (plant %s)' % plant if plant else '') + '\n' + r.stdout[-600:] + r.stderr[-600:])
            return None
        return dict(zip(KEYS, (float(m.group(i + 1)) if KEYS[i] in ('cov_worst', 'gap') else int(m.group(i + 1)) for i in range(len(KEYS)))))
    bad = 0
    base = run()
    if base is None:
        return 1
    print('records %(recs)d (skipped %(skipped)d): covers %(cov)d instances: equal %(cov_eq)d, differ %(cov_diff)d (worst %(cov_worst).3g); skirts %(sk)d instances: equal %(sk_eq)d, differ %(sk_diff)d, '
          'same skirt count in %(sk_cnt)d records; whole-ring clean %(clean)d of %(of)d' % base)
    print('  informational: the run 26 code had emitted %(old_cov)d of the game\'s %(old_cov_g)d cover rows and %(old_sk)d of the game\'s %(old_sk_g)d skirt rows; the rope-derived corner sits up to %(gap).3g off the grid record' % base)
    if base['recs'] < 50:
        print('FAIL: too few records replayed')
        bad += 1
    if base['cov_diff'] or base['cov_eq'] != base['cov'] or base['cov_worst'] > 2e-3:
        print('FAIL: our covers do not equal what the game drew')
        bad += 1
    if base['sk_diff'] or base['sk_eq'] != base['sk'] or base['sk_cnt'] != base['recs'] or base['sk'] == 0:
        print('FAIL: our visible skirts do not equal what the game drew')
        bad += 1
    if base['clean'] != base['recs']:
        print('FAIL: the whole-ring verdict is not clean on every replayed record')
        bad += 1
    for plant, key in (('cv_desc_low', 'cov_diff'), ('skirt_glob_wrong', 'sk_diff'), ('skirt_hidden_none', 'sk_cnt'), ('cover_drop', 'cov_diff')):
        r = run(plant)
        if r is None:
            bad += 1
            continue
        ok = (r[key] < base['recs']) if key == 'sk_cnt' else (r[key] > 0)
        print('twin %-18s %s (%s %s)' % (plant, 'caught' if ok else 'NOT CAUGHT', key, r[key]))
        bad += 0 if ok else 1
    bad += lint(root)
    print('RESULT', 'OK' if bad == 0 else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
