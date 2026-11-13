r"""Lane 176 (S101, docs\renderer\CROWD_WIDE.md section 4): where on the screen the game's own crowd draws land. Offline, 0 game runs, numbers only.

  py -I tools\crowd\crowd_wide_census.py LABEL=CAPTURE [LABEL=CAPTURE ...] [--frames A:B:STEP] [--k 1.3125] [--min-frames N] [--expect-wide] [--json OUT]
CAPTURE = a case zip (capture\ inside it) or a capture folder (relative paths are read from the main checkout), as cards_census.py.

For every frame: the stands members drawn (block draws at 0x82518574 and far-card draws at 0x825130C4), each one's world position (c76.w, c77.w, c78.w of its own
vertex-constant bank) projected by the frame's ViewProj (c56-c59) into the GAME's clip space. The game culls members to x in [-1, 1] (docs\renderer\CROWD_WIDE.md section 2:
sub_8250E110 / sub_8250DC10), so a capture of the game as it is has nothing beyond that. On a display k times wider the picture shows the game's x in [-k, k].
Printed per capture: frames, members per frame (the cost), the share of members by |x| band (<= 0.5, 0.5-1, 1-k, > k), the widest |x|, and the 'side strips'
= members whose centre is beyond |x| = 1.15 (the radius overshoot the game's own test allows): what only a widened cull can draw. The ViewProj convention (which register is which column) is taken
from the shader (yAudience.vert line 63: each register is a row).
Exit codes: 0 ok; with --expect-wide the capture must HAVE side-strip members (the milestone's pass: a 21:9 / 32:9 live capture after lane 176); without it the capture is
read as the BEFORE and must have none beyond a small margin (the member radius: |x| <= 1 + 0.15). 1 = that failed; 2 = an input cannot be read.
Planted twin: --plant widen_off reads the capture as if the hook was off (nothing changes in the data: the pass for --expect-wide on a 16:9 capture must FAIL)."""
import argparse
import collections
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cards_census as cc  # noqa: E402
from cards_census import pcap  # noqa: E402

MARGIN = 0.15  # a member's radius takes its centre a little beyond 1 and still draws (the game tests centre +- radius)


def parse_frames(spec, total):
    if not spec:
        return list(range(total))
    a = [int(x) if x else None for x in spec.split(':')]
    a += [None] * (3 - len(a))
    return list(range(a[0] or 0, min(a[1], total) if a[1] is not None else total, a[2] or 1))


SITE_FLOOR = 0x8276ED60
WHICH = 'stands'


def members_of_frame(cap, fi):
    """-> (world positions Nx3, ViewProj 4x4 as 4 register rows) of the frame's stands members (block + card draws)."""
    f = cap.frame(fi)
    D = f.draws
    if not len(D) or not f.has_origins:
        return None
    site = f.origins['call_site']
    sel = np.nonzero(site == SITE_FLOOR)[0] if WHICH == 'floor' else np.nonzero((site == cc.SITE_BLOCK) | (site == cc.SITE_CARD))[0]
    if not len(sel):
        return np.zeros((0, 3), np.float32), None
    banks = np.unique(D['vs_bank'][sel])
    rows = {int(i): f.bank(int(i)) for i in banks}
    pos = np.zeros((len(sel), 3), np.float32)
    vp = None
    for j, d in enumerate(sel):
        b = rows[int(D['vs_bank'][d])]
        w = cc.fl(b, cc.R_WORLD, 3).reshape(3, 4)
        pos[j] = (w[0, 3], w[1, 3], w[2, 3])
        if vp is None:
            vp = cc.fl(b, cc.R_VP, 4).reshape(4, 4).copy()
    return pos, vp


def project(pos, vp):
    """Clip x / w and w of each position. yAudience.vert: gl_Position = (dot(ViewProj[0], p), dot(ViewProj[1], p), dot(ViewProj[2], p), dot(ViewProj[3], p)) with p = (world, 1):
    each ViewProj register is a ROW of the matrix that multiplies the column vector p."""
    p4 = np.concatenate([pos, np.ones((len(pos), 1), np.float32)], axis=1)
    clip = p4 @ vp.T
    w = np.where(clip[:, 3] > 1e-6, clip[:, 3], 1e-6)
    front = float((clip[:, 3] > 0).mean()) if len(clip) else 1.0
    return clip[:, 0] / w, clip[:, 3], 'registers are rows', front


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('captures', nargs='+')
    ap.add_argument('--frames', default='')
    ap.add_argument('--k', type=float, default=21.0 / 9.0 / (16.0 / 9.0), help='the display factor the bands are named for (default 21:9)')
    ap.add_argument('--min-frames', type=int, default=1)
    ap.add_argument('--expect-wide', action='store_true')
    ap.add_argument('--plant', choices=('widen_off',))
    ap.add_argument('--floor', action='store_true', help='the seated floor people (draw site 0x8276ED60, box +-0.9) instead of the stands members')
    ap.add_argument('--json')
    a = ap.parse_args()
    global WHICH
    WHICH = 'floor' if a.floor else 'stands'
    out, fail, bad_input = {}, 0, False
    for spec in a.captures:
        label, _, path = spec.partition('=')
        if not path:
            label, path = os.path.basename(spec), spec
        if not os.path.isabs(path) and not os.path.exists(path):
            path = os.path.join(pcap.MAIN, path)
        try:
            cap = cc.Cap(path)
            frames = parse_frames(a.frames, len(cap.frames))
            nfr, per_frame, bands, widest, conv, fronts = 0, [], collections.Counter(), 0.0, collections.Counter(), []
            for fi in frames:
                r = members_of_frame(cap, fi)
                if r is None:
                    continue
                pos, vp = r
                if vp is None or not len(pos):
                    continue
                x, w, name, front = project(pos, vp)
                nfr += 1
                per_frame.append(len(pos))
                conv[name] += 1
                fronts.append(front)
                ax = np.abs(x[w > 0])
                widest = max(widest, float(ax.max()) if len(ax) else 0.0)
                bands['<=0.5'] += int((ax <= 0.5).sum())
                bands['0.5-1'] += int(((ax > 0.5) & (ax <= 1.0)).sum())
                bands['1-1.15'] += int(((ax > 1.0) & (ax <= 1.0 + MARGIN)).sum())
                bands['1.15-k'] += int(((ax > 1.0 + MARGIN) & (ax <= a.k)).sum())
                bands['>k'] += int((ax > a.k).sum())
        except (pcap.PError, cc.CensusError, OSError, KeyError) as e:
            print('FAIL  %-9s cannot read %s: %s' % (label, path, e))
            bad_input = True
            continue
        total = sum(bands.values()) or 1
        side = bands['1.15-k'] + bands['>k']
        o = dict(frames=nfr, members_per_frame_mean=round(float(np.mean(per_frame)), 1) if per_frame else 0, members_per_frame_max=max(per_frame) if per_frame else 0,
                 bands=dict(bands), widest_abs_x=round(widest, 3), side_strip_members=side, side_strip_share=round(side / total, 4), convention=dict(conv),
                 in_front_min=round(min(fronts), 4) if fronts else None)
        out[label] = o
        print('%-9s %4d frames  members/frame mean %.1f max %d   |x| <= 0.5: %d   0.5-1: %d   1-1.15 (radius): %d   1.15-%.3f: %d   > k: %d   widest |x| %.3f   side strips %d (%.2f%%)   [%s, in front >= %.3f]'
              % (label, nfr, o['members_per_frame_mean'], o['members_per_frame_max'], bands['<=0.5'], bands['0.5-1'], bands['1-1.15'], a.k, bands['1.15-k'], bands['>k'], widest, side,
                 100 * o['side_strip_share'], ','.join(conv) or '-', o['in_front_min'] or 0))
        if nfr < a.min_frames:
            print('FAIL  %-9s only %d frames with stands members (< %d)' % (label, nfr, a.min_frames))
            fail += 1
        elif a.expect_wide:
            ok = side > 0 and a.plant != 'widen_off'
            print('%s  %-9s expected side-strip members (the widened cull): %d' % ('PASS' if ok else 'FAIL', label, side))
            fail += not ok
        elif a.floor:
            print('INFO  %-9s floor people: the box is +-0.9 but the radius term (near seats) lets centres out to %.3f at 16:9; bands only, no pass/fail' % (label, widest))
        else:
            ok = widest <= 1.0 + MARGIN
            print('%s  %-9s the game as it is keeps members inside |x| <= 1 (+%.2f radius): widest %.3f' % ('PASS' if ok else 'FAIL', label, MARGIN, widest))
            fail += not ok
    if a.json:
        json.dump(out, open(a.json, 'w'), indent=1, sort_keys=True)
    return 2 if bad_input else (1 if fail else 0)


if __name__ == '__main__':
    sys.exit(main())
