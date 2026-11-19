r"""S103 lane F: is the game's order of see-through draws a sort by distance, or the submit (object) order?
  py -I tools\scene\f_order_stability.py CAPTURE_DIR [--step 5]
For each frame, the see-through objects (model-part draws after the crowd: yBG_Default family blend / zw=0 / additive) in draw order. A far-to-near
sort would permute them as the camera moves; the submit order keeps the same relative order of objects in every frame. Reports, over all frames:
how many frames the relative order of every pair of objects seen in both frames is unchanged, and whether the order is ascending by object address.
Numbers only."""
import argparse
import itertools
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'particles'))
import f_order_census as fo  # noqa: E402
import pcap  # noqa: E402

SEE = ('yBG_Default', 'yProp_Default', 'yProp_UVScroll', 'yProp_BumpMap', 'yBG_UVScroll', 'yCh_trans', 'yCh_Aniso_trans', 'yCh_cloth_trans')


def see_through_sequence(rows):
    """Objects of the late see-through group, in draw order (first appearance), after the last opaque crowd draw."""
    last_crowd = -1
    for k, r in enumerate(rows):
        if r['klass'] == 'crowd':
            last_crowd = k
    seq = []
    for r in rows[last_crowd + 1:]:
        if r['klass'] not in ('model_part', 'unnamed', '?'):
            continue
        if r['fx'].split('/')[0] not in SEE and r['vs'].split('/')[0] not in SEE:
            continue
        if r['object'] and r['object'] not in seq:
            seq.append(r['object'])
    return seq


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cap')
    ap.add_argument('--step', type=int, default=5)
    ap.add_argument('--plant', default='', help='reverse_odd: the sequences of odd frames are reversed (a distance-like sort): the check must FAIL')
    a = ap.parse_args()
    cap = pcap.Capture(a.cap)
    seqs = []
    for f in range(0, len(cap.frames), a.step):
        s = see_through_sequence(fo.draw_rows(cap.frame(f)))
        if len(s) >= 2:
            if a.plant == 'reverse_odd' and f % 2:
                s = s[::-1]
            seqs.append((f, s))
    pairs_same = pairs_diff = 0
    ascending = 0
    order = {}
    for f, s in seqs:
        if all(x < y for x, y in zip(s, s[1:])):
            ascending += 1
        for x, y in itertools.combinations(s, 2):
            key = (x, y)
            rev = (y, x)
            if rev in order:
                pairs_diff += 1
            else:
                order.setdefault(key, f)
                pairs_same += 1
    print('%s: frames with >= 2 see-through objects: %d; ascending by address in %d of them; pair relations kept %d, flipped %d' % (
        a.cap, len(seqs), ascending, pairs_same, pairs_diff))
    for f, s in seqs[:3]:
        print('  frame %d: %s' % (f, ' '.join('%08X' % x for x in s)))
    if not seqs:
        print('no frame with two see-through objects: the census read nothing')
        return 2
    return 1 if pairs_diff else 0


if __name__ == '__main__':
    sys.exit(main())
