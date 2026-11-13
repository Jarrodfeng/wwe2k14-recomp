#!/usr/bin/env python3
"""Lane 67 offline proof (0 game runs): particle runs drawn as ONE draw each (rplay --particles-ours), PARTICLES_PROMOTION_MAP.md section 6.

  py -I tools\\particles\\runs_check.py [--root DIR] [--no-build] [--step N] [--keep]

Bar (each line PASS / FAIL; a missing input FAILS, never skips):
  1. Check B (Brock, 341 frames) and Bryan's entrance (91 frames), switch off vs on, EVERY frame (step 1): the presented picture's max channel difference <= 1 level,
     validation 0 (rplay exit code), no missing shader.
  2. Census (the shipped binder's own line): Check B draws 91,724 / runs 1,495, Bryan 2,520 / 91 (lane 65's numbers, tools\\particles\\expect.json); merged draw calls + lone runs
     == runs; merged + lone + soft + third + other draws == draws (kept = the lone runs only); particles drawn == draws folded; blend keyed 0.
  3. Timing (rplay --timing, the farm's clock): median bind_us at frames 210-240 on Check B, off vs on (printed; on must be lower); GPU unchanged is printed, not asserted.
  4. Planted faults (--particles-fault), Check B frames 216 / 224 / 240 / 252: each must fail its exact check; all but tween_runs / float_key / zoffset_masked must also move
     the picture (max diff > 1 level) from the clean arm. tween_runs and float_key are census-only (stated); zoffset_masked is proved by the unit test below (the captures have
     c200.x = 0 everywhere, so the plant is a doctored real frame).
  5. tools\\particles\\runs_test.cpp (the shipped key and packer on a real frame of Check B, with doctored twins): all PASS.
Frames are rplay replays of the case zip's capture (extracted next to out\\ once). Every process has a timeout."""
import argparse
import csv
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
BROCK = 'out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip'
BRYAN = 'out/s101/runs/20261111_174549_s100_owner_bryan_entrance/case_20261111_174607_799_0_pc_run_exit.zip'
EXPECT = {'brock': (341, 91724, 1495), 'bryan': (91, 2520, 91)}
PLANT_FRAMES = (216, 224, 240, 252)
# plant -> (what must differ in the census, whether the picture must move)
PLANTS = {
    'tween_runs': ('blend_keyed', False),
    'first_texture': (None, True),
    'float_key': ('runs', False),
    'world_first': (None, True),
    'drop_last': ('particles', True),
    'strip_as_list': (None, True),
    'reorder': (None, True),
    'read_fail': ('scans', False),   # F1: every run stays the stream's after a failed read: scanned once, each member counted once, picture == the stream's
}

CENSUS = re.compile(r'frames (\d+), draws (\d+), runs (\d+) \(merged draws (\d+), folding (\d+), particles (\d+)\), kept (\d+) \(lone (\d+), op between (\d+), soft (\d+), '
                    r'third method (\d+), key differs (\d+), other (\d+)\), scans (\d+), blend keyed (\d+), ms ([\d.]+)')
FIELDS = ('frames', 'draws', 'runs', 'merged', 'folding', 'particles', 'kept', 'lone', 'op_between', 'soft', 'third', 'key_differs', 'other', 'scans', 'blend_keyed', 'ms')
failures = 0


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def say(ok, msg):
    global failures
    if not ok:
        failures += 1
    print(('PASS  ' if ok else 'FAIL  ') + msg)
    sys.stdout.flush()


def run(cmd, timeout=900):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def extract(main, rel, dest):
    if os.path.isfile(os.path.join(dest, 'images.bin')):
        return dest
    zpath = os.path.join(main, rel)
    if not os.path.isfile(zpath):
        return None
    os.makedirs(dest, exist_ok=True)
    with zipfile.ZipFile(zpath) as z:
        for name in z.namelist():
            if name.startswith('capture/') and not name.endswith('/'):
                target = os.path.join(dest, name[len('capture/'):])
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with z.open(name) as src, open(target, 'wb') as out:
                    shutil.copyfileobj(src, out)
    return dest if os.path.isfile(os.path.join(dest, 'images.bin')) else None


def rplay(exe, cap, frames, extra, shots, shot_dir, timing=None):
    cmd = [exe, '--capture', cap, '--frames', str(frames), '--validation', '1'] + extra
    if shots:
        os.makedirs(shot_dir, exist_ok=True)
        cmd += ['--shots', ','.join(str(s) for s in shots), '--shot-dir', shot_dir]
    if timing:
        cmd += ['--timing', timing]
    for attempt in range(6):  # the GPU is shared with other lanes: only "Not enough graphics memory" is retried (common.md)
        code, text = run(cmd)
        if 'Not enough graphics memory' not in text:
            break
        print('      (rplay: not enough graphics memory, retry %d)' % (attempt + 1))
        time.sleep(10)
    if code != 0 and not CENSUS.search(text):
        print('      rplay exit %d: %s' % (code, ' | '.join(text.strip().splitlines()[-3:])))
    m = CENSUS.search(text)
    census = dict(zip(FIELDS, (float(x) if k == 'ms' else int(x) for k, x in zip(FIELDS, m.groups())))) if m else None
    return code, text, census


def pics(np, Image, a_dir, b_dir, frames):
    worst, mean_worst, bad = 0, 0.0, []
    for f in frames:
        for d in (a_dir, b_dir):
            if not os.path.isfile(os.path.join(d, 'frame_%04d.png' % f)):
                raise SystemExit('FAIL  rplay wrote no picture ' + os.path.join(d, 'frame_%04d.png' % f) + ' (a failed rplay start? see the run output)')
        a = np.asarray(Image.open(os.path.join(a_dir, 'frame_%04d.png' % f)).convert('RGB')).astype(int)
        b = np.asarray(Image.open(os.path.join(b_dir, 'frame_%04d.png' % f)).convert('RGB')).astype(int)
        d = np.abs(a - b)
        m = int(d.max())
        if m > worst:
            worst = m
        mean_worst = max(mean_worst, float(d.mean()))
        if m > 1:
            bad.append(f)
    return worst, mean_worst, bad


def median_bind(path, lo, hi):
    with open(path) as f:
        rows = [r for r in csv.DictReader(f) if lo <= int(r['frame']) <= hi]
    if not rows:
        return None
    return statistics.median(float(r['bind_us']) for r in rows), statistics.median(float(r['gpu_us']) for r in rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--step', type=int, default=1)
    ap.add_argument('--keep', action='store_true')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main = main_checkout(root)
    try:
        import numpy as np
        from PIL import Image
    except ImportError as e:
        print('FAIL  numpy / PIL missing: %s' % e)
        return 1
    work = os.path.join(root, 'out', 'particles_ours')
    os.makedirs(work, exist_ok=True)
    if not args.no_build:
        code, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'renderer', 'build.ps1')], 1800)
        if code != 0:
            print(text[-2000:])
            print('FAIL  the renderer build failed')
            return 1
    exe = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    if not os.path.isfile(exe):
        print('FAIL  %s is missing' % exe)
        return 1
    caps = {}
    for name, rel in (('brock', BROCK), ('bryan', BRYAN)):
        caps[name] = extract(main, rel, os.path.join(work, name))
        if not caps[name]:
            print('FAIL  cannot read the case zip %s (or extract its capture\\)' % rel)
            return 1

    # 5. the unit test (built here, the shipped header)
    test_exe = os.path.join(work, 'runs_test.exe')
    code, text = run(['C:\\Program Files\\LLVM\\bin\\clang++.exe', '-std=c++20', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-o', test_exe, os.path.join(root, 'tools', 'particles', 'runs_test.cpp')], 600)
    if code != 0:
        print(text[-1500:])
        print('FAIL  runs_test.cpp does not build')
        return 1
    code, text = run([test_exe, caps['brock'], '224'], 300)
    for line in text.strip().splitlines():
        if line.startswith('FAIL'):
            print(line)
    say(code == 0 and 'PASS:' in text, '5. runs_test (key, doctored twins zoffset_masked / float_key, packer and its plants): ' + (text.strip().splitlines() or ['no output'])[-1])

    clean = {}
    for name in ('brock', 'bryan'):
        frames, want_draws, want_runs = EXPECT[name]
        shots = list(range(0, frames, args.step))
        a_dir, b_dir = os.path.join(work, name + '_a'), os.path.join(work, name + '_b')
        for d in (a_dir, b_dir):
            shutil.rmtree(d, ignore_errors=True)
        ta, tb = os.path.join(work, name + '_t_off.csv'), os.path.join(work, name + '_t_on.csv')
        code_a, text_a, _ = rplay(exe, caps[name], frames, [], shots, a_dir, ta)
        code_b, text_b, census = rplay(exe, caps[name], frames, ['--particles-ours'], shots, b_dir, tb)
        say(code_a == 0 and code_b == 0, '1. %s: validation 0 off and on (exit codes %d / %d)' % (name, code_a, code_b))
        say('missing shader' not in text_b.replace('0 missing a shader', ''), '1. %s: no missing shader' % name)
        if not census:
            say(False, '2. %s: no particles census line in rplay\'s output' % name)
            continue
        worst, mean_worst, bad = pics(np, Image, a_dir, b_dir, shots)
        say(worst <= 1 and not bad, '1. %s: %d frames, picture max difference %d levels (worst frame mean %.4f), frames over 1 level: %s' % (name, len(shots), worst, mean_worst, bad[:8]))
        c = census
        say(c['draws'] == want_draws and c['runs'] == want_runs, '2. %s: draws %d (want %d), runs %d (want %d)' % (name, c['draws'], want_draws, c['runs'], want_runs))
        say(c['frames'] > 0 and c['draws'] == c['folding'] + c['lone'] + c['soft'] + c['third'] + c['other'], '2. %s: draws == folded + lone + soft + third + other (kept = %d, all lone: %s)' % (name, c['kept'], c['kept'] == c['lone']))
        say(c['runs'] == c['merged'] + c['lone'] and c['kept'] == c['lone'], '2. %s: runs %d == merged draw calls %d + lone %d' % (name, c['runs'], c['merged'], c['lone']))
        say(c['scans'] == c['runs'], '2. %s: run scans %d == runs %d (each run scanned once)' % (name, c['scans'], c['runs']))
        say(c['particles'] == c['folding'] and c['blend_keyed'] == 0, '2. %s: particles drawn %d == draws folded %d; blend keyed %d' % (name, c['particles'], c['folding'], c['blend_keyed']))
        if name == 'brock':
            clean = census
            # The GPU is shared with other lanes: three interleaved timing runs (frames 0-240), the best median of each arm.
            offs, ons = [median_bind(ta, 210, 240)], [median_bind(tb, 210, 240)]
            for rep in range(2):
                for extra, store, tag in (([], offs, 'off'), (['--particles-ours'], ons, 'on')):
                    tpath = os.path.join(work, 'brock_t_%s_%d.csv' % (tag, rep))
                    rplay(exe, caps[name], 241, extra, [], '', tpath)
                    store.append(median_bind(tpath, 210, 240))
            offs, ons = [x for x in offs if x], [x for x in ons if x]
            if offs and ons:
                off, on = min(offs), min(ons)
                say(on[0] < off[0], '3. brock frames 210-240 best-of-3 median bind_us off %.0f -> on %.0f (%.0f%%); gpu_us %.0f -> %.0f (printed)' % (off[0], on[0], 100.0 * on[0] / off[0], min(x[1] for x in offs), min(x[1] for x in ons)))
            else:
                say(False, '3. no timing rows for frames 210-240')
            # 4. the plants, against the clean arm's pictures
            for plant, (field, moves) in PLANTS.items():
                p_dir = os.path.join(work, 'plant_' + plant)
                shutil.rmtree(p_dir, ignore_errors=True)
                code_p, text_p, cp = rplay(exe, caps['brock'], max(PLANT_FRAMES) + 1, ['--particles-ours', '--particles-fault', plant], list(PLANT_FRAMES), p_dir)
                if not cp:
                    say(False, '4. %s: no census line (rplay exit %d)' % (plant, code_p))
                    continue
                worst_p, mean_p, _ = pics(np, Image, a_dir, p_dir, PLANT_FRAMES) if args.step == 1 else pics(np, Image, a_dir, p_dir, [f for f in PLANT_FRAMES if f % args.step == 0])
                census_ok = True
                if field == 'blend_keyed':
                    census_ok = cp['blend_keyed'] > 0
                elif field == 'runs':
                    census_ok = cp['runs'] != 1495
                elif field == 'scans':
                    census_ok = (cp['scans'] == cp['runs'] and cp['merged'] == 0 and cp['other'] == cp['draws'] - cp['lone'] and cp['particles'] == 0 and cp['runs'] > 0)
                    pic_ok_extra = worst_p <= 1
                    census_ok = census_ok and pic_ok_extra
                elif field == 'particles':
                    census_ok = cp['particles'] != cp['folding']
                pic_ok = (worst_p > 1) if moves else True
                note = 'census-only (the picture is not expected to move)' if not moves else 'picture moved by %d levels (mean %.3f)' % (worst_p, mean_p)
                say(census_ok and pic_ok, '4. plant %s fails its check: census %s=%s (clean %s); %s' % (plant, field or '-', cp.get(field, '-') if field else '-', clean.get(field, '-') if field else '-', note))
            # zoffset_masked on the real capture: same census as clean (c200.x = 0 everywhere); the plant itself is proved by runs_test (doctored frame)
            code_z, text_z, cz = rplay(exe, caps['brock'], 20, ['--particles-ours', '--particles-fault', 'zoffset_masked'], [], '')
            say(code_z == 0 and cz is not None, '4. plant zoffset_masked runs (census-neutral on the captures, c200.x = 0 in every draw; failing arm = runs_test 2b)')
        if not args.keep:
            for d in (a_dir, b_dir):
                shutil.rmtree(d, ignore_errors=True)
    if not args.keep:
        for plant in PLANTS:
            shutil.rmtree(os.path.join(work, 'plant_' + plant), ignore_errors=True)
    print('%s: %d failure(s)' % ('FAIL' if failures else 'PASS', failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
