#!/usr/bin/env python3
"""Lane 129 (KC) review F2 / F3, 0 game runs: the thin draws of ALL THREE classes together, and with the shadow casters on. fview on Check B's capture (run 7051, 341 frames).

  py -I tools\\thin\\thin_multi_check.py [--root DIR] [--no-build] [--frames N] [--only a,b]

Arms (one fview process each; frames.csv = a 64-bit hash of the presented picture per frame):
  ours3        --arena-ours --ring-ours --wrestlers-ours: the reference.
  thin3        + --thin arena,ring,wrestlers: the picture of ALL frames byte-equal to ours3; every class: holes 0, epoch drops 0, proof mismatches 0.
  release      thin3 + --thin-release-part 120 (the wrestlers' hook destroys one part's buffers at frame 120, as the 240-frame sweep does mid-match): the ARENA's and the RING's epoch drops 0 and holes 0
               (their templates must not blink: review F2); the picture differs from ours3 only in the few frames after 120 (the released part's own in-flight draws), the last frame right.
               Planted `sum_epochs` (not built: the old summed epoch is replaced) is re-found by the honest arm's 0 arena / ring epoch drops: the first run of this check on the summed-epoch code
               cleared every template at a part release (the review's finding), which the bar `arena epoch drops 0` rejects.
  shadow_ref / shadow_thin   --wrestlers-ours --shadow-casters-ours [--thin wrestlers]: the picture byte-equal, and the wrestler core's starts equal its binds (review F3: one begin per bind
               whoever calls first; the shadow board's begin and the thin binder's FrameReady used to start the core twice).
A missing input FAILS."""
import argparse
import os
import re
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def read_csv(path):
    rows = {}
    with open(path) as fh:
        next(fh)
        for line in fh:
            f, h, d, b = line.strip().split(',')
            rows[int(f)] = h
    return rows


def thin_lines(text):
    out = {}
    for l in text.splitlines():
        m = re.match(r'\[rdr\]\[thin\] (arena|ring|wrestlers) thin (\d+), .*?holes (\d+), proof mismatches (\d+).*?unpinned (\d+),.*?epoch drops (\d+)', l)
        if m:
            out[m.group(1)] = dict(thin=int(m.group(2)), holes=int(m.group(3)), mism=int(m.group(4)), unpinned=int(m.group(5)), epoch_drops=int(m.group(6)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--frames', type=int, default=341)
    ap.add_argument('--only', default='')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    out = os.path.join(main_co, 'out', 's101', 'thin')
    cap = os.path.join(out, 'case_brock', 'capture')
    if not os.path.isfile(os.path.join(cap, 'images.bin')) or not os.path.isdir(os.path.join(cap, 'scene')) or not os.path.isdir(game):
        print('FAIL  input missing: Check B\'s capture with scene records at ' + cap + ' (run tools\\thin\\thin_check.py once to extract it) or the game files ' + game)
        return 1
    if not args.no_build:
        rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], 2400)
        if rc:
            print(text[-3000:])
            print('FAIL  fview build (exit %d)' % rc)
            return 1
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    exe = _fe.find_or_exit(root=root, kind='dev')
    if not os.path.isfile(exe):
        print('FAIL  no fview.exe at ' + exe)
        return 1
    only = set(x for x in args.only.split(',') if x)
    want = lambda n: not only or n in only
    n = args.frames
    cache = {}

    def arm(tag, flags, extra):
        if tag in cache:
            return cache[tag]
        d = os.path.join(out, 'arm_multi_' + tag)
        cmd = [exe, '--capture', cap] + flags + ['--game', game, '--frames', str(n), '--thin-run', d] + extra
        text = ''
        for attempt in range(8):  # (the GPU is shared: this exact message is retried, nothing else)
            rc, text = run(cmd, 1800)
            if 'Not enough graphics memory for the frame ring' in text and attempt < 7:
                print('  (GPU busy: frame ring refused, retry %d in 40 s)' % (attempt + 1), flush=True)
                time.sleep(40)
                continue
            break
        csv = os.path.join(d, 'frames.csv')
        if rc not in (0, 4) or not os.path.isfile(csv):
            print(text[-2500:])
            raise RuntimeError('arm %s failed (exit %d)' % (tag, rc))
        st = re.search(r'wrestler core starts: binds (\d+) starts (\d+) early hits (\d+)', text)
        res = {'rows': read_csv(csv), 'text': text, 'thin': thin_lines(text), 'starts': tuple(map(int, st.groups())) if st else None}
        cache[tag] = res
        return res

    def diffs(a, b):
        return [f for f in sorted(a['rows']) if f not in b['rows'] or a['rows'][f] != b['rows'][f]]

    fails = 0

    def verdict(ok, text):
        nonlocal fails
        print(('PASS' if ok else 'FAIL') + '  ' + text)
        fails += not ok

    three = ['--arena-ours', '--ring-ours', '--wrestlers-ours']
    thin3 = ['--thin', 'arena,ring,wrestlers']
    try:
        if want('thin3') or want('release'):
            ours = arm('ours3', three, [])
            verdict(len(ours['rows']) == n and not all(h == '0' for h in ours['rows'].values()), 'ours3 (the three hooks, the reference): %d frames' % len(ours['rows']))
        if want('thin3'):
            t = arm('thin3', three, thin3)
            d = diffs(t, ours)
            verdict(not d and len(t['thin']) == 3, 'thin3: the picture of all %d frames byte-equal to ours3 (%d differ), census lines for %s' % (n, len(d), sorted(t['thin'])))
            for c in ('arena', 'ring', 'wrestlers'):
                x = t['thin'].get(c)
                verdict(x is not None and x['holes'] == 0 and x['epoch_drops'] == 0 and x['mism'] == 0 and x['thin'] > 0, '  %s: thin %s, holes %s, epoch drops %s, proof mismatches %s' % (c, x and x['thin'], x and x['holes'], x and x['epoch_drops'], x and x['mism']))
        if want('release'):
            r = arm('release', three, thin3 + ['--thin-release-part', '120'])
            base = arm('thin3', three, thin3)
            d = diffs(r, ours)
            ar, rg, wr = r['thin'].get('arena'), r['thin'].get('ring'), r['thin'].get('wrestlers')
            verdict(ar is not None and rg is not None and ar['epoch_drops'] == 0 and rg['epoch_drops'] == 0 and ar['holes'] == 0 and rg['holes'] == 0,
                    'release_part (a wrestler part destroyed at frame 120): arena epoch drops %s holes %s, ring epoch drops %s holes %s (all required 0: only the wrestlers\' templates naming the part go)' % (
                        ar and ar['epoch_drops'], ar and ar['holes'], rg and rg['epoch_drops'], rg and rg['holes']))
            verdict(wr is not None and wr['holes'] == 0 and wr['unpinned'] > base['thin'].get('wrestlers', {}).get('unpinned', 0),
                    'release_part: wrestlers holes %s, keys unpinned %s (the undisturbed thin3 arm: %s: the released part\'s keys went)' % (wr and wr['holes'], wr and wr['unpinned'], base['thin'].get('wrestlers', {}).get('unpinned')))
            verdict((n - 1) not in d and all(120 <= f <= 130 for f in d), 'release_part: the picture differs from ours3 only in frames %s (the released part\'s in-flight draws), the last frame right' % (d[:12] if d else 'none'))
        if want('shadow'):
            ref = arm('shadow_ref', ['--wrestlers-ours', '--shadow-casters-ours'], [])
            th = arm('shadow_thin', ['--wrestlers-ours', '--shadow-casters-ours'], ['--thin', 'wrestlers'])
            d = diffs(th, ref)
            s = th['starts']
            verdict(not d and bool(th['thin'].get('wrestlers')) and th['thin']['wrestlers']['thin'] > 0, 'shadow casters on: the picture of all %d frames byte-equal to the shadow ours arm (%d differ), wrestlers thin %s' % (
                n, len(d), th['thin'].get('wrestlers', {}).get('thin')))
            verdict(s is not None and s[0] >= n - 1 and s[1] == s[0], 'shadow casters on: the wrestler core\'s starts %s = its binds %s (review F3: one begin per bind; the bind count is the frames bound, %d)' % (s and s[1], s and s[0], n))
    except RuntimeError as e:
        print('FAIL  ' + str(e))
        return 1
    print('thin_multi: %s' % ('PASS' if not fails else 'FAIL (%d)' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
