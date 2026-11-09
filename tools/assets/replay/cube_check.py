r"""Frame replay (Session 98): the viewer's analytic ambient cube (renderer.cpp PreLight, shade_viewer.prelight) against the game's
runtime ambient cube (the yPreLight pass's cube map, fetch 13, as the draw reads it x c197, capped at c187), along each covered
pixel's vertex normal, per captured draw. The viewer gets the game's own light block (c8-c11 directions, c0-c3 colours of the vertex
bank), so any difference is the cube's maths / inputs, not the viewer's constant feed.

  py -I tools\assets\replay\cube_check.py <dumped draw folder>... [--shaders out\s98\replay\shaders]
"""
import argparse
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rdraw  # noqa: E402
import shade_viewer as SV  # noqa: E402
import shader_map as SM  # noqa: E402
from shade_scene import bind_of  # noqa: E402


def check(path, shader_dir):
    meta = json.load(open(os.path.join(path, 'meta.json')))
    g, name, regs = SM.resolve(meta['ps_md5'], shader_dir)
    if not regs:
        return None
    order = sorted(regs, key=lambda r: regs[r])
    d = rdraw.Draw(path, order)
    xs, ys = d.px
    if len(xs) == 0:
        return None
    sel = d.changed()[ys, xs]
    if sel.sum() < 20:
        return None
    N = d.I(0)[:, :3]
    N = N / np.maximum(np.linalg.norm(N, axis=-1, keepdims=True), 1e-12)
    c = d.ps_c
    cb = {'ldir': [list(c[8 + k]) for k in range(4)], 'lcol': [list(d.vs_c[k][:3]) + [0.0] for k in range(4)]}
    pl, glow = SV.prelight(cb, N)
    pl = np.minimum(pl, c[187][:3])
    raw = d.sample_cube(bind_of(d, 13), N)
    game = np.minimum(raw[:, :3] * c[197][:3], c[187][:3])
    r = {'draw': os.path.basename(path), 'shader': name, 'px': int(sel.sum()),
         'game_cube': (game[sel].mean(0)).round(4).tolist(), 'viewer_prelight': (pl[sel].mean(0)).round(4).tolist(),
         'ratio': (pl[sel].mean(0) / np.maximum(game[sel].mean(0), 1e-6)).round(3).tolist(),
         'per_px_rel_err': float(np.mean(np.abs(pl[sel] - game[sel])) / max(game[sel].mean(), 1e-6)),
         'cube_alpha_x_c199': float((raw[sel, 3] * (c[199][0] if 199 in c else 1)).mean()), 'viewer_glow': float(glow[sel].mean()),
         'vs_c0_3': [np.round(d.vs_c[k][:3], 3).tolist() for k in range(4)], 'c197': np.round(c[197], 3).tolist()}
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('draws', nargs='+')
    ap.add_argument('--shaders', default=r'C:\Users\Jarrod\wwe2k14-recomp\out\s98\replay\shaders')
    a = ap.parse_args()
    for p in a.draws:
        r = check(p, a.shaders)
        if r:
            print(json.dumps(r))


if __name__ == '__main__':
    main()
