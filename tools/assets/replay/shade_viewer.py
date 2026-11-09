"""Frame replay S4 (Session 98): a numpy port of the Arena Viewer's wrestler pixel shader (src\\assets\\view\\renderer.cpp kHlsl, ps(),
the `flags2.y > 0.5 && outCurve.z < 0.5` branch), term by term with the same tap names as shade_scene.py, evaluated on the GAME's
per-pixel inputs (rdraw.Draw) so the two can be compared term by term.

Space: the HLSL works in the file's space for the shading (N = the inward file normal, Vf = surface -> eye), which is the game's
interpolator space too (vs_ch_reflect.glsl: out_normal = the skinned file normal, out_view = c72 - world). So the game's in_normal /
in_view go in as they are; the viewer's own Y-flipped vectors never appear.

cb = the DrawCB as a dict of float4 lists (bank_dump field names: mat_amb, mat_dif, ldir[4], lcol[4], sky, ground, limit, hl_dir,
hl_col, sss1, sss2, refc, wspec, wmisc, wnrm, wdet, ...). `rt` = runtime stand-ins the viewer makes itself: lit (its shadow map),
rim_map (its rim light map term), dist (|eye - wp|); each defaults to the game's value when given in `game_rt`.
"""
import numpy as np


def _v(cb, k, i=None):
    a = np.array(cb[k] if i is None else cb[k][i], dtype=np.float64)
    return a


def prelight(cb, d):
    """HLSL PreLight(d): the analytic ambient cube (rgb) and the glow luminance."""
    def lobe(c, thr, width, root_off, off, scale):
        s = c - thr
        folded = np.clip(np.abs(s) + thr, 0, 1)
        m = -width * (1.0 - folded) + root_off
        side = np.sign(s)
        t = (-np.sqrt(np.abs(m)) - off) * scale
        return (1.0 - 0.5 * t * t) - 0.5, side
    w1, s1, w2, s2, lum = [], [], [], [], []
    for k in range(4):
        c = d @ _v(cb, 'ldir', k)[:3]
        a, sa = lobe(c, 0.166666701, 4.0 * -0.333333403, 0.25, -0.5, -1.5)
        b, sb = lobe(c, 0.409090906, 4.0 * -0.4909091, 0.00999999978, -0.100000001, -1.01851797)
        w1.append(a); s1.append(sa); w2.append(b); s2.append(sb)
        lum.append(np.dot(_v(cb, 'plcol' if 'plcol' in cb else 'lcol', k)[:3], [0.755379379, 0.236693099, 0.0079275202]))
    w1, s1, w2, s2 = (np.stack(x, -1) for x in (w1, s1, w2, s2))
    lum = np.array(lum)
    k2 = w2 * s2 + 0.5
    p = w1 * s1 + 0.5
    q = w1 * s1 + (w2 * s2 + 1.0)
    e2 = q * 0.5 - k2
    e1 = q * 0.5 - p
    pl = p * lum
    total = pl.sum(-1)
    share = np.where(total > 0, pl.max(-1) / np.where(total > 0, total, 1), 0.0)
    # (c16..c19.x = viewer plCol[k].w: --plfall first, else a viewer bank's plCol w, else 0)
    if 'plfall' in cb:
        fall = np.array([float(_v(cb, 'plfall')[k]) for k in range(4)])
    elif 'plcol' in cb and len(_v(cb, 'plcol', 0)) > 3:
        fall = np.array([float(_v(cb, 'plcol', k)[3]) for k in range(4)])
    else:
        fall = np.zeros(4)
    keep = 1.0 - share[:, None] * (1.0 - fall)[None, :]
    shaded = e1 * keep + p
    weight = e2 * keep + k2
    # S5: the cube's colours are the previous frame's (renderer.cpp plCol / plScale); without them, this frame's
    pk = 'plcol' if 'plcol' in cb else 'lcol'
    scale = _v(cb, 'plscale')[:3] if 'plscale' in cb else np.ones(3)
    glow = (shaded @ lum) * scale[2]
    col = sum(weight[:, k:k + 1] * _v(cb, pk, k)[:3] for k in range(4)) * scale
    return col, glow


def wrestler(draw, cb, uv, tex_rgb, nm=None, vcol=None, lit=None, rim_map=None, dist=None, cube=None, over=None, tangent=None, glow_lum=None, refl=None):
    """The viewer's wrestler branch. nm = the normal-map sample [n,4] (None = no normal map), vcol = vertex colour [n,4]
    (i.c), lit / rim_map / dist / cube = runtime stand-ins ([n] / [n] / [n] / [n,3] = min(cube, limit) already)."""
    over = over or {}
    n = len(uv)
    N = draw.I(0)[:, :3].copy()
    N /= np.maximum(np.linalg.norm(N, axis=-1, keepdims=True), 1e-12)
    Vf = draw.I(2)[:, :3]
    Vf = Vf / np.maximum(np.linalg.norm(Vf, axis=-1, keepdims=True), 1e-12)
    wnrm, wmisc, wspec = _v(cb, 'wnrm'), _v(cb, 'wmisc'), _v(cb, 'wspec')
    if wnrm[2] > 0.5 and nm is not None:
        u = 1.0 - 2.0 * nm[:, 1]
        v = 1.0 - 2.0 * nm[:, 0]
        if wnrm[0] < 1.5 and tangent is not None:  # the skin family: the vertex tangent, B = N x T
            T = tangent / np.maximum(np.linalg.norm(tangent, axis=-1, keepdims=True), 1e-12)
            B = np.cross(N, T)
        else:
            ih = 1 / np.sqrt(np.maximum(N[:, 2] ** 2 + N[:, 0] ** 2, 1e-12))
            T = np.stack([N[:, 2] * ih, np.zeros(n), -N[:, 0] * ih], -1)
            B = np.stack([N[:, 1] * T[:, 2], T[:, 0] * N[:, 2] - T[:, 2] * N[:, 0], -N[:, 1] * T[:, 0]], -1)
        m = T * v[:, None] + B * u[:, None] + N
        N = m / np.maximum(np.linalg.norm(m, axis=-1, keepdims=True), 1e-12)
    lit = np.ones(n) if lit is None else lit
    if cube is None:
        pc, glow_lum = prelight(cb, N)
        cube = np.minimum(pc, _v(cb, 'limit')[:3])
    elif glow_lum is None:
        glow_lum = np.zeros(n)
    sky, ground = _v(cb, 'sky')[:3], _v(cb, 'ground')[:3]
    sky_gnd = sky + (ground - sky) * (0.5 - 0.5 * N[:, 1])[:, None]
    lit_amb = cube * lit[:, None]
    if wmisc[2] > 0.5:
        s1, s2 = _v(cb, 'sss1'), _v(cb, 'sss2')
        glow = glow_lum * lit
        gc = (s2[:3] + (s1[:3] - s2[:3]) * glow[:, None]) * s1[3]
        tint = s2[:3] + (1 - s2[:3]) * lit[:, None]
        lit_amb = cube * lit[:, None] * tint * (1 - gc) + gc * glow[:, None]
    light = sky_gnd * _v(cb, 'mat_amb')[:3] + lit_amb
    base = tex_rgb * _v(cb, 'mat_dif')[:3] * light
    fl2 = _v(cb, 'flags2')
    mask = np.full(n, 0.5) if fl2[2] > 1.5 else (nm[:, 3] if (fl2[2] > 0.5 and nm is not None) else np.zeros(n))
    nv = np.sum(N * Vf, -1)
    edge = 1 - np.abs(nv)
    hs = N + Vf
    hdir = hs / np.maximum(np.linalg.norm(hs, axis=-1, keepdims=True), 1e-12)
    dist = np.full(n, 500.0) if dist is None else dist
    atten = 1.0 + np.clip((dist - 500.0) / 2000.0, 0, 1) * (0.4 - 1.0)
    sp = (np.maximum(hdir @ _v(cb, 'hl_dir')[:3], 0) * np.maximum(edge, 1e-6) ** wmisc[1])[:, None] * _v(cb, 'hl_col')[:3] * wmisc[0] * atten[:, None]
    rim_map = np.ones(n) if rim_map is None else rim_map
    rim_str = ((mask - 1) * 0.83 + 1) * edge * rim_map
    body = sp * (0.75 + 0.25 * base) * rim_str[:, None] + base
    R = Vf - N * (2 * nv)[:, None]
    dots = np.maximum(np.stack([R @ _v(cb, 'ldir', k)[:3] for k in range(4)], -1), 1e-30)
    wts = np.array([_v(cb, 'lcol', k)[3] for k in range(4)])
    lobes_a = (dots ** wspec[1]) @ wts
    lobes_b = (dots ** wspec[3]) @ wts
    gain_a = vcol[:, 0] if (wnrm[3] > 0.5 and vcol is not None) else 1.0
    spec_light = mask * wspec[0] * lobes_a * gain_a + mask * wspec[2] * lobes_b
    if refl is not None:  # (oracle: the game's reflection total injected)
        spec_light = refl
    refc = _v(cb, 'refc')
    sky_up = np.maximum(-R[:, 1] * np.maximum(1 + nv, 1e-6) ** refc[3], 0)
    env = (spec_light[:, None] + sky_up[:, None] * refc[:3] * mask[:, None]) * lit[:, None]
    if fl2[3] > 0.5:
        env = sky_up[:, None] * refc[:3] * mask[:, None] * lit[:, None]
    col = body * (1 - np.minimum(env, 1)) + env
    taps = {'albedo': tex_rgb, 'mask_w': mask, 'lit': lit, 'cube': cube, 'sky_ground': sky_gnd, 'light': light, 'base': base,
            'spec': sp, 'rim_strength': rim_str, 'body': body, 'reflection': spec_light, 'env': env, 'sky_up': sky_up, 'out': col,
            'N': N, 'view': Vf, 'ndv': nv}
    return col, taps
