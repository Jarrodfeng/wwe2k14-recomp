#!/usr/bin/env python3
"""Lane 257: which draws is an arena tile fault made of? Offline, hidden viewer, 0 game runs (needs the census viewer exe of this checkout).

  py -I tools\\filecensus\\diag.py --arena 29 --angle ring_back [--tag NAME] [--env K=V ...] [--effects a,b] [--keep-shots]

For one arena and one camera preset it shoots vk once and rdr several times (all in the viewer's hidden mode):
  full    the rdr picture (the census's own),
  skip:E  rdr with the effect class E left out (WWE_RDR_SKIP_EFFECT, front_end.cpp DebugEffectFilter), for every class the coverage list names,
and prints the tiles (8 x 6 grid of selftest.tile_stats) where rdr differs from vk by more than 40 (mean colour distance, the tile check's bar),
with the classes whose removal changes that tile by more than 10 ("owns" the tile). Output pictures stay under out\\s101\\filecensus\\<tag>\\ (game pictures: local only).
An --env K=V pair is set for the rdr process only (e.g. WWE_RDR_NO_SPEC=1)."""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import census  # noqa: E402
import build_util  # noqa: E402
selftest = census.selftest


def tile_mean_dist(a, b):
    out = []
    for r, g in zip(selftest.tile_stats(a), selftest.tile_stats(b)):
        out.append((abs(r[0] - g[0]) + abs(r[1] - g[1]) + abs(r[2] - g[2])) / 3.0)
    return out


def shoot(exe, game, backend, arena, angle, env, name, out):
    err = os.path.join(out, '%s.err.txt' % name)
    link = census.Link(exe, backend, game, 'diag_%d_%s' % (os.getpid(), name), err, dict(os.environ, WWE_RDR_VALIDATION='1', **env))
    shot = os.path.join(out, '%s.png' % name).replace(os.sep, '/')
    if os.path.exists(shot):
        os.remove(shot)
    try:
        link.start()
        r = link.send({'cmd': 'load', 'kind': 'arena', 'id': arena}, 240)
        if not r.get('ok', True) or r.get('error'):
            raise RuntimeError(r)
        link.send({'cmd': 'camera', 'preset': angle}, 60)
        link.send({'cmd': 'shot', 'w': census.SIZE[0], 'h': census.SIZE[1], 'path': shot}, 120)
    finally:
        link.stop()
    text = open(err, 'rb').read().decode('utf-8', 'replace')
    return shot, text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--arena', type=int, required=True)
    ap.add_argument('--angle', default='ring_back')
    ap.add_argument('--tag', default='diag')
    ap.add_argument('--env', action='append', default=[])
    ap.add_argument('--effects', default='')
    ap.add_argument('--exe', default='')
    ap.add_argument('--game', default=os.path.join(census.MAIN, 'game_data', 'extracted'))
    a = ap.parse_args()
    exe = a.exe or build_util.exe_path(census.ROOT)
    out = os.path.join(census.MAIN, 'out', 's101', 'filecensus', a.tag, 'arena_%d_%s' % (a.arena, a.angle))
    os.makedirs(out, exist_ok=True)
    env = {}
    for kv in a.env:
        k, _, v = kv.partition('=')
        env[k] = v
    t0 = time.time()
    vk_shot, _ = shoot(exe, a.game, 'vk', a.arena, a.angle, {}, 'vk', out)
    full_shot, text = shoot(exe, a.game, 'rdr', a.arena, a.angle, env, 'rdr_full', out)
    info = census.parse_rdr_text(text)
    vk, full = selftest.read_png(vk_shot), selftest.read_png(full_shot)
    ok, notes = selftest.compare(vk, full)
    print('arena %d %s: %s  %s' % (a.arena, a.angle, 'pass' if ok else 'TILE FAULT', notes[-1]))
    dist = tile_mean_dist(vk, full)
    bad = [i for i, d in enumerate(dist) if d > 40.0]
    print('tiles over 40 (index = ty*8+tx): %s' % ', '.join('%d(%.0f)' % (i, dist[i]) for i in bad))
    classes = [c for c in info['cov']] + [c for c in info['also'] if c not in info['cov']]
    if a.effects:
        classes = a.effects.split(',')
    print('classes in the rdr list: %s' % ', '.join('%s=%d draws%s' % (c, info['cov'].get(c, ('', 0))[1], ' (no effect yet)' if info['cov'].get(c, ('',))[0] == 'no effect yet' else '') for c in classes))
    owns = {}
    for c in classes:
        e = dict(env, WWE_RDR_SKIP_EFFECT=c)
        shot, _ = shoot(exe, a.game, 'rdr', a.arena, a.angle, e, 'rdr_skip_%s' % c, out)
        d = tile_mean_dist(full, selftest.read_png(shot))
        owns[c] = [i for i, x in enumerate(d) if x > 10.0]
        vk_after = tile_mean_dist(vk, selftest.read_png(shot))
        better = sum(1 for i in bad if vk_after[i] < dist[i] - 10.0)
        print('  skip %-18s changes %2d tiles; of the %d bad tiles it brings %d closer to vk  (bad tiles it covers: %s)' % (c, len(owns[c]), len(bad), better, [i for i in bad if i in owns[c]]))
    print('seconds %.0f; pictures under %s' % (time.time() - t0, out))


if __name__ == '__main__':
    main()
