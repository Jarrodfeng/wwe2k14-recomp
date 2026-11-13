r"""Lane 155 (S101) acceptance test of the crowd card bake's invisible cut, offline, 0 game runs (fview and the shipped decision logic).

  py -I tools\crowd\bake_skip_check.py [--root DIR] [--no-replay] [--no-fview]

Material (the MAIN checkout's out\ and game_data\): Check B (run 20261111_215201, 341 frames, cards in every frame), wm30 (run 20261112_131029, 668 frames, cards only in frames 0-51, then none).
Steps:
  1. counts   tools\crowd\bake_skip_counts.py: the card count of each person type in each frame (counts only) -> out\crowd_bake_skip\LABEL.csv
  2. replay   out\tools\bake_skip_test.exe --counts (the SHIPPED bake_skip_core.h policy run over those sequences, census mode, the game's order: bake pass, then the crowd draw's card loops)
              bars: wm30 cuts >= 8,000 of 13,360 thumbnail iterations (60 %+), Check B cuts some (>= 20); on both: reads over the age bound 0, the oldest picture a card reads <= 4 bake passes, guest memory pristine.
  3. fview    the cut EMULATED on the capture (tools\crowd\bake_skip_cut.py removes the ops of every cut bake pass: SetTargets, Clear, Draw, Copy) and replayed (fview --replay --hash-frames), against the uncut replay:
              wm30: the presented surface of ALL 668 frames byte-equal (9,170 passes cut, 621 frames touched); Check B: at most 4 of 341 frames differ, each only where a returning type read an aged picture,
              and each within 0.5 % of its pixels / 20 levels (PNG of those frames, tools\crowd\png_diff.py).
  4. plant    cut_read (the bake of a type whose cards ARE drawn is elided every frame, a copy elided but the picture read: the stale-picture class): must differ in > 100 frames of Check B.
Exit 0 = every bar holds."""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
BROCK = 'out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip'
WM30 = 'out/s101/runs/20261112_131029_s100_owner_s90_wm30/case_20261112_131111_940_0_pc_run_exit.zip'
J1 = 'out/s101/runs/20261112_172304_s100_owner_s89_brock/case_20261112_172341_872_0_pc_run_exit.zip'
MATCH15 = 'out/s101/runs/20261112_131008_s100_owner_match/case_20261112_131027_265_0_pc_run_exit.zip'   # a 15-type arena: 15 bakes, 5 null slots, no cards
FAILS = 0


def line(ok, text):
    global FAILS
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    FAILS += 0 if ok else 1


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout=1800, env=None):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, errors='replace', env=env)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def replay(exe, mode, csv, extra=()):
    rc, out = run([exe, '--counts', csv, '--mode', mode] + list(extra))
    m = re.search(r'baked (\d+) \(cards (\d+), warmup (\d+), refresh (\d+), anchor (\d+)\), skipped (\d+) \(([\d.]+)% of the iterations\), game skips itself (\d+)', out)
    r = re.search(r'reads: (\d+) card loops .*?0:(\d+) 1:(\d+) 2:(\d+) 3:(\d+) 4:(\d+) 5\+:(\d+), max (\d+), over the bound (\d+)', out)
    g = re.search(r'edits (\d+) restores (\d+) restore_skipped (\d+) memory pristine (\d)', out)
    if rc or not (m and r and g):
        print(out[-1500:])
        raise RuntimeError('replay failed (%d)' % rc)
    v = list(map(float, m.groups()))
    return dict(baked=int(v[0]), cut=int(v[5]), pct=v[6], reads=int(r.group(1)), ages=[int(r.group(i)) for i in range(2, 8)], age_max=int(r.group(8)), over=int(r.group(9)),
                edits=int(g.group(1)), restores=int(g.group(2)), pristine=g.group(4) == '1')


def hash_diff(a, b):
    def rd(p):
        out = {}
        for ln in open(p):
            ln = ln.strip()
            if ln and ',' in ln:
                k, h = ln.split(',')
                out[k if k == 'format' else int(k)] = h
        return out
    A, B = rd(a), rd(b)
    return [k for k in A if k != 'format' and A[k] != B.get(k)], len([k for k in A if k != 'format'])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-replay', action='store_true')
    ap.add_argument('--no-fview', action='store_true')
    a = ap.parse_args()
    root = a.root
    main_co = main_checkout(root)
    work = os.path.join(root, 'out', 'crowd_bake_skip')
    wk = os.path.join(work, 'work')
    os.makedirs(wk, exist_ok=True)
    zips = {'brock': os.path.join(main_co, BROCK), 'wm30': os.path.join(main_co, WM30), 'j1': os.path.join(main_co, J1), 'match15': os.path.join(main_co, MATCH15)}
    for z in zips.values():
        if not os.path.isfile(z):
            print('FAIL  input missing: ' + z)
            return 1
    exe = os.path.join(root, 'out', 'tools', 'bake_skip_test.exe')
    rc, out = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'crowd', 'build_bake_skip_test.ps1')], 900)
    if rc:
        print(out[-2000:])
        print('FAIL  bake_skip_test build')
        return 1
    rc, out = run([sys.executable, '-I', os.path.join(HERE, 'bake_skip_counts.py'), 'brock=' + zips['brock'], 'wm30=' + zips['wm30'], 'j1=' + zips['j1'], 'match15=' + zips['match15'], '--out', work], 1800)
    print(out.strip())
    line(rc == 0, 'counts exported for Check B, J1, wm30 and the 15-type match capture')
    if rc:
        return 1
    stats = {}
    for lb in ('brock', 'j1', 'wm30', 'match15'):
        csv = os.path.join(work, lb + '.csv')
        st = stats[lb] = replay(exe, 'census', csv, ['--decisions', os.path.join(work, lb + '.dec.csv')])
        std = replay(exe, 'skip', csv)
        line(st['over'] == 0 and st['age_max'] <= 4 and st['pristine'] and st['edits'] == 0,
             '%s replay (census): baked %d, cut %d (%.1f %%), %d card loops read pictures aged 0:%d 1:%d 2:%d 3:%d 4:%d 5+:%d (oldest %d, over the bound %d); guest untouched' % (
                 lb, st['baked'], st['cut'], st['pct'], st['reads'], *st['ages'], st['age_max'], st['over']))
        line(std['over'] == 0 and std['age_max'] <= 4 and std['pristine'] and std['edits'] == std['restores'] == std['cut'],
             '%s replay (skip, on the fake guest memory): %d model counts set to 0 and %d restored, memory pristine afterwards, oldest picture %d' % (lb, std['edits'], std['restores'], std['age_max']))
    line(stats['wm30']['cut'] >= 8000 and stats['brock']['cut'] >= 20 and stats['match15']['cut'] >= 4000, 'the cut exists where nobody reads (wm30 %d passes) and rarely where everybody does (Check B %d)' % (stats['wm30']['cut'], stats['brock']['cut']))
    if a.no_replay or a.no_fview:
        return 1 if FAILS else 0
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    fv = _fe.find_or_exit(root=root, kind='dev')
    game = os.path.join(main_co, 'game_data', 'extracted')

    def fview(zip_path, name, extra=()):
        csv = os.path.join(wk, name + '.csv')
        if os.path.exists(csv):
            os.remove(csv)
        for _ in range(12):   # the GPU is shared: retry ONLY the exact start-up refusal
            rc, out = run([fv, '--case', zip_path, '--replay', '--frames', '1000', '--validation', '1', '--game', game, '--hash-frames', csv] + list(extra), 1800)
            if 'Not enough graphics memory for the frame ring' not in out:
                break
        if not os.path.isfile(csv) or 'validation errors 0' not in out:
            print(out[-2000:])
            raise RuntimeError('fview arm %s failed (exit %d)' % (name, rc))
        return csv

    py = [sys.executable, '-I']
    rc, out = run(py + [os.path.join(HERE, 'bake_skip_writes.py'), 'brock=' + zips['brock'], 'wm30=' + zips['wm30'], '--frames', '0:341:20'], 900)
    print(out.strip())
    line(rc == 0, 'every bake iteration writes the same constant registers as the last one (only the bone-16/17 block c124 differs) and sets the same non-constant state (only the per-type bool record differs): the anchor rule leaves the post-pass state the game\'s own')
    for lb, z, tag in (('wm30', zips['wm30'], 'W'), ('match15', zips['match15'], 'M'), ('brock', zips['brock'], 'B'), ('j1', zips['j1'], 'J')):
        cut = os.path.join(wk, tag + '_cut.zip')
        rc, out = run(py + [os.path.join(HERE, 'bake_skip_cut.py'), z, os.path.join(work, lb + '.dec.csv'), cut, '--inherit-fix'], 1800)
        print('      ' + out.strip().splitlines()[-1][:200])
        base = fview(z, tag + '_base')
        cutc = fview(cut, tag + '_cut')
        diff, n = hash_diff(base, cutc)
        if lb in ('wm30', 'match15'):
            line(n > 400 and not diff, '%s: the presented surface of %d of %d frames is byte-equal with %d bake passes cut and the later draws given the registers the cut leaves them (nobody reads the pictures)' % (lb, n - len(diff), n, stats[lb]['cut']))
        else:
            ok = len(diff) <= 4
            worst = ''
            if diff:
                dd = os.path.join(wk, 'png')
                last = max(diff) + 1
                fview(z, tag + '_base_png', ['--frames', str(last), '--dump-frames', ','.join(map(str, diff)), '--dump-dir', os.path.join(dd, 'base')])
                fview(cut, tag + '_cut_png', ['--frames', str(last), '--dump-frames', ','.join(map(str, diff)), '--dump-dir', os.path.join(dd, 'cut')])
                for f in diff:
                    rc, out = run(py + [os.path.join(HERE, 'png_diff.py'), os.path.join(dd, 'base', 'frame_%05d.png' % f), os.path.join(dd, 'cut', 'frame_%05d.png' % f)])
                    m = re.search(r'(\d+) pixels differ \(([\d.]+)%\), (\d+) by more than 8 levels, max channel difference (\d+)', out)
                    ok = ok and bool(m) and float(m.group(2)) <= 0.5 and int(m.group(4)) <= 20
                    worst += '  frame %d: %s' % (f, out.strip())
            line(ok, '%s: %d of %d frames differ (%s) with %d bake passes cut; each within 0.5 %% of its pixels and 20 levels:%s' % (lb, len(diff), n, diff, stats[lb]['cut'], worst))
    # plant no_anchor: the first and the last live thumbnail may be cut (the 15-type match capture, where the first iteration alone writes the lights and the camera): the register emulation must move the pictures
    nad = os.path.join(work, 'match15.noanchor.dec.csv')
    rc, out = run([exe, '--counts', os.path.join(work, 'match15.csv'), '--decisions', nad, '--no-anchor'])
    pz = os.path.join(wk, 'M_noanchor.zip')
    rc, out = run(py + [os.path.join(HERE, 'bake_skip_cut.py'), zips['match15'], nad, pz, '--inherit-fix'], 3000)
    print('      ' + out.strip().splitlines()[-1][:220])
    diff, n = hash_diff(os.path.join(wk, 'M_base.csv'), fview(pz, 'M_noanchor'))
    line(len(diff) > 20, 'plant no_anchor (the first and the last live thumbnail cut too, later draws given the registers the cut leaves): %d of %d frames of the 15-type match differ: caught' % (len(diff), n))
    try:
        os.remove(pz)
    except OSError:
        pass
    # review S2: the registers the bake iterations wrote are not consumed by any later draw. wm30 with EVERY bake cut from frame 60 on (the extreme: the pass leaves the frame's entry state), the later draws given
    # the entry state, and then given 42.0 in every block the bake wrote: the pictures must not move. The positive control poisons EVERY block the chain did not write (frames 60-120): the pictures must move.
    alld = os.path.join(work, 'wm30.all.dec.csv')
    with open(alld, 'w') as fh:
        for ln in open(os.path.join(work, 'wm30.dec.csv')):
            k = ln.split(',')[0]
            if k.isdigit():
                fh.write('%s,%d\n' % (k, 1048575 if int(k) >= 60 else 0))
    for name, extra, env_x, want_equal in (('cut_all', [], {}, True), ('poison_written', ['--poison-inherit'], {}, True), ('poison_every_block', ['--poison-inherit', '--only-frames', '60:120'], {'POISON_ALL': '1'}, False)):
        pz = os.path.join(wk, 'W_%s.zip' % name)
        env = dict(os.environ)
        env.update(env_x)
        rc, out = run(py + [os.path.join(HERE, 'bake_skip_cut.py'), zips['wm30'], alld, pz, '--inherit-fix'] + extra, 3000, env)
        print('      ' + out.strip().splitlines()[-1][:220])
        diff, n = hash_diff(os.path.join(wk, 'W_base.csv'), fview(pz, 'W_' + name))
        if want_equal:
            line(not diff, 'wm30 %s: %d of %d frames differ (every bake pass cut from frame 60; later draws %s): in wm30 nothing the bake wrote into the constant registers is consumed (the 15-type match is where the anchors are needed)' % (
                name, len(diff), n, 'given the entry state' if name == 'cut_all' else 'given 42.0 in every block the bake wrote'))
        else:
            line(len(diff) > 20, 'plant %s (every block the chain did not write poisoned, frames 60-120): %d of %d frames differ: the emulation can see a register dependence' % (name, len(diff), n))
        try:
            os.remove(pz)
        except OSError:
            pass
    plant = os.path.join(wk, 'B_plant.zip')
    rc, out = run(py + [os.path.join(HERE, 'bake_skip_cut.py'), zips['brock'], os.path.join(work, 'brock.dec.csv'), plant, '--plant', 'cut_read'], 1800)
    diff, n = hash_diff(os.path.join(wk, 'B_base.csv'), fview(plant, 'B_plant'))
    line(len(diff) > 100, 'plant cut_read (the bake of a type whose cards are drawn is elided every frame, the picture read stale): %d of %d frames differ: caught' % (len(diff), n))
    for f in ('B_plant.zip', 'B_cut.zip', 'W_cut.zip', 'M_cut.zip', 'J_cut.zip'):   # scratch copies of the case zips: not kept
        try:
            os.remove(os.path.join(wk, f))
        except OSError:
            pass
    print('bake_skip_check: %s' % ('PASS' if not FAILS else '%d FAIL' % FAILS))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
