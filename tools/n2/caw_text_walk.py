"""Lane 117: per-frame run-string sets across a case zip's frames; prints the frames where the set changes. 0 game runs."""
import sys, os, glob, collections
sys.path.insert(0, os.path.dirname(__file__))
from caw_text_read import frame, get_name

d = sys.argv[1]
prev = None
fs = sorted(glob.glob(os.path.join(d, 'frames', 'frame_*.bin')))
for p in fs:
    b, gf, secs, S, runs = frame(p)
    names = collections.Counter(get_name(S, s) for _, s, _, _ in runs)
    key = frozenset(names)
    if key != prev:
        print(os.path.basename(p), gf, len(runs), sorted(n for n in key if n is not None and len(n) > 1 or False)[:40])
        if prev is not None:
            print('   +', sorted(key - prev), '\n   -', sorted(prev - key))
        prev = key
