#!/usr/bin/env python3
"""S102 R25-C2: the present-queue model's offline check (docs\\renderer\\PRESENT_DEPTH.md). 0 game runs.

  py -I tools\\surface\\present_queue_check.py [--root .]

1. Past-bug check: the model, run as today's path (max_queued 2, 3 images) on the committed bar4 match rows (tools\\surface\\fixtures\\bar4_match_rows.csv = frames 2987..3587
   of the Surface run, the columns the model reads), reproduces the recorded bench-rule p99 gap (25.20 ms) within 1.0 ms. A model that cannot is refused.
2. Properties of every replayed queue (D, N in 2/3, 3/3, 3/4, 4/4, 2/4): no frame presented or shown out of order; a dropped frame is never newer than a frame
   presented after it (the newest frames win).
3. The finding (kept true by this check): the deeper queue does NOT bring the p99 gap to the 21 ms the brief hoped for (best cell stays above 24 ms), because the tail
   gaps are render-work frames, not back-pressure. If a future model change makes it reach 21 ms, this check fails and the doc must be rewritten.
4. Planted twins (each must be flagged): an out-of-order present; a dropped newest frame; a model with no calibration reproduction (work doubled) that must fail check 1.
"""
import argparse, os, sys

p = argparse.ArgumentParser()
p.add_argument('--root', default='.')
a = p.parse_args()
root = os.path.abspath(a.root)
sys.path.insert(0, os.path.join(root, 'tools', 'surface'))
import present_queue_model as m

FIX = os.path.join(root, 'tools', 'surface', 'fixtures', 'bar4_match_rows.csv')


class Fail(Exception):
    pass


def newest_dropped(res):
    """True when a dropped frame is newer than a frame presented after it."""
    fr = res['frames']
    for i, (idx, t) in enumerate(fr):
        if t is None:
            later = [j for j, tt in fr[i + 1:] if tt is not None]
            if later and later[0] < idx:
                return True
    return False


try:
    if not os.path.isfile(FIX):
        raise Fail('cannot read ' + FIX)
    o = m.run(FIX, 2987, 3587)
    rec, base = o['recorded_p99'], o['cells'][(2, 3)]['p99_bench']
    if abs(rec - 25.2) > 0.1:
        raise Fail('the recorded p99 of the fixture is %.2f, expected 25.20' % rec)
    if abs(base - rec) > 1.0:
        raise Fail('the model at D2/N3 gives p99 %.2f, recorded %.2f: it does not reproduce today (> 1.0 ms)' % (base, rec))
    print('ok   past-bug check: model D2/N3 p99 %.2f vs recorded %.2f (within 1.0 ms), vblank phase %.2f' % (base, rec, o['phase']))
    tr = m.Trace(m.load_rows(FIX, 2987, 3587))
    for (d, n), c in o['cells'].items():
        if c['ooo']:
            raise Fail('D%d N%d: %d out-of-order presents' % (d, n, c['ooo']))
        res = m.replay(tr, d, n, tr.t[0] + o['phase'])
        if newest_dropped(res):
            raise Fail('D%d N%d dropped a frame newer than one presented after it' % (d, n))
    print('ok   no frame out of order and the newest frames win, in all %d cells' % len(o['cells']))
    best = min(c['p99_bench'] for k, c in o['cells'].items() if k != (2, 3))
    if best <= 21.0:
        raise Fail('the deeper queues reach p99 %.2f <= 21: the doc says they do not; rewrite PRESENT_DEPTH.md' % best)
    print('ok   finding holds: best deeper-queue p99 %.2f ms (> 21): the tail is render work, not back-pressure' % best)
    # planted twins
    res = m.replay(tr, 2, 3, tr.t[0] + o['phase'])
    bad = dict(res)
    bad['frames'] = list(res['frames'])
    pres = [i for i, (_, t) in enumerate(bad['frames']) if t is not None]
    i0, i1 = pres[10], pres[11]
    bad['frames'][i0], bad['frames'][i1] = bad['frames'][i1], bad['frames'][i0]
    if m.out_of_order(bad) == 0:
        raise Fail('plant out-of-order present: NOT caught')
    print('ok   plant caught: two presents swapped (out of order)')
    planted = {'frames': [(10, 1.0), (12, None), (11, 2.0)]}
    if not newest_dropped(planted):
        raise Fail('plant dropped newest frame: NOT caught')
    print('ok   plant caught: the newest frame dropped while an older one is shown')
    tr2 = m.Trace(m.load_rows(FIX, 2987, 3587))
    tr2.work = [w * 2.0 for w in tr2.work]
    r2 = m.replay(tr2, 2, 3, tr2.t[0] + o['phase'])
    if abs(m.bench_p99(tr2, r2['P']) - rec) <= 1.0:
        raise Fail('plant work doubled: the reproduction check would still pass')
    print('ok   plant caught: a model with doubled work does not reproduce the recorded p99 (%.2f)' % m.bench_p99(tr2, r2['P']))
except Fail as e:
    print('FAIL', e)
    sys.exit(1)
print('PASS: present-queue model reproduces today, queue properties hold, finding recorded, 3 planted twins caught')
