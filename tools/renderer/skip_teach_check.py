r"""S102 lane K4 offline proof: the ring is not lost under the offline skip emulation (D3_SKIP=every2 / every3) any more, 0 game runs.

  py -I tools\renderer\skip_teach_check.py [--root DIR] [--no-build] [--keep]

The open problem K2 lodged (SKIP_COST.md finding 6): fview J1 with D3_SKIP=every2 loses the ring (ropes, canvas, posts) from the first frames in EVERY variant. Cause (SKIP_COST.md K4 finding 8): J1 was recorded
live, so its ring draws are already lite (kDrawThin, no geometry) from the RECORDING run's pin set, and the only full draws a template can be learned from sit in 32 frames, mostly odd ones. The schedule `f % 2 == 1`
skips exactly those, the binder never learns a ring key, and every captured-lite ring draw is "not learned" (drawn by nothing). A live skipped frame only DELAYS a learn (the reader makes a draw lite only for a key in the
binder's own published pin set: reader_thin.h Query), a frozen capture cannot. The emulation's fix: a frame the no-skip run learned a template in is bound, not skipped (fview D3_SKIP_TEACH_OUT / D3_SKIP_TEACH_IN).

Capture (REQUIRED, a missing one FAILS by name): J1 = out\s101\lane92\case_20261112_172341_872_0_pc_run_exit (343 frames), fview --ring-ours --thin ring --d3 ring.
Arms (pass 1 writes the ring ledger once; `noskip` also writes the teach list):
  noskip        the reference: exit 0, validation errors 0, templates learned L0 > 0, teach frames > 0
  every2 / every3  D3_SKIP=every2|every3 + D3_SKIP_TEACH_IN: bars: exit 0, validation 0, learned == L0, the `bound` column (draws bound per frame) equal to noskip's on every drawn frame, the ring's thin draws within 2 % of noskip's;
                the drawn pictures equal to noskip's within the tolerance (every differing frame: no pixel off by more than 8 grey levels, at most 400 pixels differing)
  prune g / n   every2 + D3_SKIP_LASTING=prune with the group rule and with D3_NOGROUP=1 (K2's arms; the picture comparison K2 had to leave INFO): same bars, and the two arms' hash lists equal each other
  default bundle every2 + --shadows-ours --shadow-casters-ours --plane-shadow-ours: same bars (a ring that is present only without the bundle does nothing for players)
Planted twins (each must trip a bar, and does):
  old_schedule  every2 without the teach list (the pre-K4 schedule: the cause): learned < L0, bound differs, pictures differ
  half_teach    every2 with every other teach frame removed
A twin that trips nothing fails as NOT CAUGHT; a plant whose arm did not run fails as DID NOT APPLY.
Exit 1 on any failed bar or an unreadable input."""
import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(HERE, '..', 'fview'))
FAILS = []
SAMPLE = [100, 196, 204, 231, 245, 257, 300, 339]   # frames saved in every arm for the picture bar (all drawn in every2 / every3 phase 1 or bound by the teach list)
TOL_PX_DIFF = 400
TOL_GREY = 8
RING = re.compile(r'ring thin (\d+), proof (\d+) \(ok (\d+)\), learned (\d+)')
VALID = re.compile(r'validation errors (\d+)')
D3 = re.compile(r'\[rdr\]\[d3\]\[ring\] mode \w+: markers (\d+), part draws (\d+), holes (\d+), no sequence (\d+), learned (\d+),.*?calls = runs checked (\d+) differ (\d+).*?unconsumed (\d+)')


def say(ok, text):
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    if not ok:
        FAILS.append(text)


def main_checkout(root):
    try:
        common = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return root


def read_csv(path):
    rows = {}
    with open(path) as fh:
        next(fh)
        for line in fh:
            p = line.strip().split(',')
            if len(p) >= 4:
                rows[int(p[0])] = (p[1], p[2], p[3])
    return rows


def ppm(path):
    from PIL import Image
    return Image.open(path).convert('RGB')


def pic_diff(a_path, b_path):
    import numpy as np
    from PIL import ImageChops
    a, b = ppm(a_path), ppm(b_path)
    if a.size != b.size:
        return 10 ** 9, 255
    n = np.array(ImageChops.difference(a, b)).max(axis=2)
    return int((n > 0).sum()), int(n.max())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--keep', action='store_true', help='keep the saved frames')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = os.environ.get('RDR_MAIN_CHECKOUT') or main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    cap = os.path.join(main_co, 'out', 's101', 'lane92', 'case_20261112_172341_872_0_pc_run_exit', 'capture')
    if not os.path.isdir(game):
        print('FAIL  input missing: the game files ' + game)
        return 1
    if not (os.path.isfile(os.path.join(cap, 'images.bin')) and os.path.isdir(os.path.join(cap, 'scene'))):
        print('FAIL  input missing: capture J1: ' + cap)
        return 1
    if not args.no_build:
        p = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], capture_output=True, text=True, timeout=2400)
        if p.returncode:
            print((p.stdout + p.stderr)[-3000:])
            print('FAIL  fview build (exit %d)' % p.returncode)
            return 1
    import fview_exe as fe
    exe = fe.find_or_exit(root=root, kind='dev')
    out = os.path.join(root, 'out', 'k4_%d' % os.getpid())   # one folder per process: two runs at once (a check and a hand run) deleted each other's saved frames
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out, exist_ok=True)
    n = len([x for x in os.listdir(os.path.join(cap, 'frames'))])
    ledger = os.path.join(out, 'ledger.txt')
    teach = os.path.join(out, 'teach.txt')
    half = os.path.join(out, 'teach_half.txt')

    def run(tag, env, extra=(), save=(), ledger_mode='in', cap_=None, flags=None, ledger_=None):
        d = os.path.join(out, tag)
        cmd = [exe, '--capture', cap_ or cap, '--game', game, '--frames', str(n), '--thin-run', d] + (flags or ['--ring-ours', '--thin', 'ring', '--d3', 'ring']) + ['--d3-ledger-' + ledger_mode, ledger_ or ledger] + list(extra)
        e = dict(os.environ)
        for k in list(e):
            if k.startswith('D3_'):
                e.pop(k)
        e.update(env)
        if save:
            e['D3_SAVE'] = ','.join(str(x) for x in save)
        text, rc = '', 1
        for attempt in range(8):
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=1500, env=e)
            text, rc = (p.stdout or '') + (p.stderr or ''), p.returncode
            if 'Not enough graphics memory for the frame ring' in text and attempt < 7:
                print('  (GPU busy: retry %d in 40 s)' % (attempt + 1), flush=True)
                import time
                time.sleep(40)
                continue
            break
        rm = RING.search(text)
        g = lambda pat: (lambda m: int(m.group(1)) if m else -1)(re.search(pat, text))
        mk = re.search(r'marked (\d+) of (\d+)', text)
        pm = re.search(r'bound guest frames even (\d+) / odd (\d+)', text)
        dm = D3.search(text)
        vm = VALID.search(text)
        csv = os.path.join(d, 'frames.csv')
        return dict(tag=tag, d=d, rc=rc, text=text, rows=read_csv(csv) if os.path.isfile(csv) else {},
                    thin=int(rm.group(1)) if rm else -1, learned=int(rm.group(4)) if rm else -1, valid=int(vm.group(1)) if vm else -1,
                    k4=dict(markers=g(r'markers (\d+)'), marked=int(mk.group(1)) if mk else -1, marked_of=int(mk.group(2)) if mk else -1, proof_obj=g(r'proof objects (\d+)'), never=g(r'never proved (\d+)'),
                            holes=g(r'holes (\d+), proof mismatches'), keys_overdue=g(r'keys not proved within \d+ frames while drawn (\d+)'), even=int(pm.group(1)) if pm else -1, odd=int(pm.group(2)) if pm else -1),
                    d3=dict(zip(('markers', 'parts', 'holes', 'no_seq', 'learned', 'calls', 'differ', 'unconsumed'), map(int, dm.groups()))) if dm else None)

    p1 = run('pass1', {}, ledger_mode='out')
    say(p1['rc'] == 0 and os.path.isfile(ledger), 'pass 1 (the ring ledger): exit %s' % p1['rc'])
    ns = run('noskip', {'D3_SKIP_TEACH_OUT': teach}, save=SAMPLE)
    frames_teach = [int(x) for x in open(teach).read().split()] if os.path.isfile(teach) else []
    say(ns['rc'] == 0 and ns['valid'] == 0 and ns['learned'] > 0 and len(frames_teach) > 0,
        'noskip: exit %s, validation errors %s, templates learned %s, teach frames %s (first %s)' % (ns['rc'], ns['valid'], ns['learned'], len(frames_teach), frames_teach[:6]))
    if not frames_teach:
        print('FAIL  no teach list: nothing to prove')
        return 1
    with open(half, 'w') as fh:
        fh.write(''.join('%d\n' % x for i, x in enumerate(frames_teach) if i % 2 == 0))
    tin = {'D3_SKIP_TEACH_IN': teach}
    bundle = ['--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours']
    plant_caught_acc = {}
    arms = [
        ('every2', run('every2', dict(tin, D3_SKIP='every2'), save=SAMPLE)),
        ('every3', run('every3', dict(tin, D3_SKIP='every3'), save=SAMPLE)),
        ('prune_group', run('prune_g', dict(tin, D3_SKIP='every2', D3_SKIP_LASTING='prune'), save=SAMPLE)),
        ('prune_nogroup', run('prune_n', dict(tin, D3_SKIP='every2', D3_SKIP_LASTING='prune', D3_NOGROUP='1'), save=SAMPLE)),
    ]
    bun_ref = run('bundle_ref', {}, extra=bundle, save=SAMPLE)
    bun = run('bundle', dict(tin, D3_SKIP='every2'), extra=bundle, save=SAMPLE)
    plants = [('old_schedule', run('old_schedule', {'D3_SKIP': 'every2'}, save=SAMPLE)),
              ('half_teach', run('half_teach', {'D3_SKIP': 'every2', 'D3_SKIP_TEACH_IN': half}, save=SAMPLE))]

    def judge(name, arm, ref, quiet=False):
        """Returns (ok, why list)."""
        why = []
        if arm['rc'] != 0 or arm['valid'] != 0:
            why.append('exit %s, validation errors %s' % (arm['rc'], arm['valid']))
        if arm['learned'] != ref['learned']:
            why.append('templates learned %s (no-skip %s)' % (arm['learned'], ref['learned']))
        drawn = [f for f in sorted(ref['rows']) if arm['rows'].get(f, ('0',))[0] != '0']
        if not drawn:
            why.append('no drawn frames')
        bound_diff = [f for f in drawn if arm['rows'][f][2] != ref['rows'][f][2]]
        if bound_diff:
            why.append('draws bound differ in %d of %d drawn frames (first %s)' % (len(bound_diff), len(drawn), bound_diff[:4]))
        if ref['thin'] > 0 and arm['thin'] < 0.98 * ref['thin'] * len(drawn) / max(1, n):
            pass   # (the ring's thin draws scale with the drawn frames; judged by the bound column above)
        a4, r4 = arm['k4'], ref['k4']   # review F2: the D3 chain must be as alive as the no-skip run's (the picture bars alone passed with a dead chain)
        share = len(drawn) / max(1, sum(1 for h in ref['rows'].values() if h[0] != '0'))
        if a4['marked'] < 0.8 * r4['marked']:
            why.append('D3 objects marked %d (no-skip %d)' % (a4['marked'], r4['marked']))
        if a4['markers'] < 0.6 * share * r4['markers']:
            why.append('D3 markers %d (no-skip %d x share %.2f)' % (a4['markers'], r4['markers'], share))
        differ = [f for f in drawn if arm['rows'][f][0] != ref['rows'][f][0]]
        worst = (0, 0, None)
        for f in differ:
            a = os.path.join(ref['d'], 'frame_%d.ppm' % f)
            b = os.path.join(arm['d'], 'frame_%d.ppm' % f)
            if f not in SAMPLE or not os.path.isfile(a) or not os.path.isfile(b):
                continue
            px, mx = pic_diff(a, b)
            if px > TOL_PX_DIFF or mx > TOL_GREY:
                why.append('frame %d: %d pixels differ, max %d grey levels' % (f, px, mx))
            if px > worst[0]:
                worst = (px, mx, f)
        unsaved = [f for f in differ if f not in SAMPLE]
        if unsaved and len(unsaved) > 6:
            why.append('%d drawn frames differ by hash outside the saved sample (first %s)' % (len(unsaved), unsaved[:6]))
        if len(differ) > 8:
            why.append('%d of %d drawn frames differ by hash' % (len(differ), len(drawn)))
        if arm['d3'] is not None and (arm['d3']['holes'] or arm['d3']['unconsumed'] or arm['d3']['differ']):
            why.append('D3 counters: holes %s, unconsumed %s, calls differ %s' % (arm['d3']['holes'], arm['d3']['unconsumed'], arm['d3']['differ']))
        if not quiet:
            say(not why, '%s: exit %s, validation %s, templates learned %s (no-skip %s), %d drawn frames, bound equal %s, pictures: %d differ by hash, worst saved frame %s px %s grey, D3 marked %s (no-skip %s) markers %s (no-skip %s)%s' % (
                name, arm['rc'], arm['valid'], arm['learned'], ref['learned'], len(drawn), 'yes' if not bound_diff else 'NO', len(differ), worst[0], worst[1], a4['marked'], r4['marked'], a4['markers'], r4['markers'], ('; ' + '; '.join(why)) if why else ''))
        return not why, why

    for name, arm in arms:
        judge('[J1] ' + name + ' + teach list', arm, ns)
    g, nn = arms[2][1], arms[3][1]
    say(g['rows'] == nn['rows'] and len(g['rows']) > 0, '[J1] prune group and prune nogroup: the %d frames.csv rows (hash, draws, bound) are equal (K2 left this INFO)' % len(g['rows']))
    judge('[J1] default bundle (shadows, casters, plane ours) + teach list', bun, bun_ref)
    for name, arm in plants:
        applied = arm['rc'] == 0 and bool(arm['rows']) and bool(arm['text'])
        if not applied:
            say(False, 'plant %s DID NOT APPLY (exit %s)' % (name, arm['rc']))
            continue
        ok, why = judge('plant ' + name, arm, ns, quiet=True)
        say(not ok, 'plant %s is CAUGHT: %s' % (name, '; '.join(why[:3]) if why else 'NOT CAUGHT (every bar passed)'))
    # review F1 / F2 (S102 K4): the proof frames visit both parities, so a session that binds ONE parity (the 30 fps decoupled mode) still certifies its D3 objects. Check B (full draws in every frame), ring and arena chains:
    # no-skip, every2 binding only one parity of guest frames (PHASE 0 and 1). Bars: the objects marked, the objects proved (scaled by the share of bound frames), the markers (same), never proved and keys-overdue not worse than
    # no-skip's, thin holes 0, the parity histogram shows ONE parity bound. Twin: the pre-K4 rule (D3_PROOF_RULE=old, a dev-build plant) at the phase whose parity its odd-only proofs never meet must fail.
    capB = os.path.join(main_co, 'out', 's101', 'thin', 'case_brock', 'capture')
    if not (os.path.isfile(os.path.join(capB, 'images.bin')) and os.path.isdir(os.path.join(capB, 'scene'))):
        say(False, 'input missing: capture B: ' + capB)
    else:
        nb = len(os.listdir(os.path.join(capB, 'frames')))
        chains = [('ring', ['--ring-ours', '--thin', 'ring', '--d3', 'ring']), ('arena', ['--arena-ours', '--thin', 'arena', '--d3', 'arena'])]

        def rot(name, arm, ref, share):
            a, r = arm['k4'], ref['k4']
            why = []
            if arm['rc'] != 0 or arm['valid'] != 0:
                why.append('exit %s, validation %s' % (arm['rc'], arm['valid']))
            if a['marked'] < 0.9 * r['marked']:
                why.append('objects marked %d of %d (no-skip %d)' % (a['marked'], a['marked_of'], r['marked']))
            if a['proof_obj'] < 0.9 * share * r['proof_obj']:
                why.append('objects proved %d (no-skip %d x share %.2f)' % (a['proof_obj'], r['proof_obj'], share))
            if a['markers'] < 0.8 * share * r['markers']:
                why.append('markers %d (no-skip %d x share %.2f)' % (a['markers'], r['markers'], share))
            if a['never'] > r['never'] + 3:
                why.append('never proved %d (no-skip %d)' % (a['never'], r['never']))
            if a['keys_overdue'] > r['keys_overdue'] + 10:
                why.append('pinned keys not proved within %d frames %d (no-skip %d)' % (128, a['keys_overdue'], r['keys_overdue']))
            if a['holes'] != 0:
                why.append('thin holes %s' % a['holes'])
            if min(a['even'], a['odd']) != 0 or max(a['even'], a['odd']) <= 0:
                why.append('bound guest frames even %s / odd %s (one parity expected)' % (a['even'], a['odd']))
            return not why, why, a

        for cname, cflags in chains:
            led = os.path.join(out, 'ledger_B_%s.txt' % cname)
            kw = dict(cap_=capB, flags=cflags, ledger_=led)
            run('B_%s_pass1' % cname, {}, ledger_mode='out', **kw)
            rb = run('B_%s_noskip' % cname, {}, **kw)
            say(rb['rc'] == 0 and rb['k4']['marked'] > 0 and rb['k4']['proof_obj'] > 0, '[B %s] no-skip: marked %s of %s, objects proved %s, markers %s, thin holes %s' % (cname, rb['k4']['marked'], rb['k4']['marked_of'], rb['k4']['proof_obj'], rb['k4']['markers'], rb['k4']['holes']))
            for ph in ('0', '1'):
                arm = run('B_%s_p%s' % (cname, ph), {'D3_SKIP': 'every2', 'D3_SKIP_PHASE': ph}, **kw)
                ok, why, a = rot('x', arm, rb, 0.5)
                say(ok, '[B %s] every2 phase %s (one parity bound: even %s / odd %s): objects marked %s of %s (no-skip %s), objects proved %s (no-skip %s), markers %s (no-skip %s), keys overdue %s (no-skip %s)%s' % (
                    cname, ph, a['even'], a['odd'], a['marked'], a['marked_of'], rb['k4']['marked'], a['proof_obj'], rb['k4']['proof_obj'], a['markers'], rb['k4']['markers'], a['keys_overdue'], rb['k4']['keys_overdue'], ('; ' + '; '.join(why)) if why else ''))
                if cname == 'ring' or ph == '0':
                    pl = run('B_%s_old_p%s' % (cname, ph), {'D3_SKIP': 'every2', 'D3_SKIP_PHASE': ph, 'D3_PROOF_RULE': 'old'}, **kw)
                    ok2, why2, a2 = rot('x', pl, rb, 0.5)
                    # the old odd-only rule certifies the odd-parity session and loses the even one: one of the two phases must be caught
                    plant_caught = plant_caught_acc.setdefault(cname, [])
                    plant_caught.append((ph, ok2, why2[:2]))
        for cname, rows in plant_caught_acc.items():
            caught = [r for r in rows if not r[1]]
            say(len(rows) > 0 and len(caught) > 0, 'plant pre-K4 proof rule (|1 hashes, frame %% 32) on Check B %s is CAUGHT at phase %s: %s' % (cname, ','.join(r[0] for r in caught) or '-', '; '.join(str(r[2]) for r in caught) or 'NOT CAUGHT at either phase'))
    # the picture bar's own twin: a picture compare that cannot see a lost ring would make every bar above hollow
    bad = plants[0][1]
    px = [pic_diff(os.path.join(ns['d'], 'frame_%d.ppm' % f), os.path.join(bad['d'], 'frame_%d.ppm' % f))[0] for f in SAMPLE if os.path.isfile(os.path.join(bad['d'], 'frame_%d.ppm' % f)) and os.path.isfile(os.path.join(ns['d'], 'frame_%d.ppm' % f))]
    say(len(px) > 0 and max(px) > 10 * TOL_PX_DIFF, 'the picture compare sees the lost ring: the pre-K4 schedule differs by up to %s pixels on a saved frame (tolerance %d)' % (max(px) if px else None, TOL_PX_DIFF))
    if not args.keep:
        for root_, _, files in os.walk(out):
            for fn in files:
                if fn.endswith('.ppm'):
                    os.remove(os.path.join(root_, fn))
    if not args.keep:
        shutil.rmtree(out, ignore_errors=True)
    print('%s: %d failed bar(s)' % ('FAIL' if FAILS else 'PASS', len(FAILS)))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
