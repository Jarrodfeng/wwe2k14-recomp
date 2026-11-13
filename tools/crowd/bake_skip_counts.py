r"""Lane 155 (S101): the card count of each person type in each frame of a capture, for the bake skip's replay (tools\crowd\bake_skip_test.exe --counts). Offline, 0 game runs.

  py -I tools\crowd\bake_skip_counts.py LABEL=CAPTURE [LABEL=CAPTURE ...] [--out DIR]
Writes DIR\LABEL.csv (default out\crowd_bake_skip\): one line a frame `frame,c0,...,c19` = the number of far-card draws (call site 0x825130C4) that bind the picture of person type k, where type k is the
k-th bake of the frame (CROWD_CARDS_PROMOTION_MAP 9.4: the k-th bake is type k, positional, on every frame with 20 bakes). A frame with no 20 bakes (no crowd, or a 15-type arena) gets zeros.
Counts only: nothing from the game is written. Also prints a one-line summary per capture."""
import argparse
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import cards_census as cc  # noqa: E402

pcap = cc.pcap


def frame_counts(cap, fi):
    f = cap.frame(fi)
    D = f.draws
    c = np.zeros(20, int)
    if not len(D):
        return c, 0
    veff = cap.effs(D['vs_hash'])
    copies_t = cc.table(f, cc.SEC_COPIES, cc.COPY_DT)
    dest = []
    for p in cc.walk(f, copies_t, cap):
        if p.size == (64, 64) and p.draws and all(veff[d] == cc.EFF_BAKE_VS for _, d in p.draws) and p.copies:
            dest.append(int(copies_t[p.copies[0][1]]['d']['image']))
    if not 1 <= len(dest) <= 20:
        return c, len(dest)
    if not f.has_origins:
        raise cc.CensusError('capture without draw origins (version 1)')
    cards = np.nonzero(f.origins['call_site'] == cc.SITE_CARD)[0]
    sl, tf, tc = f.slots, D['tex_first'].astype(np.int64), D['tex_count'].astype(np.int64)
    pos = {img: k for k, img in enumerate(dest)}
    for d in cards:
        for r in sl[int(tf[d]):int(tf[d]) + int(tc[d])]:
            if int(r['slot']) == 0 and int(r['image']) in pos:
                c[pos[int(r['image'])]] += 1
    return c, len(dest)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('captures', nargs='+')
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'crowd_bake_skip'))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    bad = False
    for spec in a.captures:
        label, _, path = spec.partition('=')
        if not path:
            label, path = os.path.basename(spec), spec
        if not os.path.isabs(path) and not os.path.exists(path):
            path = os.path.join(pcap.MAIN, path)
        try:
            cap = cc.Cap(path)
            rows, with20, with_cards = [], 0, 0
            for fi in range(len(cap.frames)):
                c, nb = frame_counts(cap, fi)
                with20 += nb >= 1
                with_cards += int(c.sum() > 0)
                rows.append('%d,%s,%d' % (cap.nums[fi], ','.join(str(int(x)) for x in c), nb))
        except (pcap.PError, cc.CensusError, OSError, KeyError, IndexError) as e:
            print('FAIL  %-9s cannot read %s: %s' % (label, path, e))
            bad = True
            continue
        with open(os.path.join(a.out, label + '.csv'), 'w') as fh:
            fh.write('# lane 155: frame, cards of person type 0..19, number of bake passes the frame has (counts only)\n' + '\n'.join(rows) + '\n')
        print('%-9s %d frames, %d with bakes, %d with cards -> %s' % (label, len(rows), with20, with_cards, os.path.join(a.out, label + '.csv')))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
