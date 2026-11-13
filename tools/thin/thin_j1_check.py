#!/usr/bin/env python3
"""Lane 162 (S101): why `--thin X` differs from `--X-ours` on Check J1 (run 7086), and the bars that make the comparison honest. 0 game runs, fview only (no window).

THE CAUSE (logged before any fix; docs/renderer/THIN_DRAWS.md "Lane 162"): J1 was RECORDED with the arena / ring / wrestlers thin switches on. Its frames already carry the draws the live reader made lite
(flag kDrawThin in the capture itself, ~330 a frame, 112,808 over 343 frames): no vertex or index bytes, no counts. An `--X-ours` arm without `--thin` cannot draw them (no template: the binder drops
them) and the hooks cannot join them; `--thin X` draws them from templates learned out of the capture's own full draws. So the two arms differ by exactly the template-drawn lite draws, and no hook
gap is involved. Check B / run 7079 / the 12:26 Brock capture carry no lite draws: there `--thin X` is byte-equal to `--X-ours`.

  py -I tools\\thin\\thin_j1_check.py [--root DIR] [--no-build] [--frames N]

Bars (PASS / FAIL; a missing input FAILs, except the optional lite-free capture, which SKIPs):
  census      J1 carries captured lite draws in every frame (INFO: the numbers)
  control     the OLD comparison (thin vs ours, nothing dropped) differs on most frames (it cannot hold on J1: the control that the bars below are about something)
  accounting  per class (arena, ring, wrestlers): the frames' bound-draw surplus of the thin arm over the ours arm equals the thin arm's template-drawn count, summed over the capture
  drop        L162_DROP_LITE=1 (the capture's own lite draws dropped in EVERY arm, the thin machinery still on): thin == ours on every frame, per class (learning, proof compares, the hooks' joins
              change nothing the picture shows)
  emulation   --thin arena with the emulation blanking ONLY the capture's own lite draws (L162_CAPTURED_ONLY=1) == the full emulation on every frame, same template-drawn count (the emulation adds no
              lite draw of its own on J1, so no hook-visible draw is lost to it)
  lite-free   the 12:26 Brock capture (381 frames, no lite draws): thin == ours on every frame (the real equality bar; SKIP when the folder was tidied)
Plants: wrong_layout (a stored template names another template's layout) on the lite-free capture: the proof compare flags it (layout mismatches > 0) AND the picture differs from ours; the old comparison
(control) is the plant for "the ours arm cannot draw the lite draws"."""
import argparse
import csv
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
THIN_RE = re.compile(r'\[rdr\]\[thin\] (arena|ring|wrestlers) thin (\d+), proof (\d+) \(ok (\d+)\), learned (\d+), pinned (\d+), holes (\d+), proof mismatches (\d+) \(program (\d+), layout (\d+)')


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def read_csv(path):
    with open(path) as fh:
        return list(csv.DictReader(fh))


def trace_rows(path):
    """(frame, op) -> row of an arm's trace.csv (fate 3 = drawn, thin 1 = drawn from a template)."""
    return {(int(r['frame']), int(r['op'])): r for r in read_csv(path)}


def emulation_bar(surplus, t_full, t_conly, tr_full, tr_conly, tr_ours):
    """Lane 300 follow-up: the EXACT accounting of the thin arena arm on a thin-recorded capture.
    (1) the capture's own lite draws are exactly the bound surplus over the ours arm: surplus == t_conly;
    (2) whatever the emulation adds on top (added = t_full - t_conly) is a set of draws that are thin in the full arm, not thin in the
        capture-lite-only arm, and that the ours arm draws live (fate 3, not thin): they replace a live bind, so they add no surplus; the set's
        size equals added.
    An emulated draw the stream does not account for (no ours-arm counterpart, or a count the trace does not show) FAILS. Returns (ok, text)."""
    added = t_full - t_conly
    keys = [k for k, r in tr_full.items() if r['thin'] == '1' and (k not in tr_conly or tr_conly[k]['thin'] != '1')]
    live = [k for k in keys if k in tr_ours and tr_ours[k]['thin'] == '0' and tr_ours[k]['fate'] == '3']
    ok = surplus == t_conly and t_conly > 0 and added >= 0 and len(keys) == added and len(live) == len(keys)
    return ok, 'bound surplus %d == capture-lite-only template-drawn %d; emulation added %d (trace: %d thin only in the full arm, %d of them drawn live by the ours arm)' % (
        surplus, t_conly, added, len(keys), len(live))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--frames', type=int, default=0)
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    lane92 = os.path.join(main_co, 'out', 's101', 'lane92')
    j1 = n12 = None
    if os.path.isdir(lane92):
        for d in sorted(os.listdir(lane92)):
            cap = os.path.join(lane92, d, 'capture')
            if os.path.isfile(os.path.join(cap, 'images.bin')) and os.path.isdir(os.path.join(cap, 'scene')):
                if 'case_20261112_172341' in d:
                    j1 = cap
                if 'case_20261112_122658' in d:
                    n12 = cap
    if not j1 or not os.path.isdir(game):
        print('FAIL  input missing: the J1 capture (out/s101/lane92/case_20261112_172341*/capture) or the game files')
        return 1
    if not args.no_build:
        p = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], capture_output=True, text=True, timeout=2400)
        if p.returncode:
            print((p.stdout + p.stderr)[-3000:])
            print('FAIL  fview build')
            return 1
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    exe = _fe.find_or_exit(root=root, kind='dev')
    out = os.path.join(root, 'out', 'l162check')
    os.makedirs(out, exist_ok=True)
    cache = {}
    fails = 0

    def arm(cap, tag, flags, env=None, frames=343):
        if tag in cache:
            return cache[tag]
        n = args.frames or frames
        d = os.path.join(out, tag)
        cmd = [exe, '--capture', cap, '--root', main_co, '--game', game, '--frames', str(n), '--thin-run', d] + flags
        e = dict(os.environ)
        e['CLAUDECODE'] = '1'   # (headless: fview refuses a window under an agent)
        e.update(env or {})
        text = ''
        for attempt in range(8):
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=1700, env=e)
            text = (p.stdout or '') + (p.stderr or '')
            if 'Not enough graphics memory for the frame ring' in text and attempt < 7:
                time.sleep(40)
                continue
            break
        if p.returncode not in (0, 4) or not os.path.isfile(os.path.join(d, 'frames.csv')):
            print(text[-2000:])
            raise RuntimeError('arm %s failed (exit %d)' % (tag, p.returncode))
        rows = read_csv(os.path.join(d, 'frames.csv'))
        res = {'hash': {r['frame']: r['hash'] for r in rows}, 'bound': [int(r['bound']) for r in rows], 'text': text}
        m = {}
        for g in THIN_RE.finditer(text):
            m[g.group(1)] = {'thin': int(g.group(2)), 'proof': int(g.group(3)), 'learned': int(g.group(5)), 'holes': int(g.group(7)), 'mism': int(g.group(8)), 'layout': int(g.group(10))}
        res['thin'] = m
        lp = os.path.join(d, 'lite.csv')
        res['dir'] = d
        res['lite'] = [int(r['captured_lite']) for r in read_csv(lp)] if os.path.isfile(lp) else []
        cache[tag] = res
        return res

    def same(a, b):
        return [f for f in a['hash'] if a['hash'][f] != b['hash'].get(f)]

    def verdict(ok, text):
        nonlocal fails
        print(('PASS' if ok else 'FAIL') + '  ' + text, flush=True)
        fails += not ok

    drop = {'L162_DROP_LITE': '1'}
    try:
        base = arm(j1, 'j1_arena_thin', ['--arena-ours', '--thin', 'arena'])
        lite = base['lite']
        print('INFO  [J1] the capture already carries %d lite draws (kDrawThin) in %d of %d frames (max %d a frame): the live run had thin on' % (sum(lite), sum(1 for x in lite if x), len(lite), max(lite or [0])))
        verdict(len(lite) > 0 and all(lite), '[J1] census: every frame carries lite draws (a thin-recorded capture)')
        for cls in ('arena', 'ring', 'wrestlers'):
            ours = arm(j1, 'j1_%s_ours' % cls, ['--%s-ours' % cls])
            thin = base if cls == 'arena' else arm(j1, 'j1_%s_thin' % cls, ['--%s-ours' % cls, '--thin', cls])
            diff = same(ours, thin)
            surplus = sum(a - b for a, b in zip(thin['bound'], ours['bound']))
            t = thin['thin'].get(cls, {}).get('thin', -1)
            verdict(len(diff) > len(ours['hash']) // 2, '[J1] control, %s: the old comparison (thin vs ours, nothing dropped) differs on %d of %d frames (it cannot hold: the ours arm cannot draw the lite draws)' % (cls, len(diff), len(ours['hash'])))
            if cls == 'arena':
                co = arm(j1, 'j1_arena_conly', ['--arena-ours', '--thin', 'arena'], {'L162_CAPTURED_ONLY': '1'})
                tf, tc = t, co['thin'].get('arena', {}).get('thin', -1)
                trf, trc, tro = (trace_rows(os.path.join(x['dir'], 'trace.csv')) for x in (thin, co, ours))
                ok, why = emulation_bar(surplus, tf, tc, trf, trc, tro)
                verdict(ok, '[J1] accounting, arena (exact): ' + why)
                # plant: one emulated draw the stream does not account for (a thin row at an op the ours arm never drew) must FAIL the bar
                bad = dict(trf)
                bad[(10 ** 6, 1)] = {'thin': '1', 'fate': '3', 'keep_why': '0', 'swapped': '0'}
                okp, whyp = emulation_bar(surplus, tf + 1, tc, bad, trc, tro)
                verdict(not okp, 'planted unaccounted emulated draw (template-drawn +1, a row with no ours-arm draw): the exact bar FAILS it (%s)' % whyp)
                okp2, _ = emulation_bar(surplus, tf + 1, tc, trf, trc, tro)
                verdict(not okp2, 'planted miscounted emulation (template-drawn +1, trace unchanged): the exact bar FAILS it')
            else:
                verdict(surplus == t and t > 0, '[J1] accounting, %s: the thin arm binds %d more draws than the ours arm over %d frames = its %d template-drawn draws' % (cls, surplus, len(ours['hash']), t))
            dours = arm(j1, 'j1_%s_dours' % cls, ['--%s-ours' % cls], drop)
            dthin = arm(j1, 'j1_%s_dthin' % cls, ['--%s-ours' % cls, '--thin', cls], drop)
            dd = same(dours, dthin)
            tm = dthin['thin'].get(cls, {})
            verdict(not dd and len(dours['hash']) == len(ours['hash']), '[J1] drop, %s: with the capture\'s own lite draws dropped in both arms, thin == ours on all %d frames (%d differ%s); thin machinery on: learned %s, proof compares %s, holes %s, mismatches %s' % (
                cls, len(dours['hash']), len(dd), (' ' + str(dd[:6])) if dd else '', tm.get('learned'), tm.get('proof'), tm.get('holes'), tm.get('mism')))
        co = arm(j1, 'j1_arena_conly', ['--arena-ours', '--thin', 'arena'], {'L162_CAPTURED_ONLY': '1'})
        dc = same(co, base)
        verdict(not dc, '[J1] emulation changes no picture: the full emulation == only-the-capture\'s-lite on all %d frames (%d differ); what it adds is accounted for by the exact bar above (full %s, capture-lite-only %s template-drawn)' % (
            len(base['hash']), len(dc), base['thin'].get('arena', {}).get('thin'), co['thin'].get('arena', {}).get('thin')))
        if n12:
            n_ours = arm(n12, 'n_ours', ['--arena-ours'], frames=381)
            n_thin = arm(n12, 'n_thin', ['--arena-ours', '--thin', 'arena'], frames=381)
            dn = same(n_ours, n_thin)
            verdict(not dn and not any(n_thin['lite']), '[lite-free] the 12:26 Brock capture (no lite draws): --thin arena == --arena-ours on all %d frames (%d differ), template-drawn %s' % (len(n_ours['hash']), len(dn), n_thin['thin'].get('arena', {}).get('thin')))
            plant = arm(n12, 'n_wrong_layout', ['--arena-ours', '--thin', 'arena', '--thin-fault', 'wrong_layout'], frames=381)
            dp = same(n_ours, plant)
            lm = plant['thin'].get('arena', {}).get('layout', 0)
            verdict(lm > 0 and len(dp) > 0, 'planted wrong_layout (lite-free capture): proof compare layout mismatches %d (honest 0), %d frames differ from ours (honest 0)' % (lm, len(dp)))
        else:
            print('SKIP  the 12:26 Brock capture (out/s101/lane92/case_20261112_122658*) is gone: the lite-free equality is Check B\'s (thin_arena)')
    except RuntimeError as e:
        print('FAIL  ' + str(e))
        return 1
    print('%s  thin_j1 (lane 162): %d failing bars' % ('FAIL' if fails else 'PASS', fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
