#!/usr/bin/env python3
"""S103 lane RGP check: the GPU-skinned ring (rf_ring.cpp GPU branch, docs\\research\\s103\\RGS.md option 1) against the CPU-posed ring, on run 16's real record (frame 399: Lesnar vs Bryan, mode 1, the posed-ring record of
ring_posed_check.py), headless with lane C's driver (tools\\scene\\c_driver.cpp, no window, no game run). Two driver runs of the same capture: ARM A = the GPU skin (the default), ARM B = the CPU pose
(WWE_RDR_RING_GPU_SKIN=0, the cvar wwe2k14_rdr_ring_gpu_skin=false). The checks:
  1. every ring key's GPU formula (ring_gpu_skin.h, the shader's ch_skin on the chunk path) equals PoseMesh on THIS record's rows: max |p_gpu - p_cpu| < 1e-3 on every key (the `[ring_gpu]` verify lines);
  2. each shared panel of the two arms is the same picture to owned_check's tolerance: a pixel differs when a channel differs by > 12; at most 0.05 % of the pixels may (float order on the GPU: ~1e-4 px);
  3. the report says the same classes were drawn from rows in both arms (rope 12, cover 12, turnbuckle 12, post 4), nothing skipped;
  4. posed CPU writes for the ring: arm A asked ModelCache::PosedModel 0 times, arm B > 0 (the driver's counter);
  5. the validation layer and the magenta (unbound effect) counts are zero in both arms;
  6. the planted twin ring_no_pose on the GPU arm still differs from the posed ring (> 1.2 % of the pixels): the palette reaches the shader.
Reads the case zip READ ONLY (one scene record extracted to out\\s103\\rgp); the game files from the main checkout READ ONLY. 0 game runs.
  py -I tools\\scene\\ring_gpu_check.py [--root DIR] [--game DIR] [--zip PATH] [--frame N] [--no-build]"""
import argparse
import os
import re
import subprocess
import sys
import zipfile

ZIP = r'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\out\cases\case_20261116_023239_766_0_pc_run_exit.zip'
PANELS = ['ring', 'ring_nopose', 'ring_rest', 'whole_plain']


def diff_fraction(pa, pb):
    import numpy as np
    from PIL import Image
    a = np.asarray(Image.open(pa).convert('RGB')).astype(int)
    b = np.asarray(Image.open(pb).convert('RGB')).astype(int)
    return float((np.abs(a - b).max(axis=2) > 12).sum()) / a.shape[0] / a.shape[1]


def run_arm(root, game, cap, frame, tag, outdir, gpu):
    env = dict(os.environ, WWE_RDR_VALIDATION='1', WWE_RDR_RING_LISTS='1', WWE_RDR_RING_GPU_SKIN='1' if gpu else '0')
    if gpu:
        env['WWE_RDR_RING_GPU_VERIFY'] = '1'
    p = subprocess.run([os.path.join(root, 'out', 'tools', 'c_driver.exe'), '--game', game, '--capture', cap, '--frame', str(frame), '--out', outdir, '--tag', tag, '--cam', '1100,500,0,45'],
                       capture_output=True, text=True, env=env, timeout=900)
    log = p.stdout + p.stderr
    with open(os.path.join(outdir, tag + '_log.txt'), 'w', encoding='utf-8') as fh:
        fh.write(log)
    return p.returncode, log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--game', default=r'C:\Users\Jarrod\wwe2k14-recomp\game_data\extracted')
    ap.add_argument('--zip', default=ZIP)
    ap.add_argument('--frame', type=int, default=399)
    ap.add_argument('--no-build', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    if not a.no_build:
        r = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'scene', 'build_c_driver.ps1')], cwd=root, capture_output=True, text=True)
        if r.returncode:
            print('FAIL: c_driver did not build\n' + r.stdout[-2000:] + r.stderr[-2000:])
            return 1
    for need, what in ((a.zip, 'the run 16 case zip'), (a.game, 'the game files')):
        if not os.path.exists(need):
            print('NEEDS-MAIN %s is missing: %s' % (what, need))
            return 4
    out = os.path.join(root, 'out', 's103', 'rgp')
    cap = os.path.join(out, 'cap')
    os.makedirs(os.path.join(cap, 'scene'), exist_ok=True)
    name = 'frame_%06d.rsc' % a.frame
    with zipfile.ZipFile(a.zip) as z, open(os.path.join(cap, 'scene', name), 'wb') as fh:
        fh.write(z.read('capture/scene/' + name))
    bad = 0
    logs = {}
    for tag, gpu in (('gpu', True), ('cpu', False)):
        rc, log = run_arm(root, a.game, cap, a.frame, tag, out, gpu)
        logs[tag] = log
        if rc:
            print('FAIL: the driver (%s arm) exited %d\n%s' % (tag, rc, log[-1500:]))
            return 1
    # 1. the verify lines (GPU arm)
    verify = re.findall(r'\[ring_gpu\] key (\d+) inst (\d+) meshes (\d+) verts (\d+) nodes (\d+) max_err ([0-9.eE+-]+)( CUT_FAILED)?', logs['gpu'])
    keys = {}
    for k, inst, meshes, verts, nodes, err, cutf in verify:
        keys[int(k)] = max(keys.get(int(k), 0.0), float(err))
        if cutf:
            print('FAIL: key %s: the chunk cut failed' % k)
            bad += 1
    print('GPU formula vs CPU pose on the record\'s rows: ' + (', '.join('key %d max %.2e' % (k, v) for k, v in sorted(keys.items())) or '(no verify lines)'))
    if not keys:
        print('FAIL: no [ring_gpu] verify line: the GPU branch did not run')
        bad += 1
    for k, v in keys.items():
        if v >= 1e-3:
            print('FAIL: key %d GPU / CPU positions differ by %.3e' % (k, v))
            bad += 1
    # S103 RGP review F2: two instances of one model with one draw key blend into each other (the kept lists' census counts them). The panels hold other duplicate-key draws that are not the ring (a baseline the CPU arm
    # shows as well), so the GPU arm must not report MORE duplicate keys than the CPU arm in any panel (the unit's pose.instance_keys covers the high / low fallback this capture's arena does not have).
    dk = {t: [int(x) for x in re.findall(r'\[scene\]\[lists\].*?duplicate keys (\d+)', logs[t])] for t in logs}
    if not dk['gpu'] or len(dk['gpu']) != len(dk['cpu']):
        print('FAIL: no (or unequal) [scene][lists] census lines: the duplicate-key check did not run (gpu %d, cpu %d)' % (len(dk['gpu']), len(dk['cpu'])))
        bad += 1
    else:
        more = [(i, g, c) for i, (g, c) in enumerate(zip(dk['gpu'], dk['cpu'])) if g > c]
        print('duplicate draw keys per panel (GPU arm vs the CPU arm baseline): ' + ('%d panels, none above the baseline' % len(dk['gpu']) if not more else 'ABOVE: %s' % more))
        if more:
            print('FAIL: the GPU-skinned ring reports duplicate draw keys the CPU arm does not: two ring instances share one key')
            bad += 1
    if '[ring_gpu]' in logs['cpu']:
        print('FAIL: the CPU arm took the GPU branch')
        bad += 1
    # 3. reports
    for tag in ('gpu', 'cpu'):
        m = re.search(r'panel ring crc.*?\n\[c\]\s+Ring: objects (\d+) drawn (\d+) skipped (\d+) why posed, classes rope=(\d+)\(\w+\) cover=(\d+)\(\w+\) turnbuckle=(\d+)\(\w+\)\s*post=(\d+)\(\w+\)', logs[tag], re.S)
        if not m:
            print('FAIL: (%s arm) the ring panel did not report a posed ring' % tag)
            bad += 1
            continue
        drawn, skipped, cls = int(m.group(2)), int(m.group(3)), tuple(int(m.group(i)) for i in (4, 5, 6, 7))
        print('%s arm: posed ring classes rope/cover/turnbuckle/post %s, drawn %d skipped %d' % (tag, cls, drawn, skipped))
        if cls != (12, 12, 12, 4) or skipped:
            print('FAIL: (%s arm) not every class was drawn from its rows' % tag)
            bad += 1
    # 4. posed CPU writes
    pc = {t: int(re.search(r'posed_model_calls (\d+)', logs[t]).group(1)) if re.search(r'posed_model_calls (\d+)', logs[t]) else -1 for t in logs}
    print('posed-model calls (CPU-posed slots built / re-posed): GPU arm %d, CPU arm %d' % (pc['gpu'], pc['cpu']))
    if pc['gpu'] != 0:
        print('FAIL: the GPU arm still CPU-poses ring pieces')
        bad += 1
    if pc['cpu'] <= 0:
        print('FAIL: the CPU arm posed nothing (the A/B arm is not the CPU path)')
        bad += 1
    # 2. pictures
    png = lambda tag, n: os.path.join(out, '%s_%s.png' % (tag, n))
    for n in PANELS:
        if not (os.path.isfile(png('gpu', n)) and os.path.isfile(png('cpu', n))):
            print('note: panel %s is not in both arms' % n)
            continue
        d = diff_fraction(png('gpu', n), png('cpu', n))
        print('panel %-12s GPU vs CPU arm: %.4f %% of the pixels differ' % (n, d * 100))
        if d > 0.0005:
            print('FAIL: panel %s: the GPU-skinned ring is not the CPU-posed ring (> 0.05 %% of the pixels)' % n)
            bad += 1
    # 5. magenta / validation
    for tag in ('gpu', 'cpu'):
        for mm in re.finditer(r'panel (\w+) crc \w+ drawn_px \d+ magenta_px (\d+) validation (\d+)', logs[tag]):
            if mm.group(1).startswith('ring') and (int(mm.group(2)) or int(mm.group(3))):
                print('FAIL: (%s arm) panel %s has magenta %s / validation errors %s' % (tag, mm.group(1), mm.group(2), mm.group(3)))
                bad += 1
    # 6. the planted twin still differs on the GPU arm
    if os.path.isfile(png('gpu', 'ring_nopose')) and os.path.isfile(png('gpu', 'ring')):
        d = diff_fraction(png('gpu', 'ring_nopose'), png('gpu', 'ring'))
        print('GPU arm: planted no_pose vs posed %.3f %%' % (d * 100))
        if d < 0.012:
            print('FAIL: the planted identity palette does not change the GPU-skinned picture (the rows do not reach the shader)')
            bad += 1
    print('RESULT', 'OK' if bad == 0 else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
