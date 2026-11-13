"""Lane 117: FrameWhere (section 68) + the widget string of one text draw, per frame. 0 game runs."""
import sys, os, struct, glob
sys.path.insert(0, os.path.dirname(__file__))
from caw_text_read import frame, get_name
cap = sys.argv[1]
fs = sorted(glob.glob(os.path.join(cap, 'frames', 'frame_*.bin')))
prev = None
for n in (0, 50, 120, 250):
    b, gf, secs, S, runs = frame(fs[n])
    if 68 in secs:
        es, cnt, o = secs[68]
        a, w, mid, ck, mc = struct.unpack_from('<IIIIQ', b, o)
        print(n, gf, 'where', get_name(S, a), '|', get_name(S, w), 'menu_id', hex(mid), 'clock', mc)
    else:
        print(n, 'no section 68')
