#!/usr/bin/env python3
"""S103 lane RLB2 check: the game's low-detail ring pieces (980 rope, 981 post, 982 cover) are loaded, bound and drawn by the posed ring
(docs\\research\\s103\\RLL.md fix sites; load switch assets::load_detail::RingLowKeys, src\\assets\\load_plants.h, set by live_assets.cpp and mirrored by tools\\scene\\c_driver.cpp).
Headless with lane C's driver on run 16's frame 399 record (the posed ring: ring_posed_check.py), 0 game runs. Three driver runs of the same capture:
  ARM LOW  = the live load (low keys loaded), GPU skin (default);   ARM CPU = the same with the CPU pose (WWE_RDR_RING_GPU_SKIN=0);   ARM OFF = the planted twin (WWE_C_RING_LOW=0: the old loader, no 980 / 981 / 982).
The checks:
  1. bind: `[C4] ring key K: N meshes, bound 1` with N > 0 for K = 980, 981, 982 in ARM LOW (a key that does not bind is skipped by the posed ring: rf_ring.cpp radius < 0);
  2. the planted twin ARM OFF has no such line for 980 / 981 / 982 (the switch is what loads them) and its census has no key 980;
  3. panel `ring_gamerope` (the ropes from the GAME's own rows, ours dropped: class kRcRopeLo = the game's LOD choice): the census `[ring_keys]` shows key 980 drawn in ARM LOW (both skins) and none in ARM OFF
     (the low ropes fell back to 960 there); the vertices drawn by rope key before (OFF) / after (LOW) are printed, and the low model is smaller;
  4. panel `ring` (ours: rope_hi class draws 960): key 980 is NOT drawn there (review finding 5: a high rope must not take the low model);
  5. the ring panels of ARM LOW are valid (no magenta / validation errors).
Reads the case zip READ ONLY (one scene record extracted to out\\s103\\rlb2); the game files from the main checkout READ ONLY.
  py -I tools\\scene\\ring_low_check.py [--root DIR] [--game DIR] [--zip PATH] [--frame N] [--no-build]"""
import argparse
import os
import re
import subprocess
import sys
import zipfile

ZIP = r'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\out\cases\case_20261116_023239_766_0_pc_run_exit.zip'


RAW = {}   # tag -> the driver's output as printed (the verify lines stay)


def run_arm(root, game, cap, frame, tag, outdir, low, gpu):
    env = dict(os.environ, WWE_RDR_VALIDATION='1', WWE_RDR_C4_LOG='1', WWE_RDR_RING_KEYS='1', WWE_RDR_RING_GPU_SKIN='1' if gpu else '0')
    if gpu:
        env['WWE_RDR_RING_GPU_VERIFY'] = '1'
    if not low:
        env['WWE_C_RING_LOW'] = '0'
    p = subprocess.run([os.path.join(root, 'out', 'tools', 'c_driver.exe'), '--game', game, '--capture', cap, '--frame', str(frame), '--out', outdir, '--tag', tag, '--cam', '1100,500,0,45'],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env, timeout=900)
    # one stream; the dev pose lines of stderr land inside a half-written stdout line: cut them out so the panel / report lines are whole again
    RAW[tag] = p.stdout
    log = re.sub(r'\[(?:C4\] key \d+ inst|ring_gpu\] key \d+ inst|rdr\]) [^\n]*\n', '', p.stdout)
    with open(os.path.join(outdir, tag + '_log.txt'), 'w', encoding='utf-8') as fh:
        fh.write(log)
    return p.returncode, log


def census_by_panel(log):
    """panel name -> {key: (instances, vertices)} from the `ring_keys:` census rf_ring.cpp appends to each panel's Ring report line (WWE_RDR_RING_KEYS)."""
    out, name = {}, None
    for line in log.splitlines():
        m = re.match(r'\[c\] panel (\w+) crc ', line)
        if m:
            name = m.group(1)
            continue
        m = re.match(r'\[c\]   Ring: .*ring_keys:(.*)$', line)   # rf_ring.cpp appends the census to the ring report (WWE_RDR_RING_KEYS)
        if m and name:
            out[name] = {int(k): (int(n), int(v)) for k, n, v in re.findall(r'key (\d+) x(\d+) verts (\d+);', m.group(1))}
    return out


def report_lines(log):
    """panel name -> its Ring report line (the one that carries the census)."""
    out, name = {}, None
    for line in log.splitlines():
        m = re.match(r'\[c\] panel (\w+) crc ', line)
        if m:
            name = m.group(1)
        elif name and line.startswith('[c]   Ring: ') and 'ring_sets:' in line:
            out[name] = line
    return out


def bounds_match(a, b, tol=0.5):
    """every box of a has an unmatched box of b within tol on all six numbers (greedy: the instance order of ours and the game's rows differs)."""
    left = list(b)
    for x in a:
        for i, y in enumerate(left):
            if all(abs(p - q) <= tol for p, q in zip(x, y)):
                del left[i]
                break
        else:
            return False
    return not left


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
    out = os.path.join(root, 'out', 's103', 'rlb2')
    cap = os.path.join(out, 'cap')
    os.makedirs(os.path.join(cap, 'scene'), exist_ok=True)
    name = 'frame_%06d.rsc' % a.frame
    with zipfile.ZipFile(a.zip) as z, open(os.path.join(cap, 'scene', name), 'wb') as fh:
        fh.write(z.read('capture/scene/' + name))
    logs = {}
    for tag, low, gpu in (('low', True, True), ('cpu', True, False), ('off', False, True)):
        rc, log = run_arm(root, a.game, cap, a.frame, tag, out, low, gpu)
        logs[tag] = log
        if rc:
            print('FAIL: the driver (%s arm) exited %d\n%s' % (tag, rc, log[-1500:]))
            return 1
    bad = 0
    # 1. bind
    for tag in ('low', 'cpu'):
        for k in (980, 981, 982):
            m = re.search(r'\[C4\] ring key %d: (\d+) meshes, bound (\d)' % k, logs[tag])
            print('%s arm: ring key %d: %s' % (tag, k, ('%s meshes, bound %s' % m.groups()) if m else 'NOT LOADED'))
            if not m or int(m.group(1)) <= 0 or m.group(2) != '1':
                print('FAIL: (%s arm) key %d is not loaded and bound' % (tag, k))
                bad += 1
    # 2. the planted twin
    for k in (980, 981, 982):
        if re.search(r'\[C4\] ring key %d:' % k, logs['off']):
            print('FAIL: the planted twin (switch off) still loaded key %d' % k)
            bad += 1
    # 3 / 4. the census per panel
    cen = {t: census_by_panel(logs[t]) for t in logs}
    for tag in ('low', 'cpu', 'off'):
        for pn in ('ring', 'ring_gamerope'):
            c = cen[tag].get(pn)
            print('%s arm panel %-14s rope draws by key: %s' % (tag, pn, ', '.join('%d x%d (%d verts)' % (k, n, v) for k, (n, v) in sorted(c.items()) if k in (960, 980)) if c else '(no census)'))
    for tag in ('low', 'cpu'):
        g = cen[tag].get('ring_gamerope', {})
        if g.get(980, (0, 0))[0] <= 0:
            print('FAIL: (%s arm) the game\'s low ropes did not draw key 980' % tag)
            bad += 1
        if cen[tag].get('ring', {}).get(980, (0, 0))[0] != 0:
            print('FAIL: (%s arm) panel ring (ours, high class) drew the low rope 980' % tag)
            bad += 1
    if cen['off'].get('ring_gamerope', {}).get(980, (0, 0))[0] != 0 or any(980 in v for v in cen['off'].values()):
        print('FAIL: the planted twin (switch off) drew key 980')
        bad += 1
    lo, off = cen['low'].get('ring_gamerope', {}), cen['off'].get('ring_gamerope', {})
    vl = sum(v for k, (n, v) in lo.items() if k in (960, 980))
    vo = sum(v for k, (n, v) in off.items() if k in (960, 980))
    print('rope vertices drawn on panel ring_gamerope: before (low keys not loaded) %d, after %d' % (vo, vl))
    if not vl < vo:
        print('FAIL: the low ropes did not reduce the rope vertices drawn')
        bad += 1
    # 6. (review 1) one light set per ring object: each high / low pair resolves to the same set ids, on every panel
    rl = {t: report_lines(logs[t]) for t in logs}
    for tag in ('low', 'cpu'):
        shown = False
        for pn, line in rl[tag].items():
            sets = {int(k): v for k, v in re.findall(r'(\d+)=\[([^\]]*)\]', re.search(r'ring_sets:(.*?);', line).group(1))}
            if not shown:
                print('%s arm light-set ids per mesh: ' % tag + ' '.join('%d=[%s]' % (k, v.rstrip(',')) for k, v in sorted(sets.items()) if k in (951, 960, 964, 980, 981, 982)))
                shown = True
            for hi, lo in ((960, 980), (951, 981), (964, 982)):
                if hi in sets and lo in sets and sets[hi] != sets[lo]:
                    print('FAIL: (%s arm, panel %s) the high / low pair %d / %d lights with different sets: %s vs %s' % (tag, pn, hi, lo, sets[hi], sets[lo]))
                    bad += 1
    # 7. (review 3) OUR recomputed rope rows flipped to the low class draw 980 on the GPU skin: no node outside the palette, the GPU formula = the CPU pose, and the posed bounds = the game's own 980 rows'
    for tag in ('low', 'cpu'):
        c = cen[tag].get('ring_ourslow', {})
        print('%s arm panel ring_ourslow rope draws by key: %s' % (tag, ', '.join('%d x%d (%d verts)' % (k, n, v) for k, (n, v) in sorted(c.items()) if k in (960, 980)) or '(none)'))
        if c.get(980, (0, 0))[0] != 12 or 960 in c:
            print('FAIL: (%s arm) our low ropes did not all draw key 980' % tag)
            bad += 1
        if '[scene][identity]' in logs[tag]:
            print('FAIL: (%s arm) a skin node was drawn past the palette (identity matrix): 980\'s palette indexes nodes outside the call\'s rows' % tag)
            bad += 1
    v980 = [float(m) for m in re.findall(r'\[ring_gpu\] key 980 inst \d+ meshes \d+ verts \d+ nodes \d+ max_err ([0-9.eE+-]+)', RAW['low'])]
    print('GPU formula vs CPU pose, key 980: ' + ('max %.2e (%d lines)' % (max(v980), len(v980)) if v980 else '(no line)'))
    if not v980 or max(v980) >= 1e-3 or any('CUT_FAILED' in l for l in RAW['low'].splitlines() if '[ring_gpu] key 980' in l):
        print('FAIL: no [ring_gpu] key 980 line, or its GPU formula differs from the CPU pose (the GPU-skinned 980 is not built / used)')
        bad += 1
    def lowb(pn):
        m = re.search(r'low_bounds:([^;]*);', rl['cpu'].get(pn, ''))
        return [tuple(float(x) for x in b.split('/')) for b in m.group(1).split()] if m else []
    bo, bg = lowb('ring_ourslow'), lowb('ring_gamerope')
    print('posed 980 bounds (CPU arm): ours %d boxes, the game\'s rows %d boxes, match %s' % (len(bo), len(bg), bounds_match(bo, bg) if bo and bg else 'n/a'))
    if not bo or not bg or not bounds_match(bo, bg):
        print('FAIL: our posed 980 ropes do not sit where the game\'s 980 rows put them')
        bad += 1
    # 5. validity
    for tag in ('low', 'cpu'):
        for mm in re.finditer(r'panel (\w+) crc \w+ drawn_px \d+ magenta_px (\d+) validation (\d+)', logs[tag]):
            if mm.group(1).startswith('ring') and (int(mm.group(2)) or int(mm.group(3))):
                print('FAIL: (%s arm) panel %s has magenta %s / validation errors %s' % (tag, mm.group(1), mm.group(2), mm.group(3)))
                bad += 1
    print('RESULT ' + ('OK' if not bad else 'FAIL (%d)' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
