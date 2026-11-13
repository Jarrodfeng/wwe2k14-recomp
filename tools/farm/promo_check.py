#!/usr/bin/env python3
"""Lane 47 offline proof (0 game runs): the live image-info supplier and the promotion switches in rplay / the replay farm. Check B's capture (run 7051, arena 4, 341 frames).

  py -I tools\\farm\\promo_check.py [--root DIR] [--no-build]

Bars (each line PASS / FAIL; a missing input FAILS, never skips; one GPU process of ours at a time):
 A. the live image-info supplier through fview (`--image-info`): the capture reader and the live ImageInfoTable (same records, same Set()) give the SAME arena census line and the same
    picture md5, and `size unchecked` is 0 in both; `none` (the old live behaviour) counts size unchecked > 0; the planted table (`live-fault`, every width +1) changes the answers
    (the census' `size` count). The unit twin (every id, the threads, the planted tables) is tools\\arena\\arena_ours_selftest.py (gate lane arena_ours_unit).
 B. rplay with every switch off draws what the base drew: frame 200's picture md5 == the crowd lane's pinned BASE_STREAM_MD5 (tools\\crowd\\crowd_ours_check.py), and also with
    `--game DIR` given and no switch. Each switch on (crowd / arena / wrestlers), validation 1: exit 0, no validation errors, the census says ours drew frames; the arena's
    `size unchecked` is 0 (the supplier is present); the arena's picture differs from the switch-off one (it really replaced something).
 C. loud failures: a switch without --game exits 2; a capture without scene records exits 2.
 D. the farm: `farm.py promo` on the capture (1 run, 60 frames, all three arms) exits 0 and writes GPU timestamps for every arm; the census guard (an arm whose ours drew nothing is a
    FAIL) is true for a made-up dead census and false for a live one (planted twin); `run --rplay-args "--arena-ours --game ..."` labels its run an arm, and `baseline` refuses it."""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CHECK_B = '20261111_215201_s100_owner_s89_brock'   # the corpus name of Check B's case zip


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout=900):
    # a start failure of the GPU programs under load by other lanes: retry that exact message only
    for attempt in range(6):
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, errors='replace')
        text = (p.stdout or '') + (p.stderr or '')
        if p.returncode == 2 and 'Not enough graphics memory' in text:
            import time
            time.sleep(10 + 10 * attempt)
            continue
        return p.returncode, text
    return p.returncode, text


def md5(path):
    with open(path, 'rb') as f:
        return hashlib.md5(f.read()).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    runs = os.path.join(main_co, 'out', 's101', 'runs', '20261111_215201_s100_owner_s89_brock')
    zips = [os.path.join(runs, f) for f in os.listdir(runs) if f.startswith('case_') and f.endswith('.zip')] if os.path.isdir(runs) else []
    fails = 0

    def verdict(ok, text):
        nonlocal fails
        print(('PASS' if ok else 'FAIL') + '  ' + text, flush=True)
        fails += not ok

    if not zips:
        print("FAIL  input missing: Check B's case zip under " + runs)
        return 1
    if not os.path.isdir(game):
        print('FAIL  input missing: the game files ' + game)
        return 1
    no_scene = os.path.join(main_co, 'out', 'captures', 'brock_s15')
    if not os.path.isfile(os.path.join(no_scene, 'images.bin')):
        print('FAIL  input missing: the capture without scene records ' + no_scene)
        return 1
    pin_file = os.path.join(root, 'tools', 'crowd', 'crowd_ours_check.py')
    m = re.search(r"BASE_STREAM_MD5\s*=\s*'([0-9a-f]{32})'", open(pin_file, encoding='utf-8').read()) if os.path.isfile(pin_file) else None
    if not m:
        print('FAIL  cannot read BASE_STREAM_MD5 from ' + pin_file)
        return 1
    base_md5 = m.group(1)
    out = os.path.join(root, 'out', 's101', 'promo_farm')
    cap = os.path.join(out, 'case_brock', 'capture')
    if not os.path.isfile(os.path.join(cap, 'images.bin')):
        os.makedirs(os.path.join(out, 'case_brock'), exist_ok=True)
        zipfile.ZipFile(zips[0]).extractall(os.path.join(out, 'case_brock'))
    if not os.path.isfile(os.path.join(cap, 'images.bin')) or not os.path.isdir(os.path.join(cap, 'scene')):
        print('FAIL  the capture did not unpack with its scene records: ' + cap)
        return 1
    if not args.no_build:
        for script in ('renderer', 'fview'):
            rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', script, 'build.ps1')] + (['-NoShaders'] if script == 'renderer' else []), 1800)
            if rc:
                print(text[-3000:])
                print('FAIL  %s build (exit %d)' % (script, rc))
                return 1
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    fview = _fe.find_or_exit(root=root, kind='dev')
    rplay = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    for exe in (fview, rplay):
        if not os.path.isfile(exe):
            print('FAIL  not built: ' + exe)
            return 1

    # ---- A. the live supplier through fview ----
    arena_line = re.compile(r'\[rdr\]\[arena\] ours frames .*')
    class_line = re.compile(r'\[rdr\]\[arena\] ours frames \d+, not ready.*')
    size_re = re.compile(r'size (\d+) \(size unchecked (\d+)\)')

    def fview_arm(mode, frame=120):
        d = os.path.join(out, 'fview_' + mode)
        rc, text = run([fview, '--capture', cap, '--frame', str(frame), '--judge-out', d, '--root', root, '--arena-ours', '--game', game, '--image-info', mode])
        png = os.path.join(d, 'picture.png')
        census = [re.sub(r', ms [0-9.]+', '', l) for l in text.splitlines() if l.startswith('[rdr][arena] ours frames') and 'replaced' in l]   # (the census' timing differs run to run)
        cls = [l for l in text.splitlines() if 'not ready' in l and 'size unchecked' in l]
        sz = size_re.search(cls[-1]) if cls else None
        return rc, (md5(png) if os.path.isfile(png) else None), (census[-1] if census else None), (cls[-1] if cls else None), (int(sz.group(1)), int(sz.group(2))) if sz else None, text

    arms = {}
    for mode in ('capture', 'live', 'live-fault', 'none'):
        arms[mode] = fview_arm(mode)
        if arms[mode][0] != 0 or arms[mode][1] is None or arms[mode][2] is None or arms[mode][4] is None:
            print(arms[mode][5][-2000:])
            verdict(False, 'fview arm --image-info %s did not run clean (exit %d)' % (mode, arms[mode][0]))
            return 1
    cap_a, live_a, fault_a, none_a = arms['capture'], arms['live'], arms['live-fault'], arms['none']
    verdict(cap_a[4][1] == 0 and live_a[4][1] == 0, 'size unchecked is 0 with the supplier: capture reader %s, live table %s' % (cap_a[4], live_a[4]))
    verdict(cap_a[1] == live_a[1] and cap_a[2] == live_a[2] and cap_a[3] == live_a[3], 'live supplier == capture reader: picture md5 %s / %s, census equal %s, class report equal %s' % (cap_a[1], live_a[1], cap_a[2] == live_a[2], cap_a[3] == live_a[3]))
    verdict(none_a[4][1] > 0, 'no supplier (the old live behaviour) counts size unchecked %d (> 0): the planted twin of the 0 above' % none_a[4][1])
    verdict(fault_a[4] != cap_a[4] or fault_a[1] != cap_a[1] or fault_a[2] != cap_a[2], 'a wrong table (every width +1) changes the answers: size/unchecked %s vs %s, census equal %s, picture equal %s' % (fault_a[4], cap_a[4], fault_a[2] == cap_a[2], fault_a[1] == cap_a[1]))

    # ---- B. rplay ----
    def rplay_run(name, extra, frames, validation=1, shots=None):
        d = os.path.join(out, 'rplay_' + name)
        os.makedirs(d, exist_ok=True)
        cmd = [rplay, '--capture', cap, '--frames', str(frames), '--validation', str(validation)] + extra
        if shots is not None:
            cmd += ['--shot-dir', d, '--shots', str(shots)]
        rc, text = run(cmd)
        return rc, text, (os.path.join(d, 'frame_%04d.png' % shots) if shots is not None else None)

    rc, text, off_png = rplay_run('off', [], 201, 0, 200)
    verdict(rc == 0 and off_png and os.path.isfile(off_png) and md5(off_png) == base_md5, 'rplay, every switch off: frame 200 md5 %s == the pinned base %s' % (md5(off_png) if off_png and os.path.isfile(off_png) else None, base_md5))
    rc, text, game_png = rplay_run('off_game', ['--game', game], 201, 0, 200)
    verdict(rc == 0 and game_png and os.path.isfile(game_png) and md5(game_png) == base_md5, 'rplay, --game given and no switch: same md5 (nothing is created)')
    census = {}
    for name, sw, frames in (('crowd', '--crowd-ours', 60), ('arena', '--arena-ours', 60), ('wrestlers', '--wrestlers-ours', 60)):
        rc, text, _ = rplay_run(name, [sw, '--game', game], frames)
        mo = re.search(r'\[rdr\]\[%s\] ours frames (\d+), fallback (\d+)' % name, text)
        ok = rc == 0 and mo is not None and int(mo.group(1)) > 0
        verdict(ok, 'rplay %s, validation 1: exit %d, census %s' % (sw, rc, mo.group(0) if mo else 'MISSING'))
        census[name] = text
    sz = size_re.search(census['arena'])
    verdict(sz is not None and int(sz.group(2)) == 0, 'rplay arena: the supplier is present, size unchecked %s' % (sz.group(2) if sz else 'MISSING'))
    rc, text, arena_png = rplay_run('arena_pic', ['--arena-ours', '--game', game], 201, 0, 200)
    verdict(rc == 0 and arena_png and os.path.isfile(arena_png) and md5(arena_png) != base_md5, 'rplay --arena-ours: frame 200 differs from the switch-off picture (it replaced the stage)')

    # ---- C. loud failures ----
    rc, text = run([rplay, '--capture', cap, '--frames', '4', '--arena-ours'])
    verdict(rc == 2 and '--game' in text, 'a switch without --game exits 2 (exit %d)' % rc)
    rc, text = run([rplay, '--capture', no_scene, '--frames', '4', '--crowd-ours', '--game', game])
    verdict(rc == 2 and 'scene records' in text, 'a capture without scene records exits 2 (exit %d)' % rc)

    # ---- D. the farm ----
    sys.path.insert(0, os.path.join(root, 'tools', 'farm'))
    os.environ['RDR_MAIN_CHECKOUT'] = main_co
    import importlib
    farm = importlib.import_module('farm')
    verdict(farm.dead_classes('arena', {'arena': {'ours_frames': 0, 'fallback_frames': 9}}) == ['arena'] and farm.dead_classes('arena', {}) == ['arena']
            and farm.dead_classes('arena', {'arena': {'ours_frames': 5, 'fallback_frames': 0}}) == [], 'the dead-arm guard: ours drew 0 frames / no census -> dead; ours drew 5 -> alive')
    pdir = os.path.join(out, 'farm_promo')
    rc, text = run([sys.executable, '-I', os.path.join(root, 'tools', 'farm', 'farm.py'), 'promo', '--build', rplay, '--only', CHECK_B, '--runs', '1', '--warm', '0', '--frames', '60', '--out', pdir,
                    '--game', game, '--arms', 'crowd,arena,wrestlers'], 3000)
    print(text[-2500:])
    pj = os.path.join(pdir, 'promo.json')
    good = rc == 0 and os.path.isfile(pj)
    if good:
        data = json.load(open(pj))
        caps = data['captures']
        good = bool(caps) and all(e['status'] == 'ok' and all(e['arms'][a].get('gpu_ts_ms') for a in ('off', 'crowd', 'arena', 'wrestlers')) for e in caps.values())
    verdict(good, 'farm promo: exit %d, every arm has GPU timestamps (median and p99 in promo.json)' % rc)
    # a promotion arm through `run` is labelled, and is never a baseline
    rdir = os.path.join(out, 'farm_arm_run')
    rc, text = run([sys.executable, '-I', os.path.join(root, 'tools', 'farm', 'farm.py'), 'run', '--build', rplay, '--only', CHECK_B, '--runs', '1', '--warm', '0', '--out', rdir,
                    '--rplay-args', '--arena-ours --game ' + game, '--timeout', '1500'], 3000)
    meta_ok = rc == 0 and os.path.isfile(os.path.join(rdir, 'run.json')) and json.load(open(os.path.join(rdir, 'run.json'))).get('arm') == ['--arena-ours']
    verdict(meta_ok, 'farm run --rplay-args "--arena-ours --game ..." is an arm (run.json arm = ["--arena-ours"]), exit %d' % rc)
    rc, text = run([sys.executable, '-I', os.path.join(root, 'tools', 'farm', 'farm.py'), 'baseline', '--from', rdir])
    verdict(rc == 1 and 'promotion arm' in text, 'baseline refuses an arm run (exit %d)' % rc)
    print('RESULT ' + ('OK' if not fails else 'FAIL %d' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
