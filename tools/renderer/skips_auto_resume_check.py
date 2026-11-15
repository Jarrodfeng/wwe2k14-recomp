#!/usr/bin/env python3
"""S102 K3 rework review F1 + F2 offline proof (0 game runs), fview on Check B, arena chain, the throttle on (D3_AUTO_THROTTLE).
  py -I tools\\renderer\\skips_auto_resume_check.py [--root DIR] [--explore]

F1 (marks outlive the chain gap): the chains do not watch a stretch of frames (schedule 0:on,100:off,160:on) and an object's template changes inside it (D3_TEMPLATE_STEP=110:3: from frame 110 the VS c185 of 3
marked objects steps every frame, as the game's fade does). Rule on (d3_binder.h Publish: no mark whose last proof is older than the resume frame): `fix` arm. Twin (D3_NO_RESUME_RULE=1, the first rework): marks
survive the gap unproved. Bars: the fix arm has fewer marked-object proof mismatches / differences from the always-on reference than the twin, holes 0; the twin must show the problem (mismatches or skips of the stepped objects).
F2 (a marker on a chain-off frame): the latch ring is 3 frames stale (D3_AUTO_LAG=3) so markers arrive on frames the timing window does not name. Rule on: 0 markers on a frame bound without the chains; twin (D3_NO_MARKER_RULE=1,
the window alone): > 0, and the same arm shows holes / unconsumed.
Exit 0 = pass."""
import argparse, os, re, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'd3'))
import d3_check as dc  # noqa: E402

MISM = re.compile(r'proof objects (\d+) \(parts (\d+), mismatching (\d+)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--explore', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_co = dc.main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    cap = os.path.join(main_co, 'out', 's101', 'thin', 'case_brock', 'capture')
    for need in (game, os.path.join(cap, 'images.bin')):
        if not os.path.exists(need):
            print('FAIL  input missing: ' + need)
            return 1
    sys.path.insert(0, os.path.join(root, 'tools', 'fview'))
    import fview_exe as fe
    exe = fe.find_or_exit(root=root, kind='dev')
    out = os.path.join(root, 'out', 'k3resume')
    os.makedirs(out, exist_ok=True)
    ledger = os.path.join(out, 'ledger.txt')
    n = 341
    base = ['--thin', 'arena', '--d3', 'arena']
    cache = {}

    def arm(tag, extra, env):
        if tag in cache:
            return cache[tag]
        d = os.path.join(out, 'arm_' + tag)
        cmd = [exe, '--capture', cap, '--arena-ours', '--game', game, '--frames', str(n), '--thin-run', d] + extra
        text, rc = '', 0
        for attempt in range(8):
            rc, text = dc.run(cmd, 1500, env)
            if 'Not enough graphics memory for the frame ring' in text and attempt < 7:
                time.sleep(40)
                continue
            break
        csv = os.path.join(d, 'frames.csv')
        if rc not in (0, 4) or not os.path.isfile(csv):
            print(text[-2500:])
            raise RuntimeError('arm %s failed (exit %d)' % (tag, rc))
        m = dc.CENSUS.search(text)
        res = {'rows': dc.read_csv(csv), 'text': text}
        if m:
            g = list(map(int, m.groups()))
            res['c'] = dict(zip(['markers', 'parts', 'holes', 'no_seq', 'lost', 'learned', 'complete', 'marked', 'pinned', 'proof_obj', 'proof_parts', 'mism', 'g_pass', 'g_template', 'g_light', 'g_shadow',
                                 'g_world', 'g_clock', 'g_drift', 'state_differ', 'gate_resets', 'ord_mism', 'unconsumed', 'pass_fallback', 'frames', 'h_markers', 'h_dropped', 'h_gate', 'h_proof', 'h_table', 'h_overflow', 'h_unreadable'], g))
        mk = re.findall(r'markers on a chain-off frame (\d+)', text)
        res['off_markers'] = int(mk[-1]) if mk else None
        rf = re.findall(r'marks withheld after a gap (\d+), published unproved after a gap (\d+)', text)
        res['refused'], res['unproved'] = (int(rf[-1][0]), int(rf[-1][1])) if rf else (None, None)
        cache[tag] = res
        return res

    def diffs(x, y):
        return [f for f in sorted(x['rows']) if f not in y['rows'] or x['rows'][f] != y['rows'][f]]
    fails = 0

    def verdict(ok, text):
        nonlocal fails
        print(('PASS' if ok else 'FAIL') + '  ' + text)
        fails += not ok
    try:
        arm('pass1', base + ['--d3-ledger-out', ledger], {})
        lb = base + ['--d3-ledger-in', ledger]
        step = {'D3_TEMPLATE_STEP': '110:3', 'D3_AUTO_THROTTLE': '1'}
        ref = arm('ref', lb, dict(step, D3_AUTO_SCHED='0:on'))
        fix = arm('fix', lb, dict(step, D3_AUTO_SCHED='0:on,100:off,160:on'))
        twin = arm('twin', lb, dict(step, D3_AUTO_SCHED='0:on,100:off,160:on', D3_NO_RESUME_RULE='1'))
        for name, r in (('ref (always on)', ref), ('fix (rule on)', fix), ('twin (no rule)', twin)):
            c = r['c']
            print('INFO  F1 %s: markers %d, holes %d, proof mismatching parts %d, gate resets %d, pictures differing from ref %d' % (name, c['markers'], c['holes'], c['mism'], c['gate_resets'], len(diffs(r, ref))))
        cf, ct = fix['c'], twin['c']
        verdict(cf['holes'] == 0 and cf['unconsumed'] == 0 and cf['ord_mism'] == 0, 'F1 fix: holes %d, unconsumed %d, ordinal mismatches %d' % (cf['holes'], cf['unconsumed'], cf['ord_mism']))
        verdict(cf['markers'] < ct['markers'], 'F1: the rule withholds marks after the gap: markers fix %d < twin %d (the twin skips objects nobody compared during the off stretch)' % (cf['markers'], ct['markers']))
        verdict(fix['refused'] is not None and fix['refused'] > 0 and fix['unproved'] == 0, 'F1 rule on: marks withheld until re-proved after the gap %s, marks published although not re-proved since the chains resumed %s (must be 0)' % (fix['refused'], fix['unproved']))
        verdict(twin['unproved'] is not None and twin['unproved'] > 0, 'F1 twin (the first rework): marks published although not re-proved since the chains resumed %s (> 0: skips with a template nobody compared during the off stretch; the stream-mode picture cannot show it: its packets carry the stream\'s own banks)' % twin['unproved'])
        # F2
        lag = dict(D3_AUTO_THROTTLE='1', D3_AUTO_LAG='3', D3_AUTO_SCHED='0:on,50:off,56:on,120:off,126:on,200:off,203:on')   # short off stretches (under the gate's 250 ms): the marks and the proven gate are still there at the edge
        f2 = arm('f2', lb, lag)
        f2t = arm('f2twin', lb, dict(lag, D3_NO_MARKER_RULE='1'))
        print('INFO  F2 rule on: markers on a chain-off frame %s, holes %d, unconsumed %d; twin: %s, holes %d, unconsumed %d' % (f2['off_markers'], f2['c']['holes'], f2['c']['unconsumed'], f2t['off_markers'], f2t['c']['holes'], f2t['c']['unconsumed']))
        verdict(f2['off_markers'] == 0 and f2['c']['holes'] == 0 and f2['c']['unconsumed'] == 0, 'F2 rule on: markers on a chain-off frame %s (must be 0), holes %d, unconsumed %d' % (f2['off_markers'], f2['c']['holes'], f2['c']['unconsumed']))
        verdict(f2t['off_markers'] is not None and f2t['off_markers'] > 0, 'F2 twin (the timing window alone): markers on a chain-off frame %s (> 0: the silent hole)' % f2t['off_markers'])
    except RuntimeError as e:
        print('FAIL  ' + str(e))
        return 1
    print('%d bar(s) failed' % fails if fails else 'all bars passed')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
