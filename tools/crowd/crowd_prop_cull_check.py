#!/usr/bin/env python3
r"""S103 lane SG, offline, 0 game runs: do OUR sign / foam-hand draws cull like the game's?

  py -I tools\crowd\crowd_prop_cull_check.py [--capture DIR]

The game: the crowd entry sub_82519508 sets cull ON once (0x82519680 sub_82BB7C28(1) -> sub_82C24128: state id 56 = 2 or 6 by the current mode [state+116]) before the sign loop, the hand loop,
the node loop and the stands (claims_Q2 / V2). Which side that is in host terms is read from the STREAM: every draw the capture recorded at the sign / hand call sites (0x82518D60 / 0x825192D0)
and the stands' block draw (0x82518574) carries its translated state (DrawFixed.state: cull = VkCullModeFlags at byte 5, front_face = VkFrontFace at byte 6).
Check (the game's side only; our side is proved by the picture, tools\scene\d4_check.py: its oracle keeps only the clockwise-on-screen triangles and the planted props_cull_front twin must fail):
  stream  every such draw in the sampled frames has cull BACK (2) and front face CLOCKWISE (1)  [the game's state, translated by the adapter].
Exit 1 on any FAIL."""
import argparse
import collections
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'solver'))
SITES = {0x82518D60: 'sign', 0x825192D0: 'hand', 0x82518574: 'stands block'}


def main_checkout():
    try:
        common = subprocess.run(['git', '-C', REPO, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return REPO


def stream_states(capture, samples=12):
    from capread import Source, Frame
    src = Source(capture)
    cnt = collections.Counter()
    for fn in src.frames[::max(1, len(src.frames) // samples)]:
        buf = src.read(fn)
        f = Frame(buf)
        if f.origins is None:
            continue
        nsec = struct.unpack_from('<I', buf, 12)[0]
        secs = {}
        for i in range(nsec):
            sid, es, c, off = struct.unpack_from('<IIQQ', buf, 32 + 24 * i)
            secs[sid] = (es, c, off)
        if 7 not in secs:
            continue
        es, c, off = secs[7]
        site = f.origins['call_site']
        for d in range(len(f.draws)):
            s = int(site[d])
            if s in SITES:
                o = off + es * int(f.draws[d]['fixed'])
                cnt[(SITES[s], buf[o + 5], buf[o + 6])] += 1
    return cnt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture')
    a = ap.parse_args()
    main_co = main_checkout()
    capture = a.capture or os.path.join(main_co, 'out', 'captures', 'brock_losswin')
    bad = False
    try:
        cnt = stream_states(capture)
    except Exception as e:
        print('FAIL  stream   cannot read %s: %s' % (capture, e))
        return 1
    kinds = {k[0] for k in cnt}
    ok = bool(cnt) and {'sign', 'hand'} <= kinds and all(k[1] == 2 and k[2] == 1 for k in cnt)
    bad |= not ok
    print('%s  stream   %s' % ('PASS' if ok else 'FAIL', ', '.join('%s cull %d front %d: %d draws' % (k[0], k[1], k[2], n) for k, n in sorted(cnt.items())) or 'no prop draws found'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
