"""Lane 117: per frame, the CAW editor's title / rows / hot row, printed on change. 0 game runs."""
import sys, os, glob
sys.path.insert(0, os.path.dirname(__file__))
from caw_state import read_state, is_hot

cap = sys.argv[1]
geom = open(os.path.join(cap, 'geometry.bin'), 'rb').read()
fs = sorted(glob.glob(os.path.join(cap, 'frames', 'frame_*.bin')))
prev = None
hot_r = []
for n, p in enumerate(fs):
    gf, seen = read_state(cap, p, geom)
    title = [(t, x, y) for d, t, x, y, c in seen if y == 58.0 and x == 140.0]
    rows = [(t, c) for d, t, x, y, c in seen if x == 144.0 and 100 <= y < 580]
    hot = [t for t, c in rows if is_hot(c)]
    key = (tuple(t for t, _, _ in title), tuple(t for t, c in rows), tuple(hot))
    hot_r += [c[0] for t, c in rows if is_hot(c)]
    if key != prev:
        print('frame', n, 'guest', gf, 'title', [t for t, _, _ in title], 'hot', hot, 'rows', [t for t, c in rows])
        prev = key
if hot_r:
    print('hot red range', min(hot_r), max(hot_r), len(hot_r))
# stability: of the frames that have list rows, how many have exactly one hot row
cnt = {}
for n, p in enumerate(fs):
    gf, seen = read_state(cap, p, geom)
    rows = [(t, c) for d, t, x, y, c in seen if x == 144.0 and 100 <= y < 580]
    if rows:
        k = sum(1 for t, c in rows if is_hot(c))
        cnt[k] = cnt.get(k, 0) + 1
print('frames with rows, by number of hot rows:', cnt)
