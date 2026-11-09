r"""Frame replay S3 / S4 oracle (Session 98, 0 game runs): for one dumped draw (rdc_draw.py), evaluates on the game's own per-pixel
inputs (rdraw.py: the post-VS interpolators rasterised, the captured textures and constants):

  A   the viewer's maths with the viewer's constants (a bank_dump json of the same mesh) and the viewer's own stand-ins for the
      runtime maps (its analytic PreLight cube, lit = 1, rim map = 1)
  B   the viewer's maths with the GAME's constants (mapped like tools\assets\replay\regmap.json) and the same stand-ins
  Bi  B with the game's runtime inputs injected (the captured ambient cube x c197, the game's shadow `lit`, its rim-map term)
  Bc  Bi but with the viewer's own PreLight cube (the ambient cube alone)
  Br  (match shaders) Bi + the game's reflection-cube total in place of the viewer's four-light lobes
  S   our scene shader (src\scene, numpy port shade_scene.py) on everything captured = the game's maths
  G   the game's colour target after the draw (the capture)

Error split per material: inputs = A - B, runtime maps = B - Bi, maths = Bi - S, harness = S - G (should be ~0). Taps per term
(albedo, cube, sky_ground, light, base, spec, rim_strength, body, reflection / env, out) for the first term that differs.

  py -I tools\assets\replay\oracle.py --draw out\s98\replay\s3s4\rp_ringin\eyes\ev11078 --kind reflect
         [--bank out\...\bank.json --mesh-verts 578 --mesh-shader yCh_Reflect] [--out report.txt]
Match draws (wrestler_skin / _head / _body, Session 98 match oracle): their pixel bank lacks c0-c3 / c8-c11 (the reflection is a
cube), so pass the draw probe of the same run: --probe <probe.csv> --probe-swaps N,N+1 (the row with the same index count and the
nearest vertex bank fills the missing registers); --plfall 0,0,0.5,0 = the cube build's c16..c19.x for the viewer's PreLight.
Everything read and written stays under out\ (game data, rule 1).
"""
import argparse
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rdraw  # noqa: E402
import shade_scene as SS  # noqa: E402
import shade_viewer as SV  # noqa: E402
import shader_map as SM  # noqa: E402

KINDS = {'wrestler_reflect_entrance': SS.reflect_entrance, 'wrestler_skin_entrance': SS.skin_entrance,
         'wrestler_head_entrance': SS.head_entrance, 'wrestler_body_entrance': SS.body_entrance,
         'wrestler_skin': SS.skin_match, 'wrestler_head': SS.head_match, 'wrestler_body': SS.body_match}
TAPS = ['vtx', 'albedo', 'cube', 'lit', 'sky_ground', 'light', 'base', 'spec', 'rim_strength', 'body', 'reflection', 'env', 'out']


def game_cb(draw, viewer_cb=None, kind=''):
    """The game's bank in the viewer's DrawCB fields (regmap.json rows). Fields the game has no register for keep the viewer's
    value (flags, wnrm, wdet, wmisc.z)."""
    c, vs = draw.ps_c, draw.vs_c
    cb = json.loads(json.dumps(viewer_cb)) if viewer_cb else {
        'flags2': [0, 1, 1, 0], 'wnrm': [2, 0, 1, 0], 'wmisc': [0, 0, 0, 0], 'sss1': [0] * 4, 'sss2': [0] * 4}
    g = lambda r: [float(x) for x in c[r]] if r in c else None
    cb['ldir'] = [g(8 + k) or [0] * 4 for k in range(4)]
    cb['lcol'] = [[float(vs[k][0]), float(vs[k][1]), float(vs[k][2]), float(c[k][3]) if k in c else 0.0] for k in range(4)]
    for f, r in (('sky', 181), ('ground', 182), ('limit', 187), ('mat_amb', 189), ('mat_dif', 190), ('hl_col', 4), ('hl_dir', 12),
                 ('refc', 204), ('wspec', 142), ('sss1', 155), ('sss2', 158)):
        if g(r):
            cb[f] = g(r)
    if g(205):
        cb['wmisc'] = [g(205)[0], g(205)[1], cb['wmisc'][2], cb['wmisc'][3]]
    if not viewer_cb and ('skin' in kind or 'head' in kind):  # the viewer's skin-family switches (asset_load: wnrm.x 1, the LOD bias, the gain; glow on)
        cb['wnrm'] = [1, g(201)[3] if g(201) else 0, 1, 1]
        cb['wmisc'][2] = 1
    return cb


def find_viewer_mesh(bank, verts, shader):
    best = None
    for d in bank['draws']:
        if d.get('verts') == verts and shader in d.get('shader', ''):
            best = d
            if d.get('model', 0) != 0:
                break
    return best


def stats(x):
    return np.round(np.mean(x, 0) * (255 if x.ndim > 1 else 1), 3)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--draw', required=True)
    ap.add_argument('--shaders', default=r'C:\Users\Jarrod\wwe2k14-recomp\out\s98\replay\shaders')
    ap.add_argument('--bank')
    ap.add_argument('--mesh-verts', type=int)
    ap.add_argument('--mesh-shader', default='yCh_')
    ap.add_argument('--out')
    ap.add_argument('--probe', help='draw-probe CSV (consts 0-511) for the registers the capture lacks (match: c0-c3, c8-c11)')
    ap.add_argument('--plfall', help='c16..c19.x of the cube build for the viewer PreLight, e.g. 0,0,0.5,0 (match set 10)')
    ap.add_argument('--probe-swaps', help='comma list of probe swaps to search')
    a = ap.parse_args()
    meta = json.load(open(os.path.join(a.draw, 'meta.json')))
    guest, kind, regmap = SM.resolve(meta['ps_md5'], a.shaders)
    if kind not in KINDS:
        raise SystemExit('no numpy port of %s (%s) yet' % (kind, guest))
    scene_fn = KINDS[kind]
    d = rdraw.Draw(a.draw, sorted(regmap, key=lambda r: regmap[r]))
    xs, ys = d.px
    sel = d.changed()[ys, xs]
    G = d.game_pixels()[:, :3]
    lines = ['draw %s (%s, guest %s): %d pixels changed by the draw (of %d rasterised)' % (a.draw, kind, guest, sel.sum(), len(xs))]
    S, _, st = scene_fn(d)
    uv = d.I(1)[:, :2]
    alb = st['albedo']
    nm = d.sample2d(SS.bind_of(d, 1), uv)
    vb = None
    if a.bank:
        bank = json.load(open(a.bank))
        nverts = a.mesh_verts or meta['postvs']['bytes'] // meta['postvs']['stride']
        m = find_viewer_mesh(bank, nverts, a.mesh_shader)
        if m:
            vb = m['cb']
            lines.append('viewer mesh: model %d mesh %d key %s part %s shader %s' % (m['model'], m['mesh'], m.get('key'), m.get('part'), m.get('shader')))
    if a.probe:
        # The match variants do not load c0-c3 / c8-c11 (their reflection is a cube), so the capture's pixel bank lacks the light
        # directions / weights the viewer's maths need: take the missing pixel registers from the draw-probe row of the same mesh
        # (same index count, nearest vertex bank).
        import probe_rows
        rows = probe_rows.rows(a.probe, swaps=set(int(s) for s in a.probe_swaps.split(',')) if a.probe_swaps else None,
                               index_counts={meta['indices']})
        if rows:
            nv = min(len(d.vs_c), 256)
            best = min(rows, key=lambda r: float(np.abs(r[4][:nv] - d.vs_c[:nv]).sum()))
            dist_vs = float(np.abs(best[4][:nv] - d.vs_c[:nv]).mean())
            added = [r for r in range(256) if r not in d.ps_c]
            for r in added:
                d.ps_c[r] = best[5][r].astype(np.float64)
            lines.append('probe row: swap %d draw %d (vs bank mean |d| %.4g); %d pixel registers added from it' % (best[0], best[1], dist_vs, len(added)))
        else:
            lines.append('probe row: none with %d indices' % meta['indices'])
    gcb = game_cb(d, vb, kind)
    if a.plfall:  # the cube build's c16..c19.x (the light set's falloff, asset_model.h LightSet::fall) for the viewer's PreLight
        gcb['plfall'] = [float(x) for x in a.plfall.split(',')]
    vtx = np.repeat(st['reflect_gain'][:, None], 4, 1) if 'reflect_gain' in st else None  # (the viewer's i.c.r = the game's interpolator 9.x)
    tg = st.get('tangent')
    dist = d.clip_w  # (the viewer's |eye - wp|; the game's fog uses clip w)
    rim_game = SS.pow_abs(st['lm_term'], d.ps_c[161][0])
    arms = {}
    if vb:
        arms['A'] = SV.wrestler(d, vb, uv, alb, nm, vtx, dist=dist, tangent=tg)
    arms['B'] = SV.wrestler(d, gcb, uv, alb, nm, vtx, dist=dist, tangent=tg)
    arms['Bi'] = SV.wrestler(d, gcb, uv, alb, nm, vtx, lit=st['lit'], rim_map=rim_game, dist=dist, cube=st['cube'], tangent=tg, glow_lum=st.get('glow_lum'))
    # Bc: Bi with the viewer's own PreLight cube (+ glow) in place of the game's: the ambient cube alone
    arms['Bc'] = SV.wrestler(d, gcb, uv, alb, nm, vtx, lit=st['lit'], rim_map=rim_game, dist=dist, tangent=tg,
                             refl=st['reflection'] if kind in ('wrestler_skin', 'wrestler_head', 'wrestler_body') else None)
    if kind in ('wrestler_skin', 'wrestler_head', 'wrestler_body'):  # match: + the game's reflection-cube total (the viewer: lobes)
        arms['Br'] = SV.wrestler(d, gcb, uv, alb, nm, vtx, lit=st['lit'], rim_map=rim_game, dist=dist, cube=st['cube'], tangent=tg,
                                 glow_lum=st.get('glow_lum'), refl=st['reflection'])
    out = {k: rdraw.epilogue(d, v[0]) for k, v in arms.items()}
    out['S'] = rdraw.epilogue(d, S)
    lines.append('mean colour over the pixels (0-255), |x - G| mean:')
    for k in list(arms) + ['S']:
        lines.append('  %-3s %s  err %.2f' % (k, (out[k][sel].mean(0) * 255).round(1), np.abs(out[k] - G)[sel].mean() * 255))
    lines.append('  G   %s' % (G[sel].mean(0) * 255).round(1))
    lines.append('taps (mean over the pixels; scene = the game maths):')
    for t in TAPS:
        row = '  %-13s S %-28s' % (t, stats(st[t][sel]) if t in st else '-')
        for k, v in arms.items():
            tv = v[1].get(t)
            row += ' %s %-28s' % (k, stats(tv[sel]) if tv is not None else '-')
        lines.append(row)
    if vb:
        lines.append('constants: viewer vs game (fields that differ)')
        for f in sorted(gcb):
            vv, gg = np.array(vb.get(f), dtype=float), np.array(gcb[f], dtype=float)
            if vv.shape == gg.shape and not np.allclose(vv, gg, atol=1e-3):
                lines.append('  %-8s viewer %s game %s' % (f, np.round(vv, 3).tolist(), np.round(gg, 3).tolist()))
    txt = '\n'.join(lines)
    print(txt)
    if a.out:
        open(a.out, 'w').write(txt + '\n')


if __name__ == '__main__':
    main()
