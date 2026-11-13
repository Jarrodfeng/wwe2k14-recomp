#!/usr/bin/env python3
"""Lane 275: headless viewer shots of the ring corners (the turnbuckle / post join), one arena, one exe, three views per corner pair. Offline, hidden viewer, 0 game runs. Pictures are game pictures: local only, under out\\.

  py -I tools\\filecensus\\corner_shots.py --exe EXE --arena N --tag before|after [--backend vk|rdr] [--outdir DIR]

Writes DIR\\arena_N_<tag>_<view>.png for the views corner0 / corner2 (orbit camera aimed at the game-space corner points (-293, -154, -293) and (293, -154, 293), 130 units away).
The viewer's space negates the game's Y (target y = +154)."""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import census  # noqa: E402

VIEWS = {   # name -> (target, yaw, pitch, dist)
    'corner0_a': ([-293, 175, -293], 200, 25, 130),
    'corner0_b': ([-293, 175, -293], 110, 25, 130),
    'corner2_a': ([293, 175, 293], 20, 25, 130),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--arena', type=int, required=True)
    ap.add_argument('--tag', required=True)
    ap.add_argument('--backend', default='vk')
    ap.add_argument('--outdir', default=os.path.join(census.MAIN, 'out', 's101', 'l275'))
    ap.add_argument('--game', default=os.path.join(census.MAIN, 'game_data', 'extracted'))
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    err = os.path.join(a.outdir, 'arena_%d_%s.err.txt' % (a.arena, a.tag))
    link = census.Link(a.exe, a.backend, a.game, 'l275_%d_%s' % (os.getpid(), a.tag), err, dict(os.environ))
    try:
        link.start()
        r = link.send({'cmd': 'load', 'kind': 'arena', 'id': a.arena}, 240)
        if not r.get('ok', True) or r.get('error'):
            raise RuntimeError(r)
        for name, (tgt, yaw, pitch, dist) in VIEWS.items():
            link.send({'cmd': 'camera', 'target': tgt, 'yaw': yaw, 'pitch': pitch, 'dist': dist, 'fov': 40}, 60)
            path = os.path.join(a.outdir, 'arena_%d_%s_%s.png' % (a.arena, a.tag, name)).replace(os.sep, '/')
            link.send({'cmd': 'shot', 'w': 960, 'h': 540, 'path': path}, 120)
            print(path)
    finally:
        link.stop()


if __name__ == '__main__':
    main()
