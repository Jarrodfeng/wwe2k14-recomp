r"""Lane 234: dump a capture's crowd draw positions per frame for tools\crowd\crowd_rank_test.exe (offline, 0 runs).
  py -I tools\crowd\crowd_rank_dump.py CAPTURE OUT.bin [--sites signs|blocks|cards|floor|all] [--frames A:B:STEP]
File: per frame u32 n, then n x (f32 x, y, z) = the draws' world positions (c76.w c77.w c78.w), in draw order. Under out\ only (numbers about positions, no game data)."""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import crowd_sign_stability as s  # noqa: E402
import cards_census as cc  # noqa: E402
from cards_census import pcap  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument('capture')
ap.add_argument('out')
ap.add_argument('--sites', default='all')
ap.add_argument('--frames', default='')
a = ap.parse_args()
if a.sites == 'all':
    s.SITES = tuple(x for v in s.SITE_SETS.values() for x in v)
else:
    s.SITES = s.SITE_SETS[a.sites]
path = a.capture if os.path.isabs(a.capture) or os.path.exists(a.capture) else os.path.join(pcap.MAIN, a.capture)
cap = cc.Cap(path)
sp = [int(x) if x else None for x in a.frames.split(':')] if a.frames else [0, None, 1]
sp += [None] * (3 - len(sp))
frames = range(sp[0] or 0, min(sp[1], len(cap.frames)) if sp[1] is not None else len(cap.frames), sp[2] or 1)
os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
n_frames = 0
with open(a.out, 'wb') as fh:
    for fi in frames:
        r = s.signs_of_frame(cap, fi)
        if r is None:
            continue
        pos = r[0].astype('<f4')
        fh.write(struct.pack('<I', len(pos)) + pos.tobytes())
        n_frames += 1
print('wrote %d frames to %s' % (n_frames, a.out))
