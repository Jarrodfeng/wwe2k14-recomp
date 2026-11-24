r"""S102 lane R24-E: offline proof of wwe2k14_rdr_gpu_small (bit 1: the dead quarter-size DoF chain of our post pass is not recorded). 0 game runs, 0 Surface runs.
  py -I tools\r24e\gpu_small_check.py [--root DIR] [--only unit|bryan|wm30|census]
  unit      tools\r24e\gpu_small_test.cpp: the decision table + every planted fault changes an answer.
  bryan     the standard Bryan entrance capture (237 frames, scene records, the SHIPPED DEFAULT BUNDLE) replayed by rplay:
              taken       the census line: on (--gpu-small 1) the lever took the frames (237 of 237 here: two_layer shape, DOFParam2.x 0); 0 taken = the lever does nothing for players = FAIL.
              equal       shots (7 frames) off vs on are BYTE-EQUAL (the combine never samples the skipped chain).
              passes      the role audit steps fall by exactly 2 per taken frame (the two 320x180 passes); undefined reads 0 on and off.
              flip        --arms on=8:1|off=8:0 --arm-period 60: every shot (last frame of a phase and the first after each flip) is byte-equal to the steady run of its phase.
              validation  rplay --validation 1 and 3 with the lever on: `validation errors 0`, exit 0.
              twins       plant 4 (never applied): the census reads 0 taken and the check FAILS on it (a lever that is on and does nothing).
  wm30      the 30 Years entrance / film capture (no scene records: --post-ours only): the lever is declined (near blur live) and the picture is byte-equal; plant 3 (neither test made)
            takes the skip where slots 2 and 6 read the chain: the picture MOVES (the twin the byte-equal check must see). That is the class of lane 296b SF1 (a pass skipped whose image the combine still samples).
  census    tools\r24e\rim_prelight_census.py on Bryan / Brock / wm30 (the rim chain and the pre-light constants: how often equal to the previous frame): reported, held to the finding recorded in docs.
Counts and numbers only (hard rule 1); pictures go under out\ only."""
import argparse, concurrent.futures, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(HERE, '..', 'renderer'))
import colour_cuts_check as cc  # BUNDLE, shot_diffs, main_checkout, say, FAILS

FAILS = cc.FAILS
say = cc.say
PERIOD = 60
BRYAN = ('s101/lane92/case_20261112_122719_969_0_pc_run_exit/capture', 237, [59, 60, 119, 120, 179, 180, 236])
WM30 = ('lane86/wm30/capture', 120, [30, 60, 90, 119])
CENSUS_RE = re.compile(r'\[rdr\]\[gpu_small\] mask (\d+), frames \d+, post records with the switch on: taken (\d+) \(passes removed (\d+)\), kept: near blur live (\d+), far blur shared with the near (\d+), bloom / glare reads the chain (\d+)')
VAL_RE = re.compile(r'validation errors (\d+)')


def run_rplay(root, capture, game, tag, outdir, bundle, extra, frames, timeout=1700):
    exe = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    shots = os.path.join(outdir, tag + '_shots')
    audit = os.path.join(outdir, tag + '_audit.jsonl')
    cmd = [exe, '--capture', capture, '--frames', str(frames), '--game', game, '--role-audit', audit] + bundle + extra
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    text = p.stdout + p.stderr
    d = dict(tag=tag, rc=p.returncode, text=text, shots=shots, audit=audit)
    m = CENSUS_RE.search(text)
    if m:
        d.update(mask=int(m.group(1)), taken=int(m.group(2)), removed=int(m.group(3)), near_live=int(m.group(4)), far_shared=int(m.group(5)), reads=int(m.group(6)))
    v = VAL_RE.findall(text)
    d['val'] = int(v[-1]) if v else -1
    return d


def audit_counts(path):
    p = subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'renderer', 'role_audit.py'), path], capture_output=True, text=True, timeout=300)
    steps = re.search(r': (\d+) steps over', p.stdout)
    und = re.search(r'^undefined: (\d+)', p.stdout, re.M)
    return (int(steps.group(1)) if steps else -1, int(und.group(1)) if und else -1)


def lever_at(frame):
    return 1 if (frame // PERIOD) % 2 == 0 else 0


def equal_shots(a, b, shots):
    bad = []
    for f in shots:
        d = cc.shot_diffs(a, b, [f])
        if d is None:
            bad.append((f, 'missing'))
        elif d[0]['nd'] != 0:
            bad.append((f, d[0]['nd']))
    return len(shots) - len(bad), bad


def flip_equal(flipped, on, off, shots):
    bad = []
    for f in shots:
        ref = on if lever_at(f) else off
        d = cc.shot_diffs(ref, flipped, [f])
        if d is None:
            bad.append((f, 'missing'))
        elif d[0]['nd'] != 0:
            bad.append((f, d[0]['nd']))
    return len(shots) - len(bad), bad


def unit(root):
    exe = os.path.join(tempfile.gettempdir(), 'r24e_gpu_small_test_%d.exe' % os.getpid())
    p = subprocess.run(['C:\\Program Files\\LLVM\\bin\\clang++.exe', '-std=c++20', '-Wall', '-Wextra', '-Werror', os.path.join(root, 'tools', 'r24e', 'gpu_small_test.cpp'), '-o', exe], capture_output=True, text=True, timeout=300)
    if p.returncode != 0:
        say(False, 'unit: the test does not compile:\n' + p.stdout + p.stderr)
        return
    p = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    fails = [l for l in p.stdout.splitlines() if l.startswith('FAIL')]
    say(p.returncode == 0 and not fails and 'all passed' in p.stdout, 'unit: gpu_small_test (%d lines, %d failed; every planted fault changes an answer)' % (len(p.stdout.splitlines()), len(fails)))
    try:
        os.remove(exe)
    except OSError:
        pass


def bryan(root, outdir, game, main_co):
    sub, frames, shots = BRYAN
    capture = os.path.join(main_co, 'out', *sub.split('/'))
    if not os.path.isfile(os.path.join(capture, 'images.bin')):
        say(False, 'bryan: cannot read the capture %s' % capture)
        return
    sh = ','.join(map(str, shots))

    def job(tag, extra):
        return ['--shots', sh, '--shot-dir', os.path.join(outdir, 'bryan_' + tag + '_shots')] + extra
    jobs = {
        'off': job('off', []),
        'on': job('on', ['--gpu-small', '1']),
        'flip': job('flip', ['--arms', 'on=8:1|off=8:0', '--arm-period', str(PERIOD), '--arm-skip', '0']),
        # review F2: no capture crosses x = 0, so an arm kind 7 "MASK:X" forces DOFParam2.x = 0.5 on alternate 60-frame phases (both runs see the same combine constant)
        'xbase': job('xbase', ['--arms', 'a=8:0|b=8:0:1.0', '--arm-period', str(PERIOD), '--arm-skip', '0']),
        'xon': job('xon', ['--arms', 'a=8:1|b=8:1:1.0', '--arm-period', str(PERIOD), '--arm-skip', '0']),
        'xp1': job('xp1', ['--arms', 'a=8:1|b=8:1:1.0', '--arm-period', str(PERIOD), '--arm-skip', '0', '--gpu-small-plant', '1']),
        'p4': job('p4', ['--gpu-small', '1', '--gpu-small-plant', '4']),
        'val1': job('val1', ['--gpu-small', '1', '--validation', '1']),
        'val3': job('val3', ['--gpu-small', '1', '--validation', '3']),
    }
    res = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as ex:
        futs = {k: ex.submit(run_rplay, root, capture, game, 'bryan_' + k, outdir, cc.BUNDLE, v, frames) for k, v in jobs.items()}
        for k, f in futs.items():
            res[k] = f.result()
    for k, r in res.items():
        say(r['rc'] == 0, 'bryan %s: rplay exit code %d' % (k, r['rc']))
    if any(r['rc'] != 0 for r in res.values()):
        return
    on = res['on']
    say(on.get('taken', 0) > 0, 'bryan taken: the lever took %s of %d frames (passes removed %s; kept: near live %s, far shared %s, bloom reads %s) (0 = it does nothing for players)' % (on.get('taken'), frames, on.get('removed'), on.get('near_live'), on.get('far_shared'), on.get('reads')))
    n, bad = equal_shots(res['off'], on, shots)
    say(not bad, 'bryan equal: %d of %d shots byte-equal off vs on%s' % (n, len(shots), '' if not bad else ' (differ: %s)' % bad))
    so, uo = audit_counts(res['off']['audit'])
    sn, un = audit_counts(on['audit'])
    say(so > 0 and sn > 0 and so - sn == 2 * on.get('taken', 0), 'bryan passes: role-audit steps off %d, on %d: %d fewer = 2 x %s taken frames' % (so, sn, so - sn, on.get('taken')))
    say(un == 0 and uo == 0, 'bryan audit: undefined reads off %d, on %d (both must be 0)' % (uo, un))
    nf, badf = flip_equal(res['flip'], on, res['off'], shots)
    fl = res['flip']
    say(not badf, 'bryan flip: every %d frames: %d of %d shots byte-equal to the steady run of their phase%s' % (PERIOD, nf, len(shots), '' if not badf else ' (differ: %s)' % badf))
    say(0 < fl.get('taken', 0) < on.get('taken', 0), 'bryan flip taken: the flipped run took %s frames, steady on %s (must be between: a flip that does nothing is not a proof)' % (fl.get('taken'), on.get('taken')))
    for k in ('val1', 'val3'):
        say(res[k]['val'] == 0, 'bryan validation %s with the lever on: validation errors %d, exit %d' % (k[-1], res[k]['val'], res[k]['rc']))
    nx, badx = equal_shots(res['xbase'], res['xon'], shots)
    say(not badx and 0 < res['xon'].get('taken', 0) < frames, 'bryan crossing x = 0 (x forced to 1.0 on every other 60-frame phase, review F2): %d of %d shots byte-equal off vs on, the lever took %s frames (declined %s as near-live) (the re-run path after a skipped phase)%s' % (nx, len(shots), res['xon'].get('taken'), res['xon'].get('near_live'), '' if not badx else ' (differ: %s)' % badx))
    n1, bad1 = equal_shots(res['xbase'], res['xp1'], shots)
    say(len(bad1) > 0, 'bryan twin plant 1 (the near test not made, forced x 1.0 phases): %d of %d shots DIFFER from the off run (must be > 0)' % (len(bad1), len(shots)))
    say(res['p4'].get('taken', 0) == 0, 'bryan twin plant 4 (never applied): the census reads %s taken (must be 0: the check "taken > 0" would FAIL on it)' % res['p4'].get('taken'))


def wm30(root, outdir, game, main_co):
    sub, frames, shots = WM30
    capture = os.path.join(main_co, 'out', *sub.split('/'))
    if not os.path.isfile(os.path.join(capture, 'images.bin')):
        say(False, 'wm30: cannot read the capture %s' % capture)
        return
    sh = ','.join(map(str, shots))
    bundle = ['--post-ours']   # (no scene records in this capture: the promotions that need them cannot run)

    def job(tag, extra):
        return ['--shots', sh, '--shot-dir', os.path.join(outdir, 'wm30_' + tag + '_shots')] + extra
    jobs = {'off': job('off', []), 'on': job('on', ['--gpu-small', '1']), 'p3': job('p3', ['--gpu-small', '1', '--gpu-small-plant', '3'])}
    res = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as ex:
        futs = {k: ex.submit(run_rplay, root, capture, game, 'wm30_' + k, outdir, bundle, v, frames) for k, v in jobs.items()}
        for k, f in futs.items():
            res[k] = f.result()
    for k, r in res.items():
        say(r['rc'] == 0, 'wm30 %s: rplay exit code %d' % (k, r['rc']))
    if any(r['rc'] != 0 for r in res.values()):
        return
    on = res['on']
    say(on.get('taken', -1) == 0 and on.get('near_live', 0) > 0, 'wm30: the lever is declined where the near blur is live: taken %s, near live %s (the census names the reason)' % (on.get('taken'), on.get('near_live')))
    n, bad = equal_shots(res['off'], on, shots)
    say(not bad, 'wm30 equal: %d of %d shots byte-equal off vs on' % (n, len(shots)))
    n3, bad3 = equal_shots(res['off'], res['p3'], shots)
    say(len(bad3) > 0 and res['p3'].get('taken', 0) > 0, 'wm30 twin plant 3 (neither test made, the chain skipped where slots 2 / 6 read it): taken %s, %d of %d shots DIFFER (must be > 0: the byte-equal check can fail)' % (res['p3'].get('taken'), len(bad3), len(shots)))


def census(root, main_co):
    caps = {'bryan': BRYAN[0], 'brock': 's101/lane92/case_20261113_093550_855_0_pc_run_exit/capture', 'wm30': 'lane86/wm30/capture'}
    for name, sub in caps.items():
        capture = os.path.join(main_co, 'out', *sub.split('/'))
        p = subprocess.run([sys.executable, '-I', os.path.join(root, 'tools', 'r24e', 'rim_prelight_census.py'), capture], capture_output=True, text=True, timeout=1500)
        lines = [l for l in p.stdout.splitlines() if l.startswith(('rim:', 'light:'))]
        ok = p.returncode == 0 and len(lines) == 2
        same = [int(re.search(r'equal to the previous frame (\d+)', l).group(1)) for l in lines] if ok else []
        # the finding the docs record: the rim chain's and the pre-light constants' input is EQUAL to the previous frame on (almost) no frame: no memo can take work
        say(ok and all(s <= 1 for s in same), 'census %s: %s' % (name, ' | '.join(lines) if ok else 'the census failed: ' + p.stderr[-200:]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--only')
    a = ap.parse_args()
    root = a.root
    main_co = cc.main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    outdir = os.path.join(root, 'out', 's102', 'r24e')
    os.makedirs(outdir, exist_ok=True)
    if not a.only or a.only == 'unit':
        unit(root)
    if not a.only or a.only == 'bryan':
        bryan(root, outdir, game, main_co)
    if not a.only or a.only == 'wm30':
        wm30(root, outdir, game, main_co)
    if not a.only or a.only == 'census':
        census(root, main_co)
    print('FAILED: %d' % len(FAILS) if FAILS else 'ALL PASS')
    sys.exit(1 if FAILS else 0)


if __name__ == '__main__':
    main()
