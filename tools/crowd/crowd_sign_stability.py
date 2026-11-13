r"""Lane 234 (S101, docs\renderer\CROWD_WIDE.md "Lane 234"): do the crowd's signs / foam hands change frame to frame? Offline, 0 game runs, numbers only.

  py -I tools\crowd\crowd_sign_stability.py LABEL=CAPTURE [LABEL=CAPTURE ...] [--density 70] [--frames A:B:STEP] [--selftest]
CAPTURE = a case zip or a capture folder (relative paths from the main checkout), as cards_census.py.

For every frame: the sign / foam-hand draws (call sites 0x82518D60 / 0x82518E68 / 0x825192D0 / 0x825194B4, docs\systems\crowd.md "Sign / foam-hand draws"), each one's world
position (c76.w, c77.w, c78.w of its own vertex-constant bank: the words the binder's density gate hashes, binder.h CrowdRank) and its game clip x (ViewProj c56-c59).
Printed per capture:
  * the draws per frame (min / mean / max) and the share beyond the game's clip |x| 1.0 (the stands' screen test never applies to them),
  * pair stability, frame j to j+1: how many of the draws keep their EXACT position words, how many have a draw within 2 units (the sign moved a little), how many have none
    (the sign appeared / left the frame);
  * with --density D (< 100): the binder's density gate (CrowdRank of the exact words >= D leaves the draw out) applied to every draw; per pair the kept draws that were
    OUT (or in) one frame later: 'flip' = the same sign (a draw within 2 units) kept in one frame and left out in the next. A static sign never flips; a flipping one is the flicker.
    The same with the rank taken from a position SNAPPED to a 100-unit cell (the plant's twin for the fix, 'snap') and from the nearest sign of the previous frame ('track').
Exit 0 always for the numbers; --selftest runs the plants (a moving sign flips under the exact rank, not under a snapped one) and exits 1 when a plant is not caught."""
import argparse
import collections
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cards_census as cc  # noqa: E402
from cards_census import pcap  # noqa: E402

SIGN_SITES = (0x82518D60, 0x82518E68, 0x825192D0, 0x825194B4)
SITE_SETS = {'signs': SIGN_SITES, 'blocks': (0x82518574,), 'cards': (0x825130C4,), 'floor': (0x8276ED60,)}
NEAR = 2.0
SITES = SIGN_SITES


def rank(w3, w7, w11):
    """binder.h CrowdRank, on uint32 words."""
    m = np.uint64(0xFFFFFFFF)
    h = ((w3.astype(np.uint64) * np.uint64(0x9E3779B1)) ^ (w7.astype(np.uint64) * np.uint64(0x85EBCA77)) ^ (w11.astype(np.uint64) * np.uint64(0xC2B2AE3D))) & m
    h ^= h >> np.uint64(16)
    h = (h * np.uint64(0x7FEB352D)) & m
    h ^= h >> np.uint64(15)
    h = (h * np.uint64(0x846CA68B)) & m
    h ^= h >> np.uint64(16)
    return (h % np.uint64(100)).astype(np.int32)


def rank_pos(pos):
    return rank(pos[:, 0].astype('<f4').view('<u4'), pos[:, 1].astype('<f4').view('<u4'), pos[:, 2].astype('<f4').view('<u4'))


def rank_snap(pos, cell=100.0):
    return rank_pos(np.floor(pos / cell) * cell)


def signs_of_frame(cap, fi):
    f = cap.frame(fi)
    D = f.draws
    if not len(D) or not f.has_origins:
        return None
    site = f.origins['call_site']
    sel = np.nonzero(np.isin(site, SITES))[0]
    pos = np.zeros((len(sel), 3), np.float32)
    clipx = np.zeros(len(sel), np.float32)
    for j, d in enumerate(sel):
        b = f.bank(int(D['vs_bank'][d]))
        w = cc.fl(b, cc.R_WORLD, 3).reshape(3, 4)
        pos[j] = (w[0, 3], w[1, 3], w[2, 3])
        vp = cc.fl(b, cc.R_VP, 4).reshape(4, 4)
        p4 = np.array([pos[j, 0], pos[j, 1], pos[j, 2], 1.0], np.float32)
        c = p4 @ vp.T
        clipx[j] = c[0] / c[3] if c[3] > 1e-6 else 99.0
    return pos, clipx, site[sel].astype(np.int64)


def nearest(a, b):
    """For each row of a, the distance to the nearest row of b (inf when b is empty)."""
    if not len(b):
        return np.full(len(a), np.inf), np.full(len(a), -1)
    d = np.linalg.norm(a[:, None, :] - b[None, :, :], axis=2)
    return d.min(axis=1), d.argmin(axis=1)


def analyse(seq, density):
    """seq = [(pos, clipx, site)] per frame -> a result dict."""
    res = collections.Counter()
    counts = [len(s[0]) for s in seq]
    res['frames'] = len(seq)
    res['draws_min'], res['draws_max'], res['draws_mean'] = min(counts), max(counts), round(float(np.mean(counts)), 2)
    res['beyond_1'] = int(sum((np.abs(s[1]) > 1.0).sum() for s in seq))
    res['beyond_1_15'] = int(sum((np.abs(s[1]) > 1.15).sum() for s in seq))
    res['draws_total'] = int(sum(counts))
    for (p0, _, _), (p1, _, _) in zip(seq, seq[1:]):
        if not len(p0):
            continue
        exact = int(sum((p1 == p).all(axis=1).any() for p in p0)) if len(p1) else 0
        dist, idx = nearest(p0, p1)
        res['pairs_draws'] += len(p0)
        res['pairs_exact'] += exact
        res['pairs_near'] += int((dist <= NEAR).sum())
        res['pairs_gone'] += int((dist > NEAR).sum())
        if density < 100:
            for name, fn in (('exact', rank_pos), ('snap', rank_snap)):
                k0, k1 = fn(p0) < density, fn(p1) < density
                near = dist <= NEAR
                flips = int(((k0 != k1[idx]) & near).sum()) if len(p1) else 0
                res['flip_' + name] += flips
            # track: the next frame's sign inherits the rank of its nearest previous-frame sign (the SDK's gpu_scene_crowd_density_track idea)
            k0, k1 = rank_pos(p0) < density, None
            if len(p1):
                d10, i10 = nearest(p1, p0)
                r1 = np.where(d10 <= 24.0, rank_pos(p0)[i10], rank_pos(p1))
                k1 = r1 < density
                # compare the same signs: p0 -> nearest in p1
                res['flip_track'] += int(((k0 != k1[idx]) & (dist <= NEAR)).sum())
    return res


def selftest():
    rng = np.random.default_rng(1)
    base = rng.uniform(-500, 500, (40, 3)).astype(np.float32)
    # a moving sign set: every sign drifts a little each frame (an arm waving)
    seq = []
    for t in range(30):
        p = base + np.array([0.3 * t, 0.2 * np.sin(t), 0.1 * t], np.float32)
        seq.append((p, np.zeros(len(p), np.float32), np.zeros(len(p), np.int64)))
    r = analyse(seq, 70)
    ok = True
    if not r['flip_exact'] > 100:
        print('FAIL  selftest: a drifting sign set must flip under the exact rank (got %d)' % r['flip_exact'])
        ok = False
    if r['flip_snap'] > r['flip_exact'] // 4:
        print('FAIL  selftest: the snapped rank must flip far less (exact %d, snap %d)' % (r['flip_exact'], r['flip_snap']))
        ok = False
    static = [(base, np.zeros(len(base), np.float32), np.zeros(len(base), np.int64))] * 10
    rs = analyse(static, 70)
    if rs['flip_exact'] != 0:
        print('FAIL  selftest: a static set must not flip (got %d)' % rs['flip_exact'])
        ok = False
    print('%s  selftest: moving set exact flips %d, snapped %d, track %d; static set %d' % ('PASS' if ok else 'FAIL', r['flip_exact'], r['flip_snap'], r['flip_track'], rs['flip_exact']))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('captures', nargs='*')
    ap.add_argument('--density', type=int, default=100)
    ap.add_argument('--frames', default='')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--sites', choices=sorted(SITE_SETS), default='signs', help='the draw class to follow (default the signs; blocks / cards / floor = the siblings of the class audit)')
    a = ap.parse_args()
    global SITES
    SITES = SITE_SETS[a.sites]
    if a.selftest:
        return selftest()
    bad = False
    for spec in a.captures:
        label, _, path = spec.partition('=')
        if not path:
            label, path = os.path.basename(spec), spec
        if not os.path.isabs(path) and not os.path.exists(path):
            path = os.path.join(pcap.MAIN, path)
        try:
            cap = cc.Cap(path)
            sp = [int(x) if x else None for x in a.frames.split(':')] if a.frames else [0, None, 1]
            sp += [None] * (3 - len(sp))
            frames = list(range(sp[0] or 0, min(sp[1], len(cap.frames)) if sp[1] is not None else len(cap.frames), sp[2] or 1))
            seq = []
            for fi in frames:
                r = signs_of_frame(cap, fi)
                if r is not None:
                    seq.append(r)
        except (pcap.PError, cc.CensusError, OSError, KeyError) as e:
            print('FAIL  %-10s cannot read %s: %s' % (label, path, e))
            bad = True
            continue
        if len(seq) < 2:
            print('FAIL  %-10s %d frames with draw origins' % (label, len(seq)))
            bad = True
            continue
        r = analyse(seq, a.density)
        print('%-10s %s' % (label, ' '.join('%s=%s' % (k, v) for k, v in sorted(r.items()))))
    return 2 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
