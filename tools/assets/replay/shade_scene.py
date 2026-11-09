"""Frame replay S4 (Session 98): numpy ports of our scene shaders (src\\scene\\*.frag, hand-written, proven equal to the game's
translated shaders by tools\\shaders\\harness), term by term, with taps. Each function takes a rdraw.Draw (per-pixel interpolators,
the guest constants by register, the captured textures) and returns (rgb, alpha, taps). Optional overrides replace one runtime
input (e.g. lit=1, env=0) so the oracle can say which term carries an error.
"""
import numpy as np

REFLECT_REGS = [0, 1, 2, 3, 4, 8, 9, 10, 11, 12, 142, 144, 161, 162, 166, 168, 169, 170, 180, 181, 182, 187, 189, 190, 195, 197,
                204, 205, 218, 247, 253, 254, 255]

# texture binds in the translation (rdc_draw meta: name xe_texture<fetch>_...) -> fetch constant
def bind_of(draw, fetch):
    for b, t in draw.tex.items():
        n = t['meta']['name']
        if n.startswith('xe_texture%d_' % fetch):
            return b
    return None


def dot3(a, b):
    return np.sum(a[:, :3] * b[:, :3], axis=-1) if b.ndim == 2 else a[:, :3] @ b[:3]


def inv_len(sq):
    a = np.abs(sq)
    return np.where(a > 0, 1 / np.sqrt(np.maximum(a, 1e-300)), 0.0)


def pow_abs(x, p):
    with np.errstate(divide='ignore', invalid='ignore'):
        lg = np.log2(np.abs(x))
        m = np.where((p == 0) | (lg == 0), 0.0, p * lg)
        return np.exp2(m)


def reflect_entrance(draw, over=None, aniso=False):
    """src\\scene\\wrestler_reflect_entrance.frag (guest PS BBB2E028DCF36E8D)."""
    over = over or {}
    c = draw.ps_c
    n = len(draw.clip_w)
    in_normal, in_uv = draw.I(0), draw.I(1)
    view = draw.I(2)[:, :3]
    sp = draw.I(3)
    lp = draw.I(5)
    vtx = draw.I(6)
    envd = draw.I(8)
    uv = in_uv[:, :2]
    albedo = draw.sample2d(bind_of(draw, 0), uv, aniso=aniso)[:, :3]
    normal1 = draw.sample2d(bind_of(draw, 1), uv, aniso=aniso)
    env_mask = draw.sample2d(bind_of(draw, 6), uv, aniso=aniso)[:, 1]
    lightmap = draw.sample2d(bind_of(draw, 15), lp[:, :2])[:, :2]
    K_EPS, K_ONE, K_M1, K_HALF = c[255][0], c[254][0], c[254][3], c[253][0]
    vn = in_normal[:, :3]
    nh = np.stack([vn[:, 2], vn[:, 0] * K_M1], -1)
    ih = inv_len(np.sum(nh * nh, -1))
    tx = vn[:, 2] * ih
    tz = (vn[:, 0] * K_M1) * ih
    tangent = np.stack([tx, np.zeros(n), tz], -1)
    bitangent = np.stack([vn[:, 1] * tz, tx * vn[:, 2] - tz * vn[:, 0], -(vn[:, 1] * tx)], -1)
    bu = normal1[:, 1] * c[254][1] + K_ONE
    bv = normal1[:, 0] * c[254][1] + K_ONE
    m = tangent * (c[218][0] * bv)[:, None] + bitangent * (c[218][1] * bu)[:, None] + vn * c[218][2]
    normal = m * inv_len(np.sum(m * m, -1))[:, None]
    ndv = np.sum(normal * view, -1)
    refl = view - normal * (2 * ndv)[:, None]
    # 3. shadow
    ndl = np.maximum(normal @ c[166][:3], K_EPS)
    with np.errstate(divide='ignore', invalid='ignore'):
        ssc = 1.0 / sp[:, 3]
    suv = np.stack([sp[:, 0] * ssc, sp[:, 1] * ssc], -1)
    occ = draw.sample2d(bind_of(draw, 12), np.nan_to_num(suv), clamp=True, lod_from_uv=False)
    recv = np.clip((sp[:, 2] - c[168][0]) * (1.0 / (c[168][1] - c[168][0])) - c[168][2], 0, 1)
    dd = recv[:, None] - occ
    sat_x = np.clip(dd[:, 0] * c[169][2], 0, 1); sat_z = np.clip(dd[:, 2] * c[169][2], 0, 1)
    shade_x = K_ONE - pow_abs(K_ONE - sat_x, c[169][3]); shade_z = K_ONE - pow_abs(K_ONE - sat_z, c[169][3])
    sat_y = np.clip((dd[:, 1] - c[170][0]) * c[170][1], 0, 1); sat_w = np.clip((dd[:, 3] - c[170][0]) * c[170][1], 0, 1)
    pow_y = pow_abs(K_ONE - sat_y, c[170][2]); pow_w = pow_abs(K_ONE - sat_w, c[170][2])
    sa = (shade_x * pow_y - shade_x) * c[170][3] + shade_x
    sb = (shade_z * pow_w - shade_z) * c[170][3] + shade_z
    samt = np.maximum(sa, sb)
    sfade = K_ONE - (K_ONE - ndl) * c[169][1]
    lit = K_ONE - (c[168][3] * samt) * sfade
    if 'lit' in over:
        lit = np.broadcast_to(over['lit'], lit.shape).astype(np.float64)
    # 4. light
    cube_raw = draw.sample_cube(bind_of(draw, 13), normal)[:, :3]
    amb = np.minimum(cube_raw * c[197][:3], c[187][:3])
    if 'cube' in over:
        amb = over['cube']
    up = -(normal @ c[180][:3])
    up_mix = K_HALF * up + K_HALF
    sky_ground = (c[182][:3] - c[181][:3]) * up_mix[:, None] + c[181][:3]
    light = amb * lit[:, None] + sky_ground * (c[247][2] * c[189][:3])
    base = ((albedo * c[190][:3]) * vtx[:, :3]) * light
    # 5.
    hs = (normal + view) * K_HALF
    hd = hs * inv_len(np.sum(hs * hs, -1))[:, None]
    sdot = np.maximum(hd @ c[12][:3], K_EPS)
    edge = K_ONE - np.abs(ndv)
    edge_pow = pow_abs(edge, c[205][1])
    spec = (sdot * edge_pow)[:, None] * c[4][:3] * c[205][0] * lp[:, 3:4]
    spec_mix = (base + spec) * c[253][2] - base + (base * (spec + K_ONE)) * c[253][3]
    lm_term = np.maximum(lightmap[:, 1] - lp[:, 2], K_EPS) * c[162][2] + lightmap[:, 0]
    if 'rimmap' in over:
        lm_term = np.broadcast_to(over['rimmap'], lm_term.shape).astype(np.float64)
    lm_pow = pow_abs(lm_term, c[161][0])
    smm = (normal1[:, 3] * K_ONE + K_M1) * c[161][1] + c[253][1]
    rim_strength = (smm * (edge * lm_pow)) * c[161][2]
    body = spec_mix * rim_strength[:, None] + base
    dots = np.stack([refl @ c[r][:3] for r in (8, 9, 10, 11)], -1)
    logs = np.log2(np.abs(np.maximum(dots, K_EPS)))
    wts = np.array([c[0][3], c[1][3], c[2][3], c[3][3]])
    def lobes(width):
        return np.sum(np.exp2(np.where((logs == 0) | (width == 0), 0.0, logs * width)) * wts, -1)
    reflection = (normal1[:, 3] * c[142][0]) * lobes(c[142][1]) + (normal1[:, 3] * c[142][2]) * lobes(c[142][3]) + K_EPS
    ed = np.stack([envd[:, 0], envd[:, 1] * K_M1, envd[:, 2]], -1)
    env_tex = draw.sample_cube(bind_of(draw, 10), ed)[:, :3] * env_mask[:, None]
    if 'env' in over:
        env_tex = env_tex * over['env']
    sky_rim = pow_abs(K_ONE + ndv, c[204][3])
    sky_up = np.maximum((-refl[:, 1]) * sky_rim, K_EPS)
    env = reflection[:, None] * c[144][:3] + ((sky_up[:, None] * c[204][:3]) * normal1[:, 3:4] + env_tex)
    env_lit = env * lit[:, None]
    color = body * (K_ONE - np.minimum(env_lit, K_ONE)) + env_lit
    color = (c[195][:3] - color) * in_uv[:, 2:3] + color
    taps = {'albedo': albedo, 'mask_w': normal1[:, 3], 'env_mask': env_mask, 'lit': lit, 'cube': amb, 'sky_ground': sky_ground,
            'light': light, 'base': base, 'spec': spec, 'rim_strength': rim_strength, 'lm_term': lm_term, 'body': body,
            'reflection': reflection, 'env_tex': env_tex, 'sky_up': sky_up, 'env': env, 'out': color, 'vtx': vtx[:, :3],
            'N': normal, 'view': view, 'ndv': ndv, 'fog': in_uv[:, 2]}
    return color, vtx[:, 3], taps


def _shadow(draw, c, normal, sp, sadd, K_ONE, K_EPS):
    """Section 3 of the skin / head / body / reflect shaders: the shadow map `lit` (sadd = in_shadow_add.x or None)."""
    ndl = np.maximum(normal @ c[166][:3], K_EPS)
    with np.errstate(divide='ignore', invalid='ignore'):
        ssc = 1.0 / sp[:, 3]
    suv = np.nan_to_num(np.stack([sp[:, 0] * ssc, sp[:, 1] * ssc], -1))
    occ = draw.sample2d(bind_of(draw, 12), suv, clamp=True, lod_from_uv=False)
    recv = np.clip((sp[:, 2] - c[168][0]) * (1.0 / (c[168][1] - c[168][0])) - c[168][2], 0, 1)
    dd = recv[:, None] - occ
    sat_x = np.clip(dd[:, 0] * c[169][2], 0, 1); sat_z = np.clip(dd[:, 2] * c[169][2], 0, 1)
    shade_x = K_ONE - pow_abs(K_ONE - sat_x, c[169][3]); shade_z = K_ONE - pow_abs(K_ONE - sat_z, c[169][3])
    sat_y = np.clip((dd[:, 1] - c[170][0]) * c[170][1], 0, 1); sat_w = np.clip((dd[:, 3] - c[170][0]) * c[170][1], 0, 1)
    pow_y = pow_abs(K_ONE - sat_y, c[170][2]); pow_w = pow_abs(K_ONE - sat_w, c[170][2])
    sa = (shade_x * pow_y - shade_x) * c[170][3] + shade_x
    sb = (shade_z * pow_w - shade_z) * c[170][3] + shade_z
    samt = np.maximum(sa, sb)
    sfade = K_ONE - (K_ONE - ndl) * c[169][1]
    if sadd is None:
        return K_ONE - (c[168][3] * samt) * sfade
    return K_ONE - np.clip((c[168][3] * samt) * sfade + sadd, 0, 1)


def _lobes(refl, c, logs_width):
    dots = np.stack([refl @ c[r][:3] for r in (8, 9, 10, 11)], -1)
    with np.errstate(divide='ignore'):
        logs = np.log2(np.abs(np.maximum(dots, c[253][0] * 0)))
    wts = np.array([c[0][3], c[1][3], c[2][3], c[3][3]])
    with np.errstate(invalid='ignore'):
        return np.nan_to_num(np.sum(np.exp2(logs * logs_width) * wts, -1))


def skin_entrance(draw, over=None, aniso=False):
    """src\\scene\\wrestler_skin_entrance.frag (guest PS 91BF021CA7C0C1A5); the bump / paint second normal is evaluated without the
    four mask taps' gradient (paint_light uses the base normal where the bump weight is on: an approximation, tapped as 'paint')."""
    over = over or {}
    c = draw.ps_c
    n = len(draw.clip_w)
    vn = draw.I(0)[:, :3]
    in_uv = draw.I(1)
    view = draw.I(2)[:, :3]
    sp, sadd, lp, tg, vtx, rg = draw.I(3), draw.I(4)[:, 0], draw.I(5), draw.I(6)[:, :3], draw.I(7), draw.I(9)[:, 0]
    uv = in_uv[:, :2]
    K_EPS, K_ONE, K_HALF, K_M1 = c[253][0], c[254][0], c[254][2], c[254][3]
    albedo = draw.sample2d(bind_of(draw, 0), uv, bias=c[201][3], aniso=aniso)[:, :3]
    normal1 = draw.sample2d(bind_of(draw, 1), uv, aniso=aniso)
    masks = draw.sample2d(bind_of(draw, 14), uv, aniso=aniso)[:, :3]
    zone_w = draw.sample2d(bind_of(draw, 6), uv, aniso=aniso)[:, 3]
    lightmap = draw.sample2d(bind_of(draw, 15), lp[:, :2])[:, :2]
    paint = masks[:, 0]
    detail_zone = masks[:, 1] * zone_w
    zone_weight = c[210][0] * detail_zone
    bump_weight = (paint * c[234][0] - zone_weight) * paint + zone_weight
    spec_level = (c[205][2] - c[205][3]) * masks[:, 2] + c[205][3]
    spec_mask = detail_zone * (spec_level * (K_ONE - paint) - normal1[:, 3]) + normal1[:, 3]
    tint = (c[246][:3] + K_M1) * paint[:, None] + K_ONE
    diffuse = (tint * (albedo * c[190][:3])) * vtx[:, :3]
    bt = np.cross(vn, tg)
    bu = normal1[:, 1] * c[253][2] + K_ONE
    bv = normal1[:, 0] * c[253][2] + K_ONE
    m = tg * (c[218][0] * bv)[:, None] + bt * (c[218][1] * bu)[:, None] + vn * c[218][2]
    normal = m * inv_len(np.sum(m * m, -1))[:, None]
    ndv = np.sum(normal * view, -1)
    refl = view - normal * (2 * ndv)[:, None]
    lit = _shadow(draw, c, normal, sp, sadd, K_ONE, K_EPS)
    if 'lit' in over:
        lit = np.broadcast_to(over['lit'], lit.shape).astype(np.float64)
    amb_tex = draw.sample_cube(bind_of(draw, 13), normal)
    cube = np.minimum(amb_tex[:, :3] * c[197][:3], c[187][:3])
    if 'cube' in over:
        cube = over['cube']
    up = -(normal @ c[180][:3])
    sky_ground = (c[182][:3] - c[181][:3]) * (K_HALF * up + K_HALF)[:, None] + c[181][:3]
    glow = (c[199][0] * amb_tex[:, 3]) * lit
    glow_color = ((c[155][:3] - c[158][:3]) * glow[:, None] + c[158][:3]) * c[155][3]
    amb_tint = (K_ONE - c[158][:3]) * lit[:, None] + c[158][:3]
    lit_amb = (cube * lit[:, None]) * amb_tint * (K_ONE - glow_color) + glow_color * glow[:, None]
    light = sky_ground * c[189][:3] + lit_amb
    knee = np.minimum(c[156][0] * normal1[:, 3], K_ONE)[:, None]
    light_low = np.minimum(light, K_ONE)
    light_knee = (light - light_low) * (K_ONE - knee) + light_low
    lift = np.clip((light - c[156][1]) * knee, 0, 1)
    lit_color = diffuse * light_knee
    base = lift * (K_ONE - lit_color) + lit_color
    hs = (normal + view) * K_HALF
    hd = hs * inv_len(np.sum(hs * hs, -1))[:, None]
    sdot = np.maximum(hd @ c[12][:3], K_EPS)
    edge = K_ONE - np.abs(ndv)
    spec = (sdot * pow_abs(edge, c[205][1]))[:, None] * c[4][:3] * c[205][0] * lp[:, 3:4]
    spec_mix = (base + spec) * c[253][1] - base + (base * (spec + K_ONE)) * c[253][3]
    lm_term = np.maximum(lightmap[:, 1] - lp[:, 2], K_EPS) * c[162][2] + lightmap[:, 0]
    lm_pow = pow_abs(lm_term, c[161][0])
    smm = (normal1[:, 3] - K_ONE) * c[161][1] + K_ONE
    rim_strength = (smm * (edge * lm_pow)) * c[161][2]
    body = spec_mix * rim_strength[:, None] + base
    if over.get('_match'):
        total, rc = _skin_env_match(draw, c, refl, spec_mask, rg, bump_weight)
    else:
        spec_light = (spec_mask * c[142][0]) * _lobes(refl, c, c[142][1]) * rg + (spec_mask * c[142][2]) * _lobes(refl, c, c[142][3])
        paint_light = np.where(bump_weight > 0, bump_weight * _lobes(refl, c, c[210][1]), 0.0)
        total = spec_light + paint_light
    if 'reflection' in over:
        total = over['reflection']
    sky_up = np.maximum(-refl[:, 1] * pow_abs(K_ONE + ndv, c[204][3]), K_EPS)
    env = total[:, None] * c[144][:3] + (sky_up[:, None] * c[204][:3]) * normal1[:, 3:4]
    env_lit = env * lit[:, None]
    color = body * (K_ONE - np.minimum(env_lit, K_ONE)) + env_lit
    color = (c[195][:3] - color) * in_uv[:, 2:3] + color
    taps = {'albedo': albedo, 'mask_w': normal1[:, 3], 'lit': lit, 'cube': cube, 'sky_ground': sky_ground, 'light': light, 'base': base,
            'spec': spec, 'rim_strength': rim_strength, 'lm_term': lm_term, 'body': body, 'reflection': total, 'env': env, 'out': color,
            'glow': glow, 'glow_lum': c[199][0] * amb_tex[:, 3], 'vtx': vtx[:, :3], 'paint': paint, 'bump_weight': bump_weight, 'N': normal, 'tangent': tg, 'reflect_gain': rg}
    return color, vtx[:, 3], taps


def _reflect_cube(draw, c, refl):
    """The MATCH variants' reflection cube (fetch 11) along the reflected vector taken through the rows c52-c54 (all four channels)."""
    d = np.stack([refl @ c[52][:3], refl @ c[53][:3], refl @ c[54][:3]], -1)
    return draw.sample_cube(bind_of(draw, 11), d)


def skin_match(draw, over=None, aniso=False):
    """src\\scene\\wrestler_skin.frag (guest PS B08B5E734D48F5D0): skin_entrance with the reflection cube (fetch 11, x c197.w) in place
    of the four-light lobes; the paint reflection (cube .z) is taken along the base normal's reflection (the same approximation as
    skin_entrance's paint light)."""
    over = dict(over or {})
    col, a, t = skin_entrance(draw, dict(over, _match=True), aniso)
    return col, a, t


def head_match(draw, over=None, aniso=False):
    """src\\scene\\wrestler_head.frag (guest PS FEE45804EDD0EC05): head_entrance with the reflection cube in place of the lobes (same
    literal-bank layout: K_EPS c253.x, K_ONE c254.x, K_M1 c254.z, K_HALF c254.w)."""
    return head_entrance(draw, dict(over or {}, _match=True), aniso)


def body_match(draw, over=None, aniso=False):
    """src\\scene\\wrestler_body.frag (guest PS 1B66FF87A6433D53): body_entrance with the match literal bank (K_ONE c254.x, K_HALF
    c254.z, K_M1 c254.w) and the reflection cube in place of the lobes."""
    return body_entrance(draw, dict(over or {}, _match=True), aniso)


def _skin_env_match(draw, c, refl, spec_mask, rg, bump_weight):
    rc = _reflect_cube(draw, c, refl)
    refl_v = (rc[:, 0] * c[197][3]) * (spec_mask * c[142][0]) * rg + (rc[:, 1] * c[197][3]) * (spec_mask * c[142][2])
    paint = np.where(bump_weight > 0, (c[197][3] * rc[:, 2]) * bump_weight, 0.0)
    return refl_v + paint, rc


def _lobes_eps(refl, c, width, eps):
    """WeightedLobes of the body / head entrance shaders: log2 of the dots clamped at eps, the console's 0 x inf = 0 at width 0."""
    dots = np.stack([refl @ c[r][:3] for r in (8, 9, 10, 11)], -1)
    with np.errstate(divide='ignore', invalid='ignore'):
        logs = np.log2(np.abs(np.maximum(dots, eps)))
        lobe = np.exp2(np.where((width == 0) | (logs == 0), 0.0, logs * width))
    wts = np.array([c[0][3], c[1][3], c[2][3], c[3][3]])
    return np.nan_to_num(np.sum(lobe * wts, -1))


def _mask_taps(draw, bind, uv, c, aniso):
    """The four mask taps around uv (c255 offsets) of the body / head paint bump, as d0..d3 against the centre."""
    t = [draw.sample2d(bind, np.stack([uv[:, 0] + c[255][a], uv[:, 1] + c[255][b]], -1), aniso=aniso)[:, 0] for a, b in ((0, 1), (0, 2), (3, 2), (3, 1))]
    return t


def body_entrance(draw, over=None, aniso=False):
    """src\\scene\\wrestler_body_entrance.frag (guest PS B1F58C0AA0E3C83A: the gear / shirt / trunks / boots in entrances)."""
    over = over or {}
    c = draw.ps_c
    n = len(draw.clip_w)
    vn = draw.I(0)[:, :3]
    in_uv = draw.I(1)
    view = draw.I(2)[:, :3]
    blend = draw.I(3)[:, 0]
    sp, lp, vtx = draw.I(4), draw.I(6), draw.I(8)
    uv = in_uv[:, :2]
    K_EPS, K_TWO, K_M1, K_ONE, K_HALF = c[252][0], c[253][0], c[254][0], c[254][1], c[254][2]
    if over.get('_match'):
        K_ONE, K_HALF, K_M1 = c[254][0], c[254][2], c[254][3]
    albedo = draw.sample2d(bind_of(draw, 0), uv, aniso=aniso)[:, :3]
    detail_mask = draw.sample2d(bind_of(draw, 6), uv, aniso=aniso)[:, 2]
    paint_mask = draw.sample2d(bind_of(draw, 14), uv, aniso=aniso)[:, 0]
    normal1 = draw.sample2d(bind_of(draw, 1), uv, aniso=aniso)
    detail = draw.sample2d(bind_of(draw, 3), uv * c[206][0], aniso=aniso)
    lightmap = draw.sample2d(bind_of(draw, 15), lp[:, :2])[:, :2]
    detail_gain = (c[208][2] - c[206][2]) * blend + c[206][2]
    normal_gain = (c[208][1] - c[206][1]) * blend + c[206][1]
    light_detail_gain = (c[208][3] - c[206][3]) * blend + c[206][3]
    paint_sq = (c[234][0] * paint_mask) * paint_mask
    detail_keep = K_ONE - np.minimum(paint_sq, K_ONE)
    tint = (c[246][:3] + K_M1) * paint_mask[:, None] + K_ONE
    tinted = tint * albedo
    detail_signed = detail[:, 3] * K_TWO + K_M1
    diffuse = ((tinted * detail_signed[:, None]) * detail_gain[:, None]) * detail_mask[:, None] + tinted
    nh = np.stack([vn[:, 2] * K_ONE, vn[:, 0] * K_M1], -1)
    ih = inv_len(np.sum(nh * nh, -1) + K_EPS)
    tx = vn[:, 2] * ih
    tz = (vn[:, 0] * K_M1) * ih
    tangent = np.stack([tx, np.zeros(n), tz], -1)
    bitangent = np.stack([vn[:, 1] * tz, tx * vn[:, 2] - tz * vn[:, 0], -(vn[:, 1] * tx)], -1)
    def pert(u, v):
        m = tangent * (c[218][0] * v)[:, None] + bitangent * (c[218][1] * u)[:, None] + vn * c[218][2]
        return m * inv_len(np.sum(m * m, -1))[:, None]
    base_u = normal1[:, 1] * c[253][2] + K_ONE
    base_v = normal1[:, 0] * c[253][2] + K_ONE
    detail_u = ((detail[:, 1] * K_TWO + K_M1) * normal_gain * detail_keep) * detail_mask
    detail_v = ((detail[:, 0] * K_TWO + K_M1) * normal_gain * detail_keep) * detail_mask
    normal = pert(base_u + detail_u, base_v + detail_v)
    normal2 = normal
    on = paint_sq > 0
    if on.any():
        t0, t1, t2, t3 = _mask_taps(draw, bind_of(draw, 14), uv, c, aniso)
        d0, d1, d2, d3 = paint_mask - t0, paint_mask - t1, paint_mask - t2, paint_mask - t3
        gu = (d2 * K_M1 + d1 * K_M1 + d0) + d3
        gv = d3 * K_M1 + d2 * K_M1 + d1 * K_ONE + d0 * K_ONE
        normal2 = np.where(on[:, None], pert((detail_u - paint_mask * gu) + base_u, (detail_v - paint_mask * gv) + base_v), normal)
    ndv = np.sum(normal * view, -1)
    refl = view - normal * (2 * ndv)[:, None]
    n2dv = np.sum(normal2 * view, -1)
    refl2 = view - normal2 * (2 * n2dv)[:, None]
    lit = _shadow(draw, c, normal, sp, None, K_ONE, K_EPS)
    if 'lit' in over:
        lit = np.broadcast_to(over['lit'], lit.shape).astype(np.float64)
    cube = np.minimum(draw.sample_cube(bind_of(draw, 13), normal)[:, :3] * c[197][:3], c[187][:3])
    if 'cube' in over:
        cube = over['cube']
    up = -(normal @ c[180][:3])
    sky_ground = (c[182][:3] - c[181][:3]) * (K_HALF * up + K_HALF)[:, None] + c[181][:3]
    light = sky_ground * c[189][:3] + cube * lit[:, None]
    base = ((c[190][:3] * vtx[:, :3]) * diffuse) * light
    hs = (normal + view) * K_HALF
    hd = hs * inv_len(np.sum(hs * hs, -1))[:, None]
    sdot = np.maximum(hd @ c[12][:3], K_EPS)
    edge = K_ONE - np.abs(ndv)
    spec = (sdot * pow_abs(edge, c[205][1]))[:, None] * c[4][:3] * c[205][0] * lp[:, 3:4]
    spec_mix = (base + spec) * c[253][1] - base + (base * (spec + K_ONE)) * c[253][3]
    lm_term = np.maximum(lightmap[:, 1] - lp[:, 2], K_EPS) * c[162][2] + lightmap[:, 0]
    lm_pow = pow_abs(lm_term, c[161][0])
    smm = (normal1[:, 3] - K_ONE) * c[161][1] + K_ONE
    rim_strength = (smm * (edge * lm_pow)) * c[161][2]
    body = spec_mix * rim_strength[:, None] + base
    if over.get('_match'):
        rc = _reflect_cube(draw, c, refl)
        spec_light = (rc[:, 0] * c[197][3]) * (normal1[:, 3] * c[142][0]) + (rc[:, 1] * c[197][3]) * (normal1[:, 3] * c[142][2]) + K_EPS
        paint_light = np.where(on, (c[197][3] * _reflect_cube(draw, c, refl2)[:, 2]) * paint_sq, 0.0)
    else:
        spec_light = (normal1[:, 3] * c[142][0]) * _lobes_eps(refl, c, c[142][1], K_EPS) + (normal1[:, 3] * c[142][2]) * _lobes_eps(refl, c, c[142][3], K_EPS) + K_EPS
        paint_light = np.where(on, paint_sq * _lobes_eps(refl2, c, c[210][1], K_EPS), 0.0)
    total = spec_light + paint_light
    if 'reflection' in over:
        total = over['reflection']
    sky_up = np.maximum(-refl[:, 1] * pow_abs(K_ONE + ndv, c[204][3]), K_EPS)
    env = total[:, None] * c[144][:3] + (sky_up[:, None] * c[204][:3]) * normal1[:, 3:4]
    env_detail = (((env * detail_signed[:, None]) * light_detail_gain[:, None]) * detail_keep[:, None]) * detail_mask[:, None] + env
    env_lit = env_detail * lit[:, None]
    color = body * (K_ONE - np.minimum(env_lit, K_ONE)) + env_lit
    color = (c[195][:3] - color) * in_uv[:, 2:3] + color
    taps = {'albedo': albedo, 'mask_w': normal1[:, 3], 'lit': lit, 'cube': cube, 'sky_ground': sky_ground, 'light': light, 'base': base,
            'spec': spec, 'rim_strength': rim_strength, 'lm_term': lm_term, 'body': body, 'reflection': total, 'env': env_detail, 'out': color,
            'vtx': vtx[:, :3], 'N': normal, 'detail_signed': detail_signed, 'detail_mask': detail_mask, 'blend': blend, 'paint': paint_sq}
    return color, vtx[:, 3], taps


def head_entrance(draw, over=None, aniso=False):
    """src\\scene\\wrestler_head_entrance.frag (guest PS 1D20CB5D36B02E5B: the head / face skin in entrances)."""
    over = over or {}
    c = draw.ps_c
    vn = draw.I(0)[:, :3]
    in_uv = draw.I(1)
    view = draw.I(2)[:, :3]
    sp, sadd, lp, tg, vtx, gloss = draw.I(3), draw.I(4)[:, 0], draw.I(5), draw.I(6)[:, :3], draw.I(7), draw.I(9)[:, 0]
    uv = in_uv[:, :2]
    K_EPS, K_ONE, K_M1, K_HALF = c[253][0], c[254][0], c[254][2], c[254][3]
    bt = np.cross(vn, tg)
    mask = draw.sample2d(bind_of(draw, 14), uv, aniso=aniso)[:, :3]
    paint_mask = mask[:, 0]
    rs = c[230][2:4] - c[230][0:2]
    with np.errstate(divide='ignore', invalid='ignore'):
        ruv = np.nan_to_num(np.stack([(uv[:, 0] - c[230][0]) * (1.0 / rs[0]), (uv[:, 1] - c[230][1]) * (1.0 / rs[1])], -1))
    albedo = draw.sample2d(bind_of(draw, 0), uv, bias=c[201][3], aniso=aniso)[:, :3]
    area = draw.sample2d(bind_of(draw, 6), uv, aniso=aniso)[:, 3]
    n2t = draw.sample2d(bind_of(draw, 5), ruv, aniso=aniso)[:, [3, 1]]
    n2t = np.where((in_uv[:, 3] != 0)[:, None], np.nan_to_num(n2t), 0.0)  # (weight 0: the guest skips the fetch; a missing / NaN tap must not leak)
    normal1 = draw.sample2d(bind_of(draw, 1), uv, aniso=aniso)
    lightmap = draw.sample2d(bind_of(draw, 15), lp[:, :2])[:, :2]
    area_band = mask[:, 1] * area
    spec_target = ((c[205][2] - c[205][3]) * mask[:, 2] + c[205][3]) * (K_ONE - paint_mask)
    spec_mask = area_band * (spec_target - normal1[:, 3]) + normal1[:, 3]
    area_share = c[210][0] * area_band
    paint_strength = (paint_mask * c[234][0] - area_share) * paint_mask + area_share
    on = paint_strength > 0
    tint = (c[246][:3] + K_M1) * paint_mask[:, None] + K_ONE
    diffuse = vtx[:, :3] * (tint * (albedo * c[190][:3]))
    def pert(u, v):
        m = tg * (c[218][0] * v)[:, None] + bt * (c[218][1] * u)[:, None] + vn * c[218][2]
        return m * inv_len(np.sum(m * m, -1))[:, None]
    base_u = normal1[:, 1] * c[253][3] + K_ONE
    base_v = normal1[:, 0] * c[253][3] + K_ONE
    second_u = (n2t[:, 0] * 2) * K_M1 + K_ONE
    second_v = (n2t[:, 1] * 2) * K_M1 + K_ONE
    u = second_u * in_uv[:, 3] + base_u
    v = second_v * in_uv[:, 3] + base_v
    normal = pert(u, v)
    normal2 = normal
    if on.any():
        t0, t1, t2, t3 = _mask_taps(draw, bind_of(draw, 14), uv, c, aniso)
        ptex = draw.sample2d(bind_of(draw, 4), uv * c[210][2], aniso=aniso)[:, :2]
        d0, d1, d2, d3 = paint_mask - t0, paint_mask - t1, paint_mask - t2, paint_mask - t3
        gu = (d2 * K_M1 + d1 * K_M1 + d0) + d3
        gv = d3 * K_M1 + d2 * K_M1 + d1 * K_ONE + d0 * K_ONE
        pu = (ptex[:, 1] * 2) * K_M1 + K_ONE
        pv = (ptex[:, 0] * 2) * K_ONE + K_M1
        normal2 = np.where(on[:, None], pert((area_band * pu + u) + gu * -paint_mask, (area_band * pv + v) + gv * -paint_mask), normal)
    ndv = np.sum(normal * view, -1)
    refl = view - normal * (2 * ndv)[:, None]
    n2dv = np.sum(normal2 * view, -1)
    refl2 = view - normal2 * (2 * n2dv)[:, None]
    lit = _shadow(draw, c, normal, sp, sadd, K_ONE, K_EPS)
    if 'lit' in over:
        lit = np.broadcast_to(over['lit'], lit.shape).astype(np.float64)
    amb_tex = draw.sample_cube(bind_of(draw, 13), normal)
    cube = np.minimum(amb_tex[:, :3] * c[197][:3], c[187][:3])
    if 'cube' in over:
        cube = over['cube']
    up = -(normal @ c[180][:3])
    sky_ground = (c[182][:3] - c[181][:3]) * (K_HALF * up + K_HALF)[:, None] + c[181][:3]
    lit_floor = (K_ONE - c[158][:3]) * lit[:, None] + c[158][:3]
    shaded = (cube * lit[:, None]) * lit_floor
    glow = (c[199][0] * amb_tex[:, 3]) * lit
    warm = ((c[155][:3] - c[158][:3]) * glow[:, None] + c[158][:3]) * c[155][3]
    light = sky_ground * c[189][:3] + (shaded * (K_ONE - warm) + warm * glow[:, None])
    gw = np.minimum(c[156][0] * normal1[:, 3], K_ONE)[:, None]
    lc = np.minimum(light, K_ONE)
    light_soft = (light - lc) * (K_ONE - gw) + lc
    gl = np.clip((light - c[156][1]) * gw, 0, 1)
    lit_colour = diffuse * light_soft
    base = gl * (K_ONE - lit_colour) + lit_colour
    hs = (normal + view) * K_HALF
    hd = hs * inv_len(np.sum(hs * hs, -1))[:, None]
    sdot = np.maximum(hd @ c[12][:3], K_EPS)
    edge = K_ONE - np.abs(ndv)
    spec = (sdot * pow_abs(edge, c[205][1]))[:, None] * c[4][:3] * c[205][0] * lp[:, 3:4]
    spec_mix = (base + spec) * c[253][1] - base + (base * (spec + K_ONE)) * c[253][2]
    lm_term = np.maximum(lightmap[:, 1] - lp[:, 2], K_EPS) * c[162][2] + lightmap[:, 0]
    lm_pow = pow_abs(lm_term, c[161][0])
    smm = (normal1[:, 3] - K_ONE) * c[161][1] + K_ONE
    rim_strength = (smm * (edge * lm_pow)) * c[161][2]
    body = spec_mix * rim_strength[:, None] + base
    if over.get('_match'):
        rc = _reflect_cube(draw, c, refl)
        spec_light = ((rc[:, 0] * c[197][3]) * (spec_mask * c[142][0])) * gloss + (rc[:, 1] * c[197][3]) * (spec_mask * c[142][2])
        paint_light = np.where(on, (c[197][3] * _reflect_cube(draw, c, refl2)[:, 2]) * paint_strength, 0.0)
    else:
        spec_light = ((spec_mask * c[142][0]) * _lobes_eps(refl, c, c[142][1], K_EPS)) * gloss + (spec_mask * c[142][2]) * _lobes_eps(refl, c, c[142][3], K_EPS)
        paint_light = np.where(on, paint_strength * _lobes_eps(refl2, c, c[210][1], K_EPS), 0.0)
    total = spec_light + paint_light
    if 'reflection' in over:
        total = over['reflection']
    sky_up = np.maximum(-refl[:, 1] * pow_abs(K_ONE + ndv, c[204][3]), K_EPS)
    env = total[:, None] * c[144][:3] + (sky_up[:, None] * c[204][:3]) * normal1[:, 3:4]
    env_lit = env * lit[:, None]
    color = body * (K_ONE - np.minimum(env_lit, K_ONE)) + env_lit
    color = (c[195][:3] - color) * in_uv[:, 2:3] + color
    taps = {'albedo': albedo, 'mask_w': normal1[:, 3], 'lit': lit, 'cube': cube, 'sky_ground': sky_ground, 'light': light, 'base': base,
            'spec': spec, 'rim_strength': rim_strength, 'lm_term': lm_term, 'body': body, 'reflection': total, 'env': env, 'out': color,
            'glow': glow, 'glow_lum': c[199][0] * amb_tex[:, 3], 'vtx': vtx[:, :3], 'N': normal, 'tangent': tg, 'reflect_gain': gloss,
            'spec_mask': spec_mask, 'paint': paint_strength}
    return color, vtx[:, 3], taps
