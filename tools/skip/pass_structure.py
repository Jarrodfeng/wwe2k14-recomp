r"""Lane 175 (S101): the render-pass structure census (the GPU side of the cut ranking; tile-based GPUs pay per target switch and per resolve). Offline, 0 game runs.

  py -I tools\skip\pass_structure.py CAPTURE.zip [--frames A:B:STEP]
Per frame, means over the frames read: SetTargets ops (= render passes on a tiler), distinct targets, switches back to a target already used earlier in the frame (a tiler flushes and reloads),
consecutive SetTargets to the SAME target (mergeable at once), copies (resolves), passes whose output the dead-work census (dead_passes.py) finds unread, tiny passes (<= 2 draws), and the
passes of the 64x64 / 384x64 / 512x512-and-smaller family. Counts only."""
import argparse
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dead_passes as dp  # noqa: E402

cc = dp.cc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('--frames', default='')
    a = ap.parse_args()
    cap = cc.Cap(a.capture)
    total = len(cap.frames)
    idx = list(range(total))
    if a.frames:
        s, e, st = (a.frames.split(':') + ['', '', ''])[:3]
        idx = list(range(int(s or 0), min(total, int(e or total)), int(st or 1)))
    per = dp.run(cap, idx)
    acc = collections.Counter()
    n = 0
    for fi, f, nodes, passes in per[:-1]:
        n += 1
        acc['passes'] += len(passes)
        seen, last = set(), None
        for p in passes:
            tgt = tuple(p['colors']) or (p['depth'],)
            if tgt == last:
                acc['same_target_consecutive'] += 1
            elif tgt in seen:
                acc['switch_back'] += 1
            seen.add(tgt)
            last = tgt
            draws = [o for o in p['ops'] if o['kind'] == 'draw']
            if len(draws) <= 2:
                acc['tiny_passes'] += 1
            if p['size'] and p['size'][0] <= 512 and p['size'][1] <= 512:
                acc['small_target_passes'] += 1
            if p['ops'] and all(not o['alive'] for o in p['ops']):
                acc['dead_passes'] += 1
        acc['distinct_targets'] += len(seen)
        acc['copies'] += sum(1 for o in nodes if o['kind'] == 'copy')
        acc['dead_copies'] += sum(1 for o in nodes if o['kind'] == 'copy' and not o['alive'])
        acc['draws'] += sum(1 for o in nodes if o['kind'] == 'draw')
    print('%s: %d frames' % (os.path.basename(a.capture), n))
    for k in ('passes', 'distinct_targets', 'switch_back', 'same_target_consecutive', 'tiny_passes', 'small_target_passes', 'dead_passes', 'copies', 'dead_copies', 'draws'):
        print('  %-26s %8.1f per frame' % (k, acc[k] / max(1, n)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
