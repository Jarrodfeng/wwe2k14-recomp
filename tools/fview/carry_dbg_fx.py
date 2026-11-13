"""Session 101 lane 210: make scratch copies of the fx folder whose skin pixel shader OUTPUTS shadow-term internals instead of a colour
(read with carry_dbg.py through fview --apply-reload yCh_skin,yCh_skin_flex --fx-dir; the repo's shaders are not touched).
  py -I tools\\fview\\carry_dbg_fx.py OUTDIR
Variants (colour = clamp(expr, 0, 1)): 1 gap.xyz*20; 2 (gap.w*20, receiver, map.x); 3 -gap.xyz*20 (gap = receiver - map; receiver = the normalised depth the skin shader computes);
4 (map.x, map.y, map.z); 5 (map.w, receiver, 0). Variant 6: the per-layer near / far parts: (near0, far0, w)."""
import pathlib, shutil, sys
src = pathlib.Path(__file__).resolve().parents[2] / 'src' / 'renderer' / 'shaders' / 'fx'
out = pathlib.Path(sys.argv[1])
EXPR = {
    1: 'dbgGap.xyz * 20.0',
    2: 'vec3(dbgGap.w * 20.0, dbgRecv, shadowMap.x)',
    3: '-dbgGap.xyz * 20.0',
    4: 'shadowMap.xyz',
    5: 'vec3(shadowMap.w, dbgRecv, 0.0)',
    7: 'vec3(0.25, 0.5, 0.75)',
    6: 'vec3(1.0 - pow(1.0 - clamp(dbgGap.x * fc.ShadowPrm[1].z, 0.0, 1.0), fc.ShadowPrm[1].w), pow(1.0 - clamp((dbgGap.y - fc.ShadowPrm[2].x) * fc.ShadowPrm[2].y, 0.0, 1.0), fc.ShadowPrm[2].z), fc.ShadowPrm[2].w)',
}
for v, e in EXPR.items():
    d = out / ('fxd%d' % v)
    if d.exists(): shutil.rmtree(d)
    shutil.copytree(src, d)
    p = d / 'fx_skin.glsl'
    t = p.read_text(encoding='utf-8')
    a = '  float amount = ch_shadow_amount(shadowMap, inShadow.z, fc.ShadowPrm[0], fc.ShadowPrm[1], fc.ShadowPrm[2], dot(n, fc.ViewLightDepth.xyz));\n'
    assert a in t
    t = t.replace(a, a + '  float dbgRecv = clamp((inShadow.z - fc.ShadowPrm[0].x) / (fc.ShadowPrm[0].y - fc.ShadowPrm[0].x) - fc.ShadowPrm[0].z, 0.0, 1.0);\n'
                  '  vec4 dbgGap = vec4(dbgRecv) - shadowMap;\n  dbgOut = ' + e + ';\n')
    b = '  outColor = vec4(color, inTint.w);\n'
    assert b in t
    c = '  float lit = 1.0;\n#ifdef FX_SHADOW'
    assert c in t
    t = t.replace(c, '  float lit = 1.0;\n  vec3 dbgOut = vec3(0.0);\n#ifdef FX_SHADOW')
    t = t.replace(b, '  outColor = vec4(clamp(dbgOut, 0.0, 1.0), 1.0);\n')
    p.write_text(t, encoding='utf-8')
print('wrote', len(EXPR), 'variants under', out)
