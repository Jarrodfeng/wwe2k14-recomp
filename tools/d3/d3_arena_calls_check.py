r"""Lane 274 (docs\renderer\SKIP_GAME_WORK_MAP.md 10.12): the ARENA chain's `calls = runs` proven against the capture's REAL calls, through the real binder (fview), 0 game runs.

  py -I tools\d3\d3_arena_calls_check.py [--root DIR] [--no-build] [--frames N] [--caps J1,RL1]

Why. Every offline `calls = runs` guard so far fed the hook tally from EmulateHook, which makes ONE emulated call per binder run: true by construction, so it hid the ring's RL1 (a model called once per instance, the instances
adjacent in the stream, ScanRuns merged them into one run). Here `D3_REAL_CALLS=1` (d3_binder.h EmulateHook, Mode::Record) feeds the hook's keyed-call count of each frame from the capture's LightRefs (one per real
sub_82C04DF0 call) of every object with a keyed run in the stream. The arena chain through the real binder, record route (`--thin arena --d3 arena --d3-mode record`, the two-pass offline ledger as d3_check.py's J1 arms), on J1 (run 7086)
and the RL1 capture (run 7171, the only kept capture that also holds ring-rows records):
  d1        `--thin arena` (no D3): the reference picture of every frame.
  honest    the fixed scan (arena route: a part restart at the same pass site starts a new run), real calls fed: calls = runs differ 0; the picture of ALL frames byte-equal to d1; markers > 0; holes 0; no sequence 0;
            ordinal mismatch 0; unconsumed 0; proof mismatches of marked objects 0.
  blind     the same arm WITHOUT D3_REAL_CALLS (the old, emulated tally): differ 0 (the control: this is why it was invisible).
  verify    `--d3-verify` + real calls: calls = runs differ 0, the picture byte-equal.
Plants (each must FAIL its named check):
  arena_merge_instances (--d3-fault, the pre-fix scan, THE PAST BUG of the ring's RL1 class on the arena chain) with real calls: calls = runs differ > 0, caught by `calls`; with the emulated tally: differ 0 (the blindness, reported).
Needs the kept captures (J1, RL1) and out\renderer\dev\fview.exe (rebuilt unless --no-build). A missing required input FAILS loudly (no silent SKIP)."""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import d3_check as DC  # noqa: E402  (CENSUS regex, run(), read_csv(), main_checkout())

CALLS = re.compile(r'calls = runs checked (\d+) differ (\d+)')
MARKED = re.compile(r'objects marked (\d+) of (\d+) pinned')
MARKMIS = re.compile(r'proof mismatches of marked objects (\d+) \(all (\d+)\)')


def find_caps(main_co):
    caps = {}
    lane92 = os.path.join(main_co, 'out', 's101', 'lane92')
    if os.path.isdir(lane92):
        for d in sorted(os.listdir(lane92)):
            c = os.path.join(lane92, d, 'capture')
            if 'case_20261112_172341' in d and os.path.isfile(os.path.join(c, 'images.bin')):
                caps['J1'] = c
    rl1 = os.path.join(main_co, 'out', 's101', 'rl1_diag', 'case', 'capture')
    if os.path.isfile(os.path.join(rl1, 'images.bin')):
        caps['RL1'] = rl1
    return caps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--frames', type=int, default=0)
    ap.add_argument('--caps', default='J1,RL1')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_co = DC.main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    if not os.path.isdir(game):
        print('FAIL  input missing: the game files ' + game)
        return 1
    caps = find_caps(main_co)
    want = [c for c in a.caps.split(',') if c]
    for c in want:
        if c not in caps:
            print('FAIL  input missing: the %s capture (J1: out/s101/lane92/case_20261112_172341*/capture; RL1: out/s101/rl1_diag/case/capture)' % c)
            return 1
    if not a.no_build:
        rc, text = DC.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], 2400)
        if rc:
            print(text[-3000:])
            print('FAIL  fview build (exit %d)' % rc)
            return 1
    sys.path.insert(0, os.path.join(HERE, '..', 'fview'))
    import fview_exe as fe  # noqa: E402
    exe = fe.find_or_exit(root=root, kind='dev')
    out = os.path.join(root, 'out', 'd3calls')
    os.makedirs(out, exist_ok=True)
    fails = 0

    def verdict(ok, text):
        nonlocal fails
        print(('PASS' if ok else 'FAIL') + '  ' + text)
        fails += not ok

    def arm(ck, tag, extra, env=None):
        cap = caps[ck]
        d = os.path.join(out, 'arm_%s_%s' % (ck, tag))
        nfr = a.frames or len([x for x in os.listdir(os.path.join(cap, 'frames')) if x.startswith('frame_') and x.endswith('.bin')])
        cmd = [exe, '--capture', cap, '--arena-ours', '--game', game, '--frames', str(nfr), '--thin-run', d] + extra
        text, rc = '', 0
        for attempt in range(8):
            rc, text = DC.run(cmd, 1500, env)
            if 'Not enough graphics memory for the frame ring' in text and attempt < 7:
                print('  (GPU busy: frame ring refused, retry %d in 40 s)' % (attempt + 1), flush=True)
                time.sleep(40)
                continue
            break
        csv = os.path.join(d, 'frames.csv')
        if rc not in (0, 4) or not os.path.isfile(csv):
            print(text[-2500:])
            raise RuntimeError('arm %s/%s failed (exit %d)' % (ck, tag, rc))
        res = {'rows': DC.read_csv(csv), 'text': text}
        m = DC.CENSUS.search(text)
        if m:
            g = list(map(int, m.groups()))
            res['c'] = dict(zip(['markers', 'parts', 'holes', 'no_seq', 'lost', 'learned', 'complete', 'marked', 'pinned'], g[:9]))
            res['c']['gate_resets'] = g[20]
            res['c']['ord_mism'] = g[21]
            res['c']['unconsumed'] = g[22]
        m = CALLS.search(text)
        res['calls'] = tuple(map(int, m.groups())) if m else None
        m = MARKED.search(text)
        res['marked'] = tuple(map(int, m.groups())) if m else None
        m = MARKMIS.search(text)
        res['markmis'] = int(m.group(1)) if m else None
        return res

    def diffs(x, y):
        return [f for f in sorted(x['rows']) if f not in y['rows'] or x['rows'][f] != y['rows'][f]]

    real = {'D3_REAL_CALLS': '1'}
    for ck in want:
        print('== %s' % ck)
        d1 = arm(ck, 'd1', ['--thin', 'arena'])
        ledger = os.path.join(out, 'ledger_%s.txt' % ck)
        base = ['--thin', 'arena', '--d3', 'arena', '--d3-mode', 'record']
        arm(ck, 'pass1', base + ['--d3-ledger-out', ledger], real)
        if not os.path.isfile(ledger):
            verdict(False, '[%s] pass 1 wrote no ledger' % ck)
            continue
        led = ['--d3-ledger-in', ledger]
        h = arm(ck, 'honest', base + led, real)
        c = h.get('c')
        if not c or not h['calls']:
            verdict(False, '[%s] honest: no census / calls = runs line' % ck)
            continue
        dd = diffs(h, d1)
        verdict(h['calls'][0] > 0 and h['calls'][1] == 0, '[%s] real calls fed: calls = runs checked %d differ %d (required differ 0)' % (ck, h['calls'][0], h['calls'][1]))
        verdict(not dd and len(h['rows']) == len(d1['rows']), '[%s] the picture of all %d frames byte-equal to D1 (%d differ%s)' % (ck, len(h['rows']), len(dd), (': ' + str(dd[:8])) if dd else ''))
        verdict(c['markers'] > 0, '[%s] markers engaged: %d (marked %s of pinned)' % (ck, c['markers'], h['marked']))
        verdict(c['holes'] == 0 and c['no_seq'] == 0 and c['ord_mism'] == 0 and c['unconsumed'] == 0 and (h['markmis'] or 0) == 0, '[%s] holes %d, no sequence %d, ordinal mismatch %d, unconsumed %d, proof mismatches of marked objects %s (all required 0)' % (
            ck, c['holes'], c['no_seq'], c['ord_mism'], c['unconsumed'], h['markmis']))
        b = arm(ck, 'blind', base + led)
        verdict(b['calls'] is not None and b['calls'][1] == 0, '[%s] control, the emulated tally (one emulated call per run): calls = runs differ %s (0 by construction: the blindness that hid RL1)' % (ck, b['calls'] and b['calls'][1]))
        v = arm(ck, 'verify', base + led + ['--d3-verify'], real)
        dv = diffs(v, d1)
        verdict(v['calls'] is not None and v['calls'][1] == 0 and not dv, '[%s] verify arm, real calls: calls = runs differ %s, picture byte-equal to D1 (%d frames differ)' % (ck, v['calls'] and v['calls'][1], len(dv)))
        # plants: the pre-fix scan
        p = arm(ck, 'plant_merge', base + led + ['--d3-fault', 'arena_merge_instances'], real)
        verdict(p['calls'] is not None and p['calls'][1] > 0, 'planted arena_merge_instances (the pre-fix scan), real calls: calls = runs differ %s of %s frames: caught by `calls = runs`' % (p['calls'] and p['calls'][1], p['calls'] and p['calls'][0]))
        pb = arm(ck, 'plant_merge_blind', base + led + ['--d3-fault', 'arena_merge_instances'])
        print('INFO  [%s] the same plant with the emulated tally: calls = runs differ %s (blind); picture differs from D1 on %d frames' % (ck, pb['calls'] and pb['calls'][1], len(diffs(pb, d1))))
        print('INFO  [%s] plant with real calls: picture differs from D1 on %d frames; holes %s, markers %s' % (ck, len(diffs(p, d1)), p.get('c', {}).get('holes'), p.get('c', {}).get('markers')))
    print('%s  (%d failures)' % ('PASS' if not fails else 'FAIL', fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
