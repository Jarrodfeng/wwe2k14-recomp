"""Session 21: frame-hash verdict from several runs per arm (the A/A noise of replay runs is not zero).

    py tools/perf/hash_matrix.py --off RUN,RUN --on RUN,RUN [--dir out\\harness\\step21\\clips]

Per guest frame (whole-image hash):
  same        every run agrees;
  systematic  the off runs agree with each other, the on runs agree with each other, and off != on: the setting
              changes this frame (a FAIL for a setting that must not change pixels);
  noise       anything else (some run differs from another run of its own arm).
  unseen      an on run shows an image (whole-image hash) that no off run produced at that frame.
Some frames have two possible images whatever the settings (timing-sensitive: runs 1499-1511 in Session 21), so a
setting can shift which one comes up; 'systematic' frames whose on image was also produced by an off run are that
case. The verdict is on 'unseen'. Exit 0 when no frame is unseen. Numbers only; our own run output.
"""
import argparse
import collections
import sys

sys.path.insert(0, __import__('os').path.dirname(__file__))
from hash_compare import CLIPS, load_full, resolve  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--off', required=True)
    ap.add_argument('--on', required=True)
    ap.add_argument('--dir', default=CLIPS)
    a = ap.parse_args()
    offs = [load_full(resolve(r, a.dir)) for r in a.off.split(',')]
    ons = [load_full(resolve(r, a.dir)) for r in a.on.split(',')]
    frames = sorted(set.intersection(*[set(d) for d in offs + ons]))
    kinds = collections.Counter()
    systematic, noise_off, noise_on, unseen = [], [], [], []
    for f in frames:
        off_h = {d[f][0] for d in offs}
        on_h = {d[f][0] for d in ons}
        if on_h - off_h:
            unseen.append(f)
        if len(off_h) == 1 and len(on_h) == 1:
            if off_h == on_h:
                kinds['same'] += 1
            else:
                kinds['systematic'] += 1
                systematic.append(f)
        else:
            kinds['noise'] += 1
            if len(off_h) > 1:
                noise_off.append(f)
            if len(on_h) > 1:
                noise_on.append(f)
    print('off runs %s, on runs %s: %d common frames' % (a.off, a.on, len(frames)))
    print('same %d, noise %d (off runs disagree on %d, on runs on %d), systematic %d' % (
        kinds['same'], kinds['noise'], len(noise_off), len(noise_on), kinds['systematic']))
    if systematic:
        print('systematic frames: %s' % ', '.join(map(str, systematic[:40])))
    print('unseen %d (an on image no off run produced)%s' % (
        len(unseen), ': ' + ', '.join(map(str, unseen[:40])) if unseen else ''))
    print('VERDICT: %s' % ('NO NEW IMAGES' if not unseen else 'NEW IMAGES'))
    sys.exit(1 if unseen else 0)


if __name__ == '__main__':
    main()
