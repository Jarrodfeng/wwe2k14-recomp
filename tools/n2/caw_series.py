"""Lane 117: per-frame row colours (VS reg 1) of the editor's list rows. 0 game runs."""
import sys, os, glob
sys.path.insert(0, os.path.dirname(__file__))
from caw_state import read_state
cap = sys.argv[1]; lo = int(sys.argv[2]); hi = int(sys.argv[3]); step = int(sys.argv[4]) if len(sys.argv) > 4 else 1
geom = open(os.path.join(cap, 'geometry.bin'), 'rb').read()
fs = sorted(glob.glob(os.path.join(cap, 'frames', 'frame_*.bin')))
for n in range(lo, hi + 1, step):
    gf, seen = read_state(cap, fs[n], geom)
    rows = [(t[:10], c[:3]) for d, t, x, y, c in seen if x == 144.0 and 100 <= y < 580]
    print(n, gf, ' | '.join('%s %s' % (t, ','.join('%.2f' % v for v in c)) for t, c in rows[:4]))
