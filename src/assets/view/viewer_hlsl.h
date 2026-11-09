// Generated once from the viewer sources (Session 98 phone port, stage A): every HLSL shader of the Arena Viewer, ONE source
// for both backends. D3D11 compiles this text at run time (D3DCompile); the Vulkan backend uses SPIR-V built from the same
// text by tools\shaders\build_viewer_spv.py (DXC -spirv) into spirv\viewer_spv.h. Edit the shaders HERE, then run that script.
// Vulkan-only details sit behind __spirv__ (DXC defines it; FXC never sees them): explicit bindings / vertex input locations.
#pragma once
#include <string>

namespace view {
namespace hlsl {

inline const char* const kScene = R"HLSL(
#pragma pack_matrix(row_major)
// Vulkan (DXC -spirv, tools\shaders\build_viewer_spv.py): set 0 = per pass, set 1 = per draw; vertex inputs by location.
#ifdef __spirv__
#define VKB(b, s) [[vk::binding(b, s)]]
#define VKL(n) [[vk::location(n)]]
#else
#define VKB(b, s)
#define VKL(n)
#endif
VKB(0, 0) cbuffer Frame : register(b0) {
  float4x4 viewProj;
  float4 camPos;
  float4 light0;   // plain look: xyz = direction TO the light, w = intensity
  float4 light1;
  float4 ambient;
  float4 look;     // x = 1 lit look (Session 97), 0 plain look; y = exposure
  float4 outCurve; // x = 1: the game's display gamma (Session 97, below)
  float4 lmParam;  // stage lightmap: xy = world.xz scale (g_f4LightMapParam default 1/3500), z = strength, w = half-range factor (x2)
};
VKB(0, 1) cbuffer Draw : register(b1) {
  float4 offset;   // xyz = model offset, w = 1 when the mesh is skinned
  float4 difColor;
  float4 flags;    // x = has diffuse, y = has own lightmap, z = alpha test, w = 1 for the blended pass
  float4 flags2;   // x = use the stage lightmap, y = wrestler recipe, z = spec mask (0 none, 1 normal-map w, 2 constant), w = hair / translucent (1), crowd (2)
  float4 matAmb;   // g_f4MatAmbCol
  float4 matDif;   // g_f4MatDifCol
  float4 spec;     // rgb = specular colour, w = level
  float4 misc;     // x = specular power
  float4 lDir[4];  // directions TO the light
  float4 lCol[4];
  float4 sky;
  float4 ground;
  float4 limit;    // g_fvLightLimit
  float4 xform;    // Session 98 (scenes): x = sin, y = cos of the model's turn about Y (x' = x cos + z sin, z' = -x sin + z cos)
  // Session 98 (skin fix): the wrestler constants of the game's skin draw (probe run 6816; out\s98\skin_fix\_model.py). lCol[k].w = c0..c3.w (lobe weights).
  float4 hlDir;    // c12: the highlight light's direction (file space, the way the light travels)
  float4 hlCol;    // c4: its colour
  float4 sss1;     // c155 = g_f3SSSSinCol (glow colour at full strength; w = scale)
  float4 sss2;     // c158 = g_f3SSSSinCol2 (ambient tint / glow colour at zero)
  float4 refc;     // c204 = (g_f4Ref_COL.rgb, g_f4Ref_DOT): the sky rim colour and power
  float4 wspec;    // c142 = (g_fSpecLev, g_fSpecPow, g_fSpecLvBase, g_fSpecPwBase): the lobe weights / widths
  float4 wmisc;    // x = g_fFairy, y = g_fFairyEdge (c205.xy: highlight gain, edge power), z = 1 the skin glow is on
  // Session 98 (wrestler fix 3): the rest of the game's wrestler pixel maths (wrestler_skin_entrance.frag / wrestler_body_entrance.frag).
  float4 wnrm;     // x = family (1 skin: vertex tangent frame, 2 gear / reflect: horizontal frame), y = colour LOD bias (c201.w), z = 1 the
                   // normal map perturbs the normal, w = 1 the vertex colour's grey scales the first lobe set (skin interpolator 9)
  float4 wdet;     // x = detail tiling (c206.x), y / z = g_fNoiseTNear / Far (vertex c188), w = 1 the tiled detail is on (gear)
  float4 wdetA;    // near set (c206.yzw): x normal gain, y diffuse gain, z light gain
  float4 wdetB;    // far set (c208.yzw)
  // Session 98 (replay S5): the ambient cube's own light colours. The game's yPreLight pass of frame t is fed the PREVIOUS frame's light
  // colours (c0..c3 and c198 = 1 / their sum) while the wrestler reads it back x c197 = this frame's sum (capture rp_ringin, run 6818:
  // the cube texels = side_prelight of probe frame t's prelight row, whose colours equal the wrestler draws of frame t-1, to 1 / 255;
  // per pixel x1.01 after the lag vs x1.043 without). plCol = those colours (= lCol when the lights hold), plScale = sum(lCol) / sum(plCol).
  float4 plCol[4];
  float4 plScale;
};
VKB(2, 1) Texture2D tDiff : register(t0);
VKB(3, 1) Texture2D tLM : register(t1);
VKB(4, 1) Texture2D tStage : register(t2);
VKB(5, 1) Texture2D tNrm : register(t3);
VKB(6, 1) Texture2D tDet : register(t8);   // Session 98 (wrestler fix 3): g_mNoiseTilePrm (`noise2`)
VKB(7, 1) Texture2D tDetM : register(t9);  // g_mMaskPrm: z = where the detail applies
VKB(6, 0) SamplerState samp : register(s0);
VKB(7, 0) SamplerState sampClamp : register(s1);
// Session 97: the skinning palette (one row-vector 4x4 per bone; identity = the stored pose) and per-instance placement.
// Session 97: four float4 rows per bone, so the matrix layout never depends on a pack_matrix setting (the identity palette
// hid a transposed read: a posed wrestler came out scrambled).
VKB(8, 1) StructuredBuffer<float4> bones : register(t0);
float4x4 BoneM(uint k) { return float4x4(bones[4 * k], bones[4 * k + 1], bones[4 * k + 2], bones[4 * k + 3]); }
struct VSIn {
  VKL(0) float3 p : POSITION; VKL(1) float3 n : NORMAL; VKL(2) float2 uv0 : TEXCOORD0; VKL(3) float2 uv1 : TEXCOORD1; VKL(4) float4 c : COLOR;
  VKL(5) uint4 bi : BLENDINDICES; VKL(6) float4 bw : BLENDWEIGHT; VKL(7) float3 t : TANGENT;
  VKL(8) float4 r0 : INST0; VKL(9) float4 r1 : INST1; VKL(10) float4 r2 : INST2; VKL(11) float4 r3 : INST3;
};
struct VSOut { float4 pos : SV_Position; float3 n : NORMAL; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; float4 c : COLOR; float3 wp : TEXCOORD2; float4 lc : TEXCOORD3; float3 tg : TEXCOORD4; float card : TEXCOORD5; };
// Session 97 (look spec section 1a): the arena's per-vertex lighting = 4 directional lights + sky / ground hemisphere.
float3 LightSum(float3 n) {
  float3 s = 0;
  [unroll] for (int k = 0; k < 4; k++) s += max(dot(n, lDir[k].xyz), 0.0) * lCol[k].rgb;
  return s;
}
float3 Hemi(float3 n) { return lerp(sky.rgb, ground.rgb, 0.5 - 0.5 * n.y); }
// Session 98 (skin fix): the wrestlers' ambient cube, as the game's yPreLight pass fills it (src\scene\side_prelight.frag) and the skin
// shader reads it back (texel x c197; c198 = 1 / c197, so the pair cancels): for a direction d (file space, the inward normal), each of
// lights 0-3 gets a weight from two wrap-around lobes of its cosine (cos 1 -> 1, cos -1 -> 0), the strongest light's share of the
// luminance blends the wider lobe out (scaled per light by 1 - c16..c19.x, plCol[k].w); rgb = sum weight_k * colour_k. `glow` = the cube's alpha x c199.x (c199.x x c199.y = 1): the
// luminance of the first lobe's blend. Lobe constants c204-c209 / c254 / c255 measured identical on all 81 prelight draws of run 6816;
// c16-c19 = 0 there.
float PLLobe(float c, float thr, float width, float rootOff, float off, float scale, out float side) {
  float s = c - thr;
  float folded = saturate(abs(s) + thr);
  float m = -width * (1.0 - folded) + rootOff;  // (edge c204.z / c207.z = 1)
  side = sign(s);
  float t = (-sqrt(abs(m)) - off) * scale;
  return (1.0 - 0.5 * t * t) - 0.5;  // the quadratic Bezier of (1, 1, 0.5) at t, plus c254.x (-0.5)
}
float3 PreLight(float3 d, out float glow) {
  float4 w1, s1, w2, s2, lum;
  [unroll] for (int k = 0; k < 4; k++) {
    float c = dot(lDir[k].xyz, d);
    float a, b;
    w1[k] = PLLobe(c, 0.166666701, 4.0 * -0.333333403, 0.25, -0.5, -1.5, a);
    w2[k] = PLLobe(c, 0.409090906, 4.0 * -0.4909091, 0.00999999978, -0.100000001, -1.01851797, b);
    s1[k] = a; s2[k] = b;
    lum[k] = dot(plCol[k].rgb, float3(0.755379379, 0.236693099, 0.0079275202));  // c255.yzx against r, g, b
  }
  float4 k2 = w2 * s2 + 0.5, p = w1 * s1 + 0.5, q = w1 * s1 + (w2 * s2 + 1.0);
  float4 e2 = q * 0.5 - k2, e1 = q * 0.5 - p;
  float4 pl = p * lum;
  float total = pl.x + pl.y + pl.z + pl.w;
  float share = total > 0.0 ? max(max(pl.x, pl.y), max(pl.z, pl.w)) / total : 0.0;
  // Session 98 (match oracle): keep_k = 1 - share x (1 - c16..c19.x); plCol[k].w = c16..c19.x = the light set's falloff (asset_model.h
  // LightSet::fall: 0.5 on the match set's top light, 0 in entrances). Without it the match cube came out 4.5 % dark (capture match_a).
  float4 keep = 1.0 - share * (1.0 - float4(plCol[0].w, plCol[1].w, plCol[2].w, plCol[3].w));
  float4 shaded = e1 * keep + p, weight = e2 * keep + k2;
  glow = dot(shaded, lum) * plScale.z;  // (alpha x c199.y of the build, read x c199.x of this frame: the blue channel's ratio)
  return (weight.x * plCol[0].rgb + weight.y * plCol[1].rgb + weight.z * plCol[2].rgb + weight.w * plCol[3].rgb) * plScale.rgb;
}
VSOut vs(VSIn i) {
  VSOut o;
  float3 p = i.p, n = i.n, t = i.t;
  if (offset.w > 0.5) {  // skinned: the palette blend (weights sum to 1)
    float4x4 m = BoneM(i.bi.x) * i.bw.x + BoneM(i.bi.y) * i.bw.y + BoneM(i.bi.z) * i.bw.z + BoneM(i.bi.w) * i.bw.w;
    p = mul(float4(p, 1.0), m).xyz;
    n = mul(float4(n, 0.0), m).xyz;
    t = mul(float4(t, 0.0), m).xyz;
  }
  float4x4 inst = float4x4(i.r0, i.r1, i.r2, i.r3);
  p = mul(float4(p, 1.0), inst).xyz;
  n = mul(float4(n, 0.0), inst).xyz;
  t = mul(float4(t, 0.0), inst).xyz;
  p = float3(p.x * xform.y + p.z * xform.x, p.y, -p.x * xform.x + p.z * xform.y);  // Session 98: per-model turn about Y (identity: sin 0, cos 1)
  n = float3(n.x * xform.y + n.z * xform.x, n.y, -n.x * xform.x + n.z * xform.y);
  t = float3(t.x * xform.y + t.z * xform.x, t.y, -t.x * xform.x + t.z * xform.y);
  float3 wp = p + offset.xyz;
  o.pos = mul(float4(wp, 1.0), viewProj);
  // Session 98 (gen check 3): the game's crowd LOD (sub_8250E110): a STANDS member (instance tag r3.w = 2, asset_load.h) whose
  // centre (origin + 120 up) lies at clip w >= the card distance (ambient.z, MakeFrameCB) is drawn as a far CARD (T_Audience2D,
  // c184.z 2.125 instead of the bodies' 2.5).
  o.card = 0;
  if (flags2.w > 1.5 && i.r3.w > 1.5 && ambient.z > 0) {
    float3 org = i.r3.xyz;
    org = float3(org.x * xform.y + org.z * xform.x, org.y, -org.x * xform.x + org.z * xform.y) + offset.xyz + float3(0, 120, 0);
    o.card = mul(float4(org, 1.0), viewProj).w >= ambient.z ? 1.0 : 0.0;
  }
  o.n = n; o.uv0 = i.uv0; o.uv1 = i.uv1; o.c = i.c; o.wp = wp;
  o.tg = t;  // Session 98 (wrestler fix 3): the skinned vertex tangent (viewer space; the skin family's normal-map frame)
  float3 nn = dot(n, n) > 1e-6 ? normalize(n) : float3(0, 1, 0);
  // Crowd (S97 polish, own_crowd_vs.glsl): the game never normalises the world normal; its length is the member's scale (the
  // viewer's instance matrix is 10x that, the 1/10 rig), so the light sum and the hemisphere blend scale with it.
  if (flags2.w > 1.5 && flags2.y < 0.5) nn = n * 0.1;
  // Session 97 (light A/B, out/assets/s97/light): the game lights with the file's own normal and the light directions as stored
  // (file space, -Y up). The stored normals point INTO the surface (a floor's is +Y in the file), the directions are the way the
  // light travels, so dot(N, L) > 0 on a lit surface, and the ambient blend reads dot(-N, c180) = 0 on an up-facing floor (sky).
  // Our vertices are Y-negated: the file normal is (x, -y, z) of ours.
  float3 ng = float3(nn.x, -nn.y, nn.z);
  float3 lit = LightSum(ng) * matDif.rgb + Hemi(ng) * matAmb.rgb;
  o.lc = float4(lit, matAmb.w * matDif.w) * i.c;
  return o;
}
// Session 97 (run 6814: the game's own display-gamma cache at 0x83947010, read after boot, equals this curve with 0 of
// 1024 entries off): the game builds its front-buffer table as BT.709 encode of sRGB decode, and the screen (and our
// game captures) go through it. The viewer applies the same curve last; `{"cmd":"look","gamma":false}` turns it off.
float3 SrgbDecode(float3 x) { return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4); }
float3 Bt709Encode(float3 l) { return l < 0.018 ? 4.5 * l : 1.099 * pow(l, 0.45) - 0.099; }
// Session 98: the scene pass writes the game's pre-gamma colour; the display-gamma curve is the post chain's LAST pass (post.cpp).
float3 OutC(float3 c) { return saturate(c); }
//SHADOW_CHUNK
float4 ps(VSOut i) : SV_Target {
  // Session 98 (replay S5): {"cmd":"look","debug":14} = the blurred rim light map itself at every pixel (rg), for the replay harness
  if (look.z > 13.5 && look.z < 14.5) return float4(tRim.SampleLevel(sampClamp, i.pos.xy * rimS.xy, 0).rg, 0, 1);
  float3 n = i.n;
  n = dot(n, n) > 1e-6 ? normalize(n) : float3(0, 1, 0);
  float expo = look.y;
  if (look.x < 0.5) {  // the old plain look (A/B switch: {"cmd":"look","mode":"plain"})
    float lit = ambient.x + abs(dot(n, light0.xyz)) * light0.w + abs(dot(n, light1.xyz)) * light1.w;
    float4 col = (flags2.y > 0.5 ? float4(1, 1, 1, i.c.a) : i.c) * difColor;  // (a wrestler's vertex rgb is a lobe gain, not a colour)
    if (flags.x > 0.5) col *= tDiff.Sample(samp, i.uv0);
    if (flags.z > 0.5) clip(col.a - 0.5);
    float3 rgb = col.rgb * lit;
    // Session 96: a lightmapped surface takes its light from the lightmap (x2: plain-look guess).
    if (flags.y > 0.5) rgb = col.rgb * tLM.Sample(samp, i.uv1).rgb * 2.0;
    return float4(rgb, flags.w > 0.5 ? col.a : 1.0);
  }
  // Session 98 (wrestler fix 3): the skin / head / hair shaders read the colour texture with the LOD bias c201.w (-1 in every draw of run
  // 6816: one mip sharper than the derivatives pick); the gear / reflect shaders and the arena read it plain (wnrm.y = 0).
  float4 tex = flags.x > 0.5 ? tDiff.SampleBias(samp, i.uv0, wnrm.y) : float4(1, 1, 1, 1);
  float3 V = normalize(camPos.xyz - i.wp);
  if (flags2.y > 0.5) {
    // Wrestler (look spec section 5, "Wrestlers"): hemisphere ambient x MatAmb + capped light sum, x albedo x MatDif; a
    // highlight (spec mask = normal map w) and a simple rim. The runtime cube maps / shadow / SSS are approximated away.
    // Session 98 (spot / beard pass): the trans / hair family's alpha is the NORMAL map's w (x the vertex alpha), not the colour's:
    // src\scene\wrestler_trans*.frag / wrestler_hair.frag (proven equal to the game's translations) write normal.w x in_vertex.w; the
    // draws' alpha test is "pass if >= 1/255" [C: system constants of capture rp_ringin events 11731-11750]. With the colour's alpha
    // (BC1: 1 everywhere) Bryan's beard cards drew as solid plates.
    const float alphaSrc = wmisc.w > 0.5 ? tNrm.Sample(samp, i.uv0).w : tex.a;
    if (wmisc.w > 0.5) clip(alphaSrc * i.c.a - 1.0 / 255.0);
    else if (flags.z > 0.5) clip(tex.a * i.c.a - 0.5);
    if (dot(n, V) < 0) n = -n;
    if (outCurve.z < 0.5) {
      // Session 98 (skin fix; probe run 6816, out\s98\skin_fix\_model.py): the game's wrestler pixel shaders (src\scene\wrestler_skin*.frag,
      // wrestler_body*.frag, wrestler_head*.frag) with the inputs the viewer has. Space: the file's (-Y up); the stored normals point INTO
      // the surface (checked on ch161: every mesh) and the light directions are the way the light travels, so the game's normal is minus
      // the outward one: N = (-n.x, n.y, -n.z) of our (Y-flipped, outward) n. V = surface to eye.
      float3 N = float3(-n.x, n.y, -n.z);
      float3 Vf = float3(V.x, -V.y, V.z);
      // Session 98 (wrestler fix 3): the normal map, as the game's shaders perturb the normal (wrestler_skin_entrance.frag /
      // wrestler_body_entrance.frag section 2; probe run 6816: c218 = (1, 1, 1), the map scale c253.z = -2, K_ONE = 1): offsets
      // u = 1 - 2 map.y (bitangent side), v = 1 - 2 map.x (tangent side), N' = normalize(T v + B u + N). The skin family's frame is the
      // vertex tangent with B = N x T; the gear / reflect frame is horizontal from N and the up axis. The gear adds its tiled detail
      // normal (`noise2` x c206.x = 50, its x / y unpacked, x the normal gain mixed near -> far by the depth and the mask's z).
      float detailSigned = 0.0, detailDiff = 0.0, detailLight = 0.0;
      if (wnrm.z > 0.5) {
        const float4 nm = tNrm.Sample(samp, i.uv0);
        float u = 1.0 - 2.0 * nm.y, v = 1.0 - 2.0 * nm.x;
        float3 T, B;
        if (wnrm.x < 1.5) {
          T = float3(i.tg.x, -i.tg.y, i.tg.z);  // the file-space tangent (our vertices are Y-mirrored)
          T = dot(T, T) > 1e-12 ? normalize(T) : float3(1, 0, 0);
          B = cross(N, T);
        } else {
          const float ih = rsqrt(max(N.z * N.z + N.x * N.x, 1e-12));
          T = float3(N.z * ih, 0.0, -N.x * ih);
          B = float3(N.y * T.z, T.x * N.z - T.z * N.x, -N.y * T.x);
        }
        if (wdet.w > 0.5) {
          const float blend = saturate((length(camPos.xyz - i.wp) - wdet.y) / max(wdet.z - wdet.y, 1e-3));  // vertex c188: clip z near / far
          const float3 g = lerp(wdetA.xyz, wdetB.xyz, blend);
          const float4 d = tDet.Sample(samp, i.uv0 * wdet.x);
          const float dm = tDetM.Sample(samp, i.uv0).z;
          u += (2.0 * d.y - 1.0) * g.x * dm;
          v += (2.0 * d.x - 1.0) * g.x * dm;
          detailSigned = 2.0 * d.w - 1.0;
          detailDiff = detailSigned * g.y * dm;
          detailLight = detailSigned * g.z * dm;
        }
        const float3 m = T * v + B * u + N;
        N = dot(m, m) > 1e-12 ? normalize(m) : N;
      }
      float lit = flags.w < 0.5 ? ShadowTerm(i.wp, n, true, i.pos.xy, false) : 1.0;  // the game's `lit` (its own shadow map; ours here)
      float glowLum;
      float3 cube = min(PreLight(N, glowLum), limit.rgb);                     // min(cube x c197, c187)
      float3 skyGnd = lerp(sky.rgb, ground.rgb, 0.5 - 0.5 * N.y);             // up_mix = 0.5 dot(-N, c180) + 0.5
      float3 litAmb = cube * lit;
      if (wmisc.z > 0.5) {  // skin glow (yCh_skin family): c199.x x cube.a x lit, from c158 to c155
        float glow = glowLum * lit;
        float3 gc = lerp(sss2.rgb, sss1.rgb, glow) * sss1.w;
        float3 tint = lerp(sss2.rgb, 1.0, lit);
        litAmb = cube * lit * tint * (1.0 - gc) + gc * glow;
      }
      float3 light = skyGnd * matAmb.rgb + litAmb;                             // c189 = MatAmb; the knee (c156.x) is 0 in run 6816: off
      float3 base = tex.rgb * (1.0 + detailDiff) * matDif.rgb * light;        // c190 = MatDif; the vertex material colour is 1 (c237/c238 = 0); gear detail
      float mask = flags2.z > 1.5 ? 0.5 : (flags2.z > 0.5 ? tNrm.Sample(samp, i.uv0).w : 0.0);  // the normal map's w
      // Edge highlight from light 4 (c4 / c12), gated by the rim light map (Session 98 wrestler fix 2: the game's runtime pass,
      // shadow.cpp RimMapTerm; with the map off, the constant outCurve.w stands in, 0 = off).
      float nv = dot(N, Vf);
      float edge = 1.0 - abs(nv);
      float3 hs = N + Vf;
      float3 hdir = dot(hs, hs) > 1e-12 ? normalize(hs) : float3(0, 0, 0);
      float atten = 1.0 + saturate((length(camPos.xyz - i.wp) - 500.0) / 2000.0) * (0.4 - 1.0);  // c174 = g_fRimAtn* (500, 2500, 1, 0.4)
      float3 sp = max(dot(hdir, hlDir.xyz), 0.0) * pow(max(edge, 1e-6), wmisc.y) * hlCol.rgb * wmisc.x * atten;
      const float rimMap = rimP.w > 0.5 ? RimMapTerm(i.wp, i.pos.xy) : outCurve.w;
      float rimStr = ((mask - 1.0) * 0.83 + 1.0) * edge * rimMap;              // c161 = (1, 0.83, 1): map power, mask mix, strength
      float3 body = sp * (0.75 + 0.25 * base) * rimStr + base;                 // ((base+spec)*0.75 - base + base*(spec+1)*0.25) x rim + base
      // Session 98 (replay S4 taps, {"cmd":"look","debug":11..13}): 11 = (lit, rim map, glow luminance), 12 = the cube, 13 = base (no curve)
      if (look.z > 10.5) return float4(look.z < 11.5 ? float3(lit, rimMap, glowLum) : (look.z < 12.5 ? cube : base), 1);
      // The reflection: entrance = the four lights along the reflected view vector (two lobe widths, weights c0..c3.w). Plus the sky
      // rim c204 at grazing angles; soft-added.
      // Session 98 (sweep look): the MATCH variants read a 64x64 reflection cube (fetch 11) along the reflected vector (in view space,
      // rows c52..c54), x c197.w: channel x weighted c142.x x the gain, y weighted c142.z. That cube holds the SAME four-light lobes
      // as the entrance maths: x = sum_k w_k max(dot(d, l_k), 0)^c142.y / sum_k w_k, y = the same at c142.w, with c197.w = sum_k w_k
      // [C: capture rp_ring (run 6818, Orton cue 20, skin draw ev11048, PS B08B5E73), the cube sampled as the draw samples it over
      // 20000 directions: x = power 25 (= c142.y) |err| 0.0011, y = power 3 (= c142.w) |err| 0.0027, scale 1.009 / 1.007; with that
      // cube our scene-shader port gives the captured pixels to 0.97 / 255. Run 6824 match frame 42030: c197.w 2.736 = sum c0..c3.w].
      // So the match uses the lobes below too. Before: 0 in the match (the cube taken for an unknown runtime pass).
      // Residual [U]: the cube's 64x64 texels blur the narrow lobe a little (the port with exact lobes: 3.1 / 255 mean off the capture).
      float3 R = Vf - N * (2.0 * nv);
      float specLight = 0.0;
      {
        float4 dots = max(float4(dot(R, lDir[0].xyz), dot(R, lDir[1].xyz), dot(R, lDir[2].xyz), dot(R, lDir[3].xyz)), 1e-30);
        float4 wts = float4(lCol[0].w, lCol[1].w, lCol[2].w, lCol[3].w);
        float lobesA = dot(pow(dots, wspec.y), wts), lobesB = dot(pow(dots, wspec.w), wts);
        // Session 98 (wrestler fix 3): the skin family scales the first set by interpolator 9 = the vertex colour's grey (jboy.h)
        const float gainA = wnrm.w > 0.5 ? i.c.r : 1.0;
        specLight = mask * wspec.x * lobesA * gainA + mask * wspec.z * lobesB;
      }
      float skyUp = max(-R.y * pow(max(1.0 + nv, 1e-6), refc.w), 0.0);
      float3 env = (specLight + skyUp * refc.rgb * mask) * (1.0 + detailLight) * lit;  // c144 = 1; the gear's detail lightens / darkens it
      if (flags2.w > 0.5) env = skyUp * refc.rgb * mask * lit;               // hair / translucent: no lobes in this port
      float3 col = body * (1.0 - min(env, 1.0)) + env;
      float a = flags.w > 0.5 ? alphaSrc * i.c.a * matAmb.w * matDif.w : 1.0;
      return float4(OutC(col * expo), a);
    }
    float3 cube = min(LightSum(n), limit.rgb);
    // Session 98: the wrestlers' soft shadow map (shadow.cpp) darkens the direct light only; the hemisphere ambient stays
    if (flags.w < 0.5) cube *= ShadowTerm(i.wp, n, true, i.pos.xy, false);
    float3 light = Hemi(n) * matAmb.rgb + cube;
    float3 base = tex.rgb * matDif.rgb * light;
    float mask = flags2.z > 1.5 ? 0.5 : (flags2.z > 0.5 ? tNrm.Sample(samp, i.uv0).w : 0.0);
    float3 H = normalize(V + lDir[0].xyz);
    float3 sp = pow(saturate(dot(n, H)), max(misc.x, 1.0)) * spec.w * spec.rgb * lCol[0].rgb * mask * 0.5;
    float edge = 1.0 - saturate(dot(n, V));
    float3 rim = pow(edge, 3.0) * 0.30 * lCol[0].rgb * (flags2.w > 0.5 ? 0.5 : 1.0);
    float a = flags.w > 0.5 ? tex.a * i.c.a * matAmb.w * matDif.w : 1.0;
    return float4(OutC((base + sp + rim) * expo), a);
  }
  // Arena: per-vertex light (i.lc, already x vertex colour) x texture; lightmapped surfaces x the lightmap.
  if (flags.z > 0.5) clip(tex.a * i.c.a - 0.5);
  float3 rgb = tex.rgb * i.lc.rgb;
  // c184 (g_f4LightMapParam): xy = the top-down scale, z = the gain. Live (arena 5, probe): (1/5600, 1/5750, 2.5, 1). A mesh's own
  // lightmap (sampLightMapPrm) reads the second UV and takes the same gain.
  float lmGain = misc.w;
  if (i.card > 0.5) lmGain = ambient.w;  // (the card draw's c184.z, probe runs 6816 / 6819: 2.125 in arenas 5 and 1)
  if (flags.y > 0.5) rgb *= tLM.Sample(samp, i.uv1).rgb * lmGain;
  else if (flags2.x > 0.5) {
    // stage-wide top-down lightmap: uv = world.xz * c184.xy * 0.5 + 0.5 (look spec 1a)
    float2 uv = (i.wp.xz - offset.xz) * misc.yz * 0.5 + 0.5;  // (model space: the stage map ignores the viewer offset)
    rgb *= tStage.Sample(sampClamp, uv).rgb * lmGain;
  }
  if (flags.w < 0.5 && flags2.w < 1.5) rgb *= ShadowTerm(i.wp, -n, false, i.pos.xy, xform.w > 0.5);  // Session 98: floor / canvas / ring (outward normal = minus ours)
  if (spec.w > 0 && flags.z < 0.5) {  // arena highlight: strength in the diffuse alpha
    // outward normal = minus ours (see the vertex stage); to-the-light = the stored direction with its sign flipped, Y negated
    float3 toL = float3(-lDir[0].x, lDir[0].y, -lDir[0].z);
    float3 H = normalize(V + toL);
    rgb += tex.a * spec.w * spec.rgb * pow(saturate(dot(-n, H)), max(misc.x, 1.0)) * lCol[0].rgb;
  }
  if (look.w > 0.5) {  // depth haze of the match combine (look_live.md section 2): c27 towards the colour, by the depth the combine reads
    float depthC = 9.92 * i.pos.w;  // 1 - z/w of the game's projection = 9.92 / w_clip (SV_Position.w = 1 / w_clip)
    float k = 0.08 / 0.007 * max(0.007 - depthC, 0.0);
    rgb += (float3(0.4, 0.55, 0.55) - rgb) * k;
  }
  float a = flags.w > 0.5 ? tex.a * i.lc.a : 1.0;
  if (look.z > 0.5) {  // debug views ({"cmd":"look","debug":N}): 1 = vertex light only, 2 = texture only, 3 = stage lightmap used (green)
    if (look.z < 1.5) return float4(i.lc.rgb * expo, 1);
    if (look.z < 2.5) return float4(tex.rgb * expo, 1);
    if (look.z < 3.5) return float4(flags2.x > 0.5 ? float3(0, 1, 0) : (flags.y > 0.5 ? float3(0, 0, 1) : float3(0.3, 0.3, 0.3)), 1);
    if (look.z < 4.5) return float4(tLM.Sample(samp, i.uv1).rgb, 1);
    if (look.z < 5.5) return float4(tLM.Sample(samp, i.uv1).aaa, 1);
    return float4(tStage.Sample(sampClamp, i.wp.xz * misc.yz * 0.5 + 0.5).rgb * 2.0, 1);
  }
  return float4(OutC(rgb * expo), a);
}
)HLSL";

inline const char* const kFlash = R"HLSL(
cbuffer FlashCB : register(b0) { float4 vpc[4]; float4 centre; float4 ax; float4 ay; float4 az; float4 colour; };
Texture2D tex0 : register(t0);
Texture2D<float> depth : register(t1);
SamplerState smp : register(s0);
SamplerState pnt : register(s1);
struct VsIn { float3 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
struct VsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 col : COLOR0; };
float4 Clip(float3 p) { float4 q = float4(p, 1); return float4(dot(q, vpc[0]), dot(q, vpc[1]), dot(q, vpc[2]), dot(q, vpc[3])); }
VsOut vs_flash(VsIn v) {
  VsOut o;
  float3 w = centre.xyz + centre.w * (v.pos.x * ax.xyz + v.pos.y * ay.xyz + v.pos.z * az.xyz);
  o.pos = Clip(w);
  float4 c = Clip(centre.xyz);
  float2 uv = saturate(float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5));
  float d = depth.SampleLevel(pnt, uv, 0);
  bool hidden = c.w <= 0 || (ax.w < 0.5 && d < c.z / c.w);
  o.uv = v.uv;
  o.col = float4(colour.rgb * v.col.rgb, hidden ? 0 : colour.a * v.col.a);
  return o;
}
float4 ps_flash(VsOut i) : SV_Target { return tex0.Sample(smp, i.uv) * i.col; }
)HLSL";

inline const char* const kSprite = R"HLSL(
// Session 99 (particles 4): soft particles as the game's smoke pixel shader (RenderDoc kane_stage_frame5451: alpha x
// min(|scene depth - particle depth|, 30) / 30, view units). The pass has no depth attachment: the scene depth is read here
// (view w = nq / (q - depth)) and a sprite pixel behind the scene is discarded (the old depth test, no depth write).
cbuffer SpriteCB : register(b0) { float4 vpc[4]; float4 soft; };  // soft = (q, nq, 1 / range, 0)
Texture2D tex0 : register(t0);
Texture2D<float> depth : register(t1);
SamplerState smp : register(s0);
struct VsIn { float3 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
struct VsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 col : COLOR0; float w : TEXCOORD1; };
VsOut vs_sprite(VsIn v) {
  VsOut o;
  float4 q = float4(v.pos, 1);
  o.pos = float4(dot(q, vpc[0]), dot(q, vpc[1]), dot(q, vpc[2]), dot(q, vpc[3]));
  o.uv = v.uv;
  o.col = v.col;
  o.w = o.pos.w;
  return o;
}
float4 ps_sprite(VsOut i) : SV_Target {
  float4 c = tex0.Sample(smp, i.uv) * i.col;
  float d = depth.Load(int3(i.pos.xy, 0));
  float gap = soft.y / max(soft.x - d, 1e-6) - i.w;  // scene view w - the sprite's
  if (gap < 0) discard;
  c.a *= saturate(gap * soft.z);
  return c;
}
)HLSL";

inline const char* const kShadow = R"HLSL(
VKB(1, 0) cbuffer Shadow : register(b2) {
  float4x4 wrVP;
  float4x4 propsVP;
  float4 wrP;      // x = 1 / map size, y = depth bias (normalised), z = strength, w = enabled
  float4 wrL;      // xyz = direction TO the light, w = depth range in world units
  float4 propsP;   // x = enabled, y = strength, z = min height gap, w = fade range
  float4 planeR;   // centre x, centre z, half extent, enabled
  float4 planeL;   // xyz = point light, w = plane height
  float4 planeP;   // x = strength, y = band half-height
  float4 blobs[6]; // xyz = foot position, w = sigma
  float4 blobP;    // x = count, y = strength, z = band half-height
  float4 rimP;     // rim light map: x, y = view-depth range (c162.xy), z = gain (c162.z), w = enabled
  float4 rimS;     // xy = 1 / scene size
  float4 wrR;      // wrestler receivers: x = strength (c168.w), y = N.L fade (c169.y)
};
VKB(2, 0) Texture2D tProps : register(t4);
VKB(3, 0) Texture2D tWr : register(t5);
VKB(4, 0) Texture2D tPlane : register(t6);
VKB(5, 0) Texture2D tRim : register(t7);

// Session 98 (wrestler fix 2): the rim light map (the game's yRimLightMap, src\scene\vs_skin_rim.vert + rim_light_map.frag; probe
// run 6816): the wrestlers from the main camera, green = saturate((clip w - c162.x) / (c162.y - c162.x)), red 0 (the clear's red is
// 1: the game's mask reduction 901118D1 tests red against c255.y = 1). The skin's vertex stage makes the same depth (interpolator
// 5.z) for the comparison.
float RimFade(float3 wp) {
  const float w = dot(float4(wp, 1.0), float4(viewProj._14, viewProj._24, viewProj._34, viewProj._44));
  return saturate((w - rimP.x) / (rimP.y - rimP.x));
}
float4 ps_rim(VSOut i) : SV_Target {
  if (flags.x > 0.5 && flags.z > 0.5) clip(tDiff.Sample(samp, i.uv0).a * i.c.a - 0.5);
  return float4(0.0, RimFade(i.wp), 0.0, 1.0);
}
// The skin shaders' term (wrestler_skin.frag section 5): max(map.y - own depth, eps) x c162.z + map.x, raised to c161.x (1).
float RimMapTerm(float3 wp, float2 sp) {
  // Session 98 (replay S5): read one map texel up-left. The game's map sits one texel down-right of its wrestlers (its raster's
  // half-texel + the blur taps' half-texel, the console's pixel-centre convention): capture rp_ringin, the map's silhouette = Orton's
  // coverage shifted by (+7, +7) px at 2560x1440 (IoU 0.94; stage / ramp / ring x +7..8), the viewer's (-2, 0) (IoU 0.99). With one
  // texel the term at the parts' own pixels: head 0.067 / game 0.080 (was 0.046), shirt 0.027 / 0.024, eyes 0.0042 / 0.0045.
  const float2 lm = tRim.SampleLevel(sampClamp, sp * rimS.xy - float2(1.0 / 512.0, 1.0 / 256.0), 0).rg;
  return max(lm.y - RimFade(wp), 0.0) * rimP.z + lm.x;
}

// Caster entry points (the viewer's own vertex stage skins and places the mesh; the matrix is the light's, in the Frame cbuffer).
float4 ps_caster(VSOut i) : SV_Target {
  if (flags.x > 0.5 && flags.z > 0.5) clip(tDiff.Sample(samp, i.uv0).a * i.c.a - 0.5);
  return float4(i.wp.y, 0, 0, 1);  // the props map stores the height of the top-most piece
}
struct PlaneOut { float4 pos : SV_Position; float h : TEXCOORD0; float2 uv : TEXCOORD1; float a : TEXCOORD2; };
PlaneOut vs_plane(VSIn i) {
  float3 p = i.p;
  if (offset.w > 0.5) {
    float4x4 m = BoneM(i.bi.x) * i.bw.x + BoneM(i.bi.y) * i.bw.y + BoneM(i.bi.z) * i.bw.z + BoneM(i.bi.w) * i.bw.w;
    p = mul(float4(p, 1.0), m).xyz;
  }
  float4x4 inst = float4x4(i.r0, i.r1, i.r2, i.r3);
  p = mul(float4(p, 1.0), inst).xyz;
  p = float3(p.x * xform.y + p.z * xform.x, p.y, -p.x * xform.x + p.z * xform.y);  // Session 98 (scenes): the model's turn, as vs_main
  float3 wp = p + offset.xyz;
  float ph = xform.z;  // this wrestler's standing height (the renderer sets it per model)
  float3 L = float3(planeL.x, ph + planeL.y, planeL.z);
  float t = (L.y - ph) / max(L.y - wp.y, 1.0);  // from the point light through the vertex onto the canvas plane
  float3 q = L + (wp - L) * t;
  PlaneOut o;
  o.pos = float4((q.x - planeR.x) / planeR.z, -(q.z - planeR.y) / planeR.z, 0.5, 1.0);
  // Session 98 (match look): the game's mask value (yPlaneShadowMap, src\scene\vs_plane_shadow_skin.vert; constants of every mask draw
  // of probe runs 6816 / 6818, entrance and match alike: c0.x 500, c1-c3.x 300 / 40 / 1.5, c4-c6.x 0.5 / 0.35 / 0.21): per vertex,
  // d = the distance from the vertex to its projection on the plane (along the light ray), r = 1 - saturate(d / 500),
  // fade = 0.5 r^300 + 0.35 r^40 + 0.21 r^1.5 (interpolated; the 8-bit target saturates it). Full at the contact points, ~0.2 a few
  // units up, ~0.1-0.15 for the body. Was ours: 1 - 0.5 x height / 400 (x 0.6): a dark body-sized blob the game does not draw.
  const float r = 1.0 - saturate(length(q - wp) / 500.0);
  o.h = 0.5 * pow(r, 300.0) + 0.35 * pow(r, 40.0) + 0.21 * pow(r, 1.5);
  o.uv = i.uv0; o.a = i.c.a;
  return o;
}
float4 ps_plane(PlaneOut i) : SV_Target {
  if (flags.x > 0.5 && flags.z > 0.5) clip(tDiff.Sample(samp, i.uv).a * i.a - 0.5);  // cut-outs (hair cards)
  return float4(saturate(i.h), 0, 0, 1);
}

// Load outside a texture returns 0 (a false blocker at the map edge): clamp to the edge texel instead.
int3 LoadAt(float2 p, float n) { return int3(clamp(int2(p), int2(0, 0), int2(n - 1.0, n - 1.0)), 0); }
float2 SpiralTap(int k, int n, float rot) {
  float r = sqrt((k + 0.5) / n);
  float a = k * 2.39996323 + rot;
  return float2(cos(a), sin(a)) * r;
}
// The wrestlers' soft shadow: a blocker search sets the penumbra from the receiver-to-blocker distance (PCSS-like), then a 16-tap PCF.
float WrestlerShadow(float3 wp, float rot) {
  float4 lp = mul(float4(wp, 1.0), wrVP);
  float2 uv = float2(lp.x * 0.5 + 0.5, 0.5 - lp.y * 0.5);
  if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0 || lp.z > 1.0) return 0.0;
  float N = 1.0 / wrP.x;
  float zr = lp.z - wrP.y;
  float bsum = 0.0, bn = 0.0;
  [unroll] for (int k = 0; k < 8; k++) {
    float d = tWr.Load(LoadAt(uv * N + SpiralTap(k, 8, rot) * 8.0, N)).x;
    if (d < zr) { bsum += d; bn += 1.0; }
  }
  if (bn < 0.5) return 0.0;
  float pen = clamp((zr - bsum / bn) * wrL.w * 0.07, 1.5, 10.0);
  float lit = 0.0;
  [unroll] for (int j = 0; j < 16; j++) {
    float d = tWr.Load(LoadAt(uv * N + SpiralTap(j, 16, rot) * pen, N)).x;
    lit += d >= zr ? 1.0 : 0.0;
  }
  return 1.0 - lit / 16.0;
}
// The ring pieces' top-down map: the floor under a piece darkens, softer the further below it is.
float PropsShadow(float3 wp) {
  float4 lp = mul(float4(wp, 1.0), propsVP);
  float2 uv = float2(lp.x * 0.5 + 0.5, 0.5 - lp.y * 0.5);
  if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) return 0.0;
  float s = 0.0;
  [unroll] for (int k = 0; k < 9; k++) {
    float2 o = float2((k % 3) - 1, (k / 3) - 1) * 2.0;
    float h = tProps.Load(LoadAt(uv * 512.0 + o, 512.0)).x;
    float gap = h - wp.y;
    s += gap > propsP.z ? saturate(1.0 - (gap - propsP.z) / propsP.w) : 0.0;
  }
  return s / 9.0;
}
float Blobs(float3 wp) {
  float s = 0.0;
  for (int k = 0; k < (int)blobP.x; k++) {
    float2 d = wp.xz - blobs[k].xz;
    float r2 = dot(d, d) / (blobs[k].w * blobs[k].w);
    if (abs(wp.y - blobs[k].y) < blobP.z) s += exp2(-r2 * 1.44);  // the circle shadow: exp2(-r^2 k)
  }
  return min(s, 1.0);
}
// Standing heights: the wrestlers' true foot heights (the blob list); canvas / floor pieces at one of them take the plane shadow.
bool OnPlane(float3 wp) {
  for (int k = 0; k < (int)blobP.x; k++) if (abs(wp.y - blobs[k].y) < planeP.y) return true;
  return false;
}
float PlaneShadow(float3 wp) {  // (the ring mat only: no standing-height test, the mat is the plane)
  float2 uv = float2((wp.x - planeR.x) / planeR.z * 0.5 + 0.5, 0.5 - (wp.z - planeR.y) / planeR.z * 0.5);
  if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) return 0.0;
  return tPlane.SampleLevel(sampClamp, uv, 0).r;
}
// 1 = lit. nOut = the outward normal; isWr = a wrestler piece (receives only the wrestlers' light map); ringMat = the ring mat
// (yProp_PSM: only the plane shadow).
// Session 98 (shadow / match light): who receives what, as the game does it [C: captures rp_stage / rp_ramp / rp_ringin / rp_ring
// (run 6818), every draw's pixel textures by guest address: the wrestlers' map B (1FA00000) is read ONLY by wrestler materials (skin /
// head / gear / eyes, fetch 12); the blurred plane mask (1F592000) only by the ring mat (yProp_PSMLM 4E1A94BC, 1 draw a frame); the
// top-down props map (1F600000, the ring pieces + the circle blobs) only by the shadowed floor shaders (yBG_Default B63CD5FC,
// yBG_DefaultLM ED379393; in the match yBG_Default E98D0C36, passes.md)]. The 31-degree wrestler map on the canvas / floor / stage was
// ours: it drew the long stretched shadows the game does not have. Floor strengths: c168.w = 0.2 (the props term) and c169.x = 0.3
// (the blob term) in every probed floor draw of run 6816 (entrance 8941, match 12841 / 14041) and capture rp_ramp.
float ShadowTerm(float3 wp, float3 nOut, bool isWr, float2 sp, bool ringMat) {
  float t = 0.0;
  float rot = 6.2831853 * frac(52.9829189 * frac(dot(sp, float2(0.06711056, 0.00583715))));
  if (isWr) {
    if (wrP.w > 0.5) {  // the game's wrestler receivers (wrestler_skin.frag section 3): c168.w x shadow x (1 - (1 - N.L) x c169.y)
      const float nl = saturate(dot(nOut, wrL.xyz));
      t = max(t, WrestlerShadow(wp, rot) * wrR.x * (1.0 - (1.0 - nl) * wrR.y));
    }
    return 1.0 - t;
  }
  float upf = saturate((nOut.y - 0.3) / 0.4);
  if (upf <= 0.0) return 1.0;
  if (ringMat) {
    if (planeR.w > 0.5) t = max(t, PlaneShadow(wp) * planeP.x * upf);
  } else {
    if (propsP.x > 0.5) t = max(t, PropsShadow(wp) * propsP.y * upf);
    if (blobP.x > 0.5) t = max(t, Blobs(wp) * blobP.y * upf);
  }
  return 1.0 - t;
}
)HLSL";

inline const char* const kBlur = R"HLSL(
cbuffer Blur : register(b0) { float4 dir; };  // xy = step in uv
Texture2D tSrc : register(t0);
SamplerState sLin : register(s0);
struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VOut vsFull(uint id : SV_VertexID) {
  VOut o;
  float2 p = float2((id << 1) & 2, id & 2);
  o.uv = p;
  o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
float4 psDown(VOut i) : SV_Target {  // 4-tap
  float s = 0;
  [unroll] for (int k = 0; k < 4; k++) s += tSrc.SampleLevel(sLin, i.uv + float2((k & 1) ? 0.5 : -0.5, (k & 2) ? 0.5 : -0.5) * dir.xy, 0).r;
  return float4(s * 0.25, 0, 0, 1);
}
float4 psBlur(VOut i) : SV_Target {  // 21 taps (the game's blur width), gaussian
  float s = 0, ws = 0;
  [unroll] for (int k = -10; k <= 10; k++) {
    float w = exp(-k * k / 32.0);
    s += tSrc.SampleLevel(sLin, i.uv + dir.xy * k * 0.7, 0).r * w;
    ws += w;
  }
  return float4(s / ws, 0, 0, 1);
}
// Session 98 (wrestler fix 2): the rim map's blur, the game's side_blur13 (632189BA) with the offsets / weights of probe run 6816
// (draw 887 of swap 8341, pixel c0-c12 / c16-c28): a 13-tap diamond, spacing 2 texels, each tap bilinear between two texels
// (the game's taps sit on texel corners: a 2x2 average). Weights (rgb) 0.18386 centre, 0.11152 / 0.06764 / 0.02488 outward.
float4 psRim13(VOut i) : SV_Target {
  const float3 T[13] = {float3(0, 0, 0.18386),
      float3(-2, 0, 0.11152), float3(2, 0, 0.11152), float3(0, -2, 0.11152), float3(0, 2, 0.11152),
      float3(-2, -2, 0.06764), float3(-2, 2, 0.06764), float3(2, -2, 0.06764), float3(2, 2, 0.06764),
      float3(-4, 0, 0.02488), float3(4, 0, 0.02488), float3(0, -4, 0.02488), float3(0, 4, 0.02488)};
  float4 s = 0;
  [unroll] for (int k = 0; k < 13; k++) s += tSrc.SampleLevel(sLin, i.uv + (T[k].xy + 0.5) * dir.xy, 0) * T[k].z;
  return s;
}
)HLSL";

inline const char* const kPost = R"HLSL(
#pragma pack_matrix(row_major)
cbuffer Post : register(b0) {
  float4 c20;      // DoF slopes: x near, y far, w far cap  (match 91.33, 338.67, _, 0.8)
  float4 c21;      // DoF thresholds in z = 1 - depth: x near, y far, z offset (match 0.991855, 0.99509, 0)
  float4 hazeCol;  // c27
  float4 hazeP;    // x = c29.x strength (0.08), y = c30.x start (0.993), z = c255.x floor (0)
  float4 size;     // x = 1/w, y = 1/h, z = w, w = h   (the full-size scene)
  float4 proj;     // x = q, y = nq, z = the game's depth scale 9.92, w = far-blur gate depth
  float4 flags;    // x = fxaa, y = dof, z = haze, w = gamma
  float4 bloomP;   // x = bloom strength, y = glare strength, z = bright threshold, w = 1 when the entrance recipe runs
  float4 passP;     // x,y = blur step in source texels, z = 1: depth-gated, w = source texel width (1/srcW) for the bright pass
  float4 film;      // Session 98 (arena fix 2): the entrance film filter (VS 032E8FAC quad, alpha-blended over the picture): rgb, a = strength
};
Texture2D tScene : register(t0);
Texture2D tDepth : register(t1);
Texture2D tBlurA : register(t2);
Texture2D tBlurB : register(t3);
Texture2D tBloom : register(t4);
Texture2D tGlare : register(t5);
SamplerState sLin : register(s0);
struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VOut vsFull(uint id : SV_VertexID) {
  VOut o;
  float2 p = float2((id << 1) & 2, id & 2);
  o.uv = p;
  o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
// The game's depth value (small = far, ~0.007 near the camera): 9.92 / view w, from the depth buffer.
float GameDepth(float z) {
  float w = proj.y / max(proj.x - z, 1e-6);
  return proj.z / max(w, 1e-3);
}
float DepthAt(int2 p) {
  int2 c = clamp(p, int2(0, 0), int2(size.zw) - 1);
  return GameDepth(tDepth.Load(int3(c, 0)).x);
}
// ---- the quarter-size downsample: 4 bilinear taps (each 2x2) over the 4x4 block; far = only taps past the DoF start ----
float4 psDownNear(VOut i) : SV_Target {
  float2 px = size.xy;
  float3 s = 0;
  [unroll] for (int k = 0; k < 4; k++) {
    float2 o = float2((k & 1) ? 1.0 : -1.0, (k & 2) ? 1.0 : -1.0);
    s += tScene.SampleLevel(sLin, i.uv + o * px, 0).rgb;
  }
  return float4(s * 0.25, 1);
}
float4 psDownFar(VOut i) : SV_Target {
  float2 px = size.xy;
  float3 sum = 0, all = 0;
  float n = 0;
  [unroll] for (int k = 0; k < 4; k++) {
    float2 o = float2((k & 1) ? 1.0 : -1.0, (k & 2) ? 1.0 : -1.0);
    float2 uv = i.uv + o * px;
    float3 c = tScene.SampleLevel(sLin, uv, 0).rgb;
    all += c;
    if (DepthAt(int2(uv * size.zw)) < proj.w) { sum += c; n += 1; }  // past the far start only
  }
  return n > 0 ? float4(sum / n, n * 0.25) : float4(all * 0.25, 0);
}
// ---- 13-tap blur at quarter size (gated: weighted by the marked share, so the foreground never bleeds into the far blur) ----
float4 psBlur13(VOut i) : SV_Target {
  float2 px = passP.xy;
  float3 sum = 0; float wsum = 0, asum = 0, tot = 0;
  [unroll] for (int k = 0; k < 13; k++) {
    float2 o = float2(0, 0);
    if (k >= 1 && k <= 4) o = float2(k == 1 ? 1 : (k == 2 ? -1 : 0), k == 3 ? 1 : (k == 4 ? -1 : 0));
    else if (k >= 5 && k <= 8) o = float2((k & 1) ? 1 : -1, (k & 2) ? 1 : -1);
    else if (k >= 9) o = float2(k == 9 ? 2 : (k == 10 ? -2 : 0), k == 11 ? 2 : (k == 12 ? -2 : 0));
    float w = exp(-dot(o, o) * 0.255);
    float4 c = tScene.SampleLevel(sLin, i.uv + o * px * 1.3, 0);
    float cw = passP.z > 0.5 ? w * c.a : w;
    sum += c.rgb * cw; wsum += cw; asum += w * c.a; tot += w;
  }
  float4 centre = tScene.SampleLevel(sLin, i.uv, 0);
  float3 rgb = wsum > 1e-4 ? sum / wsum : centre.rgb;
  return float4(rgb, asum / tot);
}
// ---- bloom: bright pass over the 8x8 block (16 taps), threshold subtracted; 15-tap separable blur ----
float4 psBright(VOut i) : SV_Target {
  float2 px = size.xy;
  float3 s = 0;
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 4; x++) {
      float2 o = (float2(x, y) - 1.5) * 2.0 * px;
      s += max(tScene.SampleLevel(sLin, i.uv + o, 0).rgb - bloomP.z, 0);
    }
  return float4(s / 16.0, 1);
}
float4 psBlur15(VOut i) : SV_Target {
  float3 s = 0; float ws = 0;
  [unroll] for (int k = -7; k <= 7; k++) {
    float w = exp(-k * k / 18.0);
    s += tScene.SampleLevel(sLin, i.uv + passP.xy * k, 0).rgb * w; ws += w;
  }
  return float4(s / ws, 1);
}
// ---- star glare: 4 streak directions, 8 decaying taps each way, from the bright pass ----
float4 psGlare(VOut i) : SV_Target {
  float2 px = size.xy;  // the eighth-size target's texel
  float3 s = 0; float ws = 0;
  [unroll] for (int d = 0; d < 4; d++) {
    float a = 0.7853982 * d;
    float2 dir = float2(cos(a), sin(a)) * px * 1.5;
    float w = 1.0;
    for (int k = 1; k <= 8; k++) {
      w *= 0.8;
      s += (tScene.SampleLevel(sLin, i.uv + dir * k, 0).rgb + tScene.SampleLevel(sLin, i.uv - dir * k, 0).rgb) * w;
      ws += 2 * w;
    }
  }
  return float4(s / ws * 2.0, 1);
}
// ---- the combine: console-style FXAA, near / far DoF, depth haze, bloom + glare (entrances), display gamma last ----
float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float3 Tap(float2 p) { return tScene.SampleLevel(sLin, p, 0).rgb; }
float3 Fxaa(float2 uv) {
  float2 px = size.xy;
  float2 nw = uv - 0.5 * px;
  float lNW = Luma(Tap(nw)), lNE = Luma(Tap(nw + float2(px.x, 0))), lSW = Luma(Tap(nw + float2(0, px.y))), lSE = Luma(Tap(nw + px));
  float3 mid = Tap(uv);
  float lM = Luma(mid);
  float reduce = max((lNW + lNE + lSW + lSE) * 0.03125, 1.0 / 128.0);
  float dNS = (lNW + lNE) - (lSW + lSE);
  float dWE = (lNW + lSW) - (lNE + lSE);
  float rcpMin = 1.0 / (min(abs(dNS), abs(dWE)) + reduce);
  float2 dir = clamp(float2(-dNS, dWE) * rcpMin, -8.0, 8.0) * px;
  float3 inner = Tap(uv + dir * (1.0 / 3.0 - 0.5)) + Tap(uv + dir * (2.0 / 3.0 - 0.5));
  float3 outer = Tap(uv - dir * 0.5) + Tap(uv + dir * 0.5);
  float3 two = inner * 0.5;
  float3 four = inner * 0.25 + outer * 0.25;
  float lFour = Luma(four);
  float lMin = min(min(min(lSW, lSE), min(lNW, lNE)), lM);
  float lMax = max(max(max(lSW, lSE), max(lNW, lNE)), lM);
  return (lFour < lMin || lFour > lMax) ? two : four;
}
float3 SrgbDecode(float3 x) { return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4); }
float3 Bt709Encode(float3 l) { return l < 0.018 ? 4.5 * l : 1.099 * pow(l, 0.45) - 0.099; }
float4 psCombine(VOut i) : SV_Target {
  float2 uv = i.uv;
  float3 color = flags.x > 0.5 ? Fxaa(uv) : Tap(uv);
  float depth = DepthAt(int2(uv * size.zw));
  float z = 1.0 - depth;
  if (flags.y > 0.5) {
    if (c21.x > z) {  // right in front of the camera: toward the near blur (flags.y > 1.5 only; the viewer's free cameras get too near)
      if (flags.y > 1.5) {
        float f = clamp((c21.x - z) * c20.x + c21.z, c21.z, 1.0);
        color += (tBlurA.SampleLevel(sLin, uv, 0).rgb - color) * f;
      }
    } else {          // the background: toward the far blur, capped (c20.w)
      float f = clamp((z - c21.y) * c20.y + c21.z, c21.z, c20.w);
      color += (tBlurB.SampleLevel(sLin, uv, 0).rgb - color) * f;
    }
  }
  if (flags.z > 0.5) {  // the depth tint toward c27 (up to 8% on the far crowd)
    float span = 1.0 - hazeP.y;
    float k = hazeP.x / span * max(span - depth, hazeP.z);
    color += (hazeCol.rgb - color) * k;
  }
  if (bloomP.w > 0.5) {
    if (bloomP.x != 0.0) color += tBloom.SampleLevel(sLin, uv, 0).rgb * bloomP.x;
    if (bloomP.y != 0.0) color += tGlare.SampleLevel(sLin, uv, 0).rgb * bloomP.y;
  }
  color = saturate(color);
  // The film filter: lerp toward the colour by a, before the display curve [C: Orton's game frames peak at (249, 244, 236) = white
  // lerped 0.078 toward (0.729, 0.557, 0.212) through the curve; darkest 1% (5-9, 4-6, 1-3) = the colour x a].
  color = lerp(color, film.rgb, film.a);
  if (flags.w > 0.5) color = Bt709Encode(SrgbDecode(color));  // the game's display-gamma curve, last (run 6814)
  return float4(color, 1);
}
)HLSL";

inline const char* const kPick = R"HLSL(
VKB(1, 1) cbuffer PickCB : register(b3) {
  uint4 pickIds;      // x = (model << 20 | mesh) + 1
  float4 pickMode;    // x = 1: alpha blended material (clip like a cut-out)
};
struct VSIdOut { float4 pos : SV_Position; float2 uv0 : TEXCOORD0; float a : TEXCOORD1; nointerpolation uint inst : TEXCOORD2; };
// Session 98 (step 16): the scene's vertex stage (skin, placed copy, model turn, viewProj), reduced to the position, for the ID and mask passes.
VSIdOut IdTransform(VSIn i, uint iid, float zbias) {
  float3 p = i.p;
  if (offset.w > 0.5) {
    float4x4 m = BoneM(i.bi.x) * i.bw.x + BoneM(i.bi.y) * i.bw.y + BoneM(i.bi.z) * i.bw.z + BoneM(i.bi.w) * i.bw.w;
    p = mul(float4(p, 1.0), m).xyz;
  }
  float4x4 inst = float4x4(i.r0, i.r1, i.r2, i.r3);
  p = mul(float4(p, 1.0), inst).xyz;
  p = float3(p.x * xform.y + p.z * xform.x, p.y, -p.x * xform.x + p.z * xform.y);
  float3 wp = p + offset.xyz;
  VSIdOut o;
  o.pos = mul(float4(wp, 1.0), viewProj);
  o.pos.z -= o.pos.w * zbias;
  o.uv0 = i.uv0; o.a = i.c.a; o.inst = iid;
  return o;
}
VSIdOut vs_id(VSIn i, uint iid : SV_InstanceID) { return IdTransform(i, iid, 0.0); }
VSIdOut vs_mask(VSIn i, uint iid : SV_InstanceID) { return IdTransform(i, iid, 0.0); }  // (the mask's depth bias is the rasterizer's)
void IdClip(VSIdOut i) {
  if (flags.z > 0.5 || pickMode.x > 0.5) clip((flags.x > 0.5 ? tDiff.Sample(samp, i.uv0).a : 1.0) * i.a - 0.5);
}
uint2 ps_id(VSIdOut i) : SV_Target { IdClip(i); return uint2(pickIds.x, i.inst + 1); }
float4 ps_mask(VSIdOut i) : SV_Target { IdClip(i); return float4(1, 1, 1, 1); }
)HLSL";

inline const char* const kComp = R"HLSL(
Texture2D tMask : register(t0);
cbuffer CompCB : register(b0) {
  float4 texel;   // xy = 1 / size
  float4 selc;    // rgb colour, a = strength (0 = nothing selected)
  float4 hovc;
  float4 misc;    // x = glow radius (px), y = outline radius (px)
};
struct VOut { float4 pos : SV_Position; };
VOut vs_fs(uint id : SV_VertexID) {
  VOut o;
  float2 p = float2((id << 1) & 2, id & 2);
  o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
float2 M(int2 p, int2 sz) { p = clamp(p, int2(0, 0), sz - 1); return tMask.Load(int3(p, 0)).rg; }
float4 ps_comp(VOut i) : SV_Target {
  int2 p = int2(i.pos.xy);
  int2 sz = int2(1.0 / texel.xy + 0.5);
  float2 c = M(p, sz);
  float2 mx = c, mn = c;
  [unroll] for (int k = 0; k < 8; k++) {
    float a = k * 0.785398;
    int2 d = int2(round(cos(a) * misc.y), round(sin(a) * misc.y));
    float2 m = M(p + d, sz);
    mx = max(mx, m); mn = min(mn, m);
  }
  float2 edge = saturate(mx - mn);          // a band about 3 px wide straddling the mask's border
  float2 g = 0; float ws = 0;
  [unroll] for (int j = 0; j < 24; j++) {   // a blurred copy of the mask: 24 taps on a spiral, falling off with distance
    float t = (j + 0.5) / 24.0;
    float rad = misc.x * sqrt(t);
    float ang = j * 2.39996;
    int2 d = int2(round(cos(ang) * rad), round(sin(ang) * rad));
    float w = 1.0 - 0.6 * t;
    g += M(p + d, sz) * w; ws += w;
  }
  g = g / ws * (1.0 - 0.45 * c);            // a little less glow on the part itself
  float so = edge.x * selc.a;
  float ho = edge.y * hovc.a * 0.55;
  float3 rgb = selc.rgb * (so + g.x * 0.95 * selc.a * (1.0 - so));
  rgb += hovc.rgb * (ho + g.y * 0.7 * hovc.a * (1.0 - ho));
  return float4(rgb, max(so, ho));          // premultiplied: the outline is solid, the glow only adds
}
)HLSL";

// The scene shader as compiled: kScene with the shadow receivers / casters and the pick entry points spliced in at //SHADOW_CHUNK.
inline std::string SceneSource() {
  std::string s = kScene;
  const size_t at = s.find("//SHADOW_CHUNK");
  if (at == std::string::npos) return std::string();
  s.replace(at, 14, std::string(kShadow) + kPick);
  return s;
}

}  // namespace hlsl
}  // namespace view
