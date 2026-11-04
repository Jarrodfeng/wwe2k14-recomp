// Session 87 (stage 4): the body of the wrestlers' gear vertex shaders (the game's yCh_gear; vs_ch_gear_w12.vert, vs_ch_gear_w9.vert).
// Hand-written from our reading of the guest shaders (see vs_ch.glsl for the skinning). The variants only differ in the vertex layout, set
// by the includer: CH_STRIDE (dwords a vertex), CH_WEIGHTS (blend weights at dword 4), CH_BYTES / CH_NORMAL / CH_TANGENT / CH_UV (the
// dwords of the bone bytes, the normal, the tangent and the texture coordinate), and CH_SKIN_STYLE (vs_ch.glsl). Include after
// xenos_vs.glsl, xenos_vs_math.glsl, xenos_vs_skin.glsl and vs_ch.glsl.
// The literals are in c255 (0, 1, 1/2, the bone stride). The pixel side reads the reflection pieces of yCh_Reflect (unit normal and view,
// a specular term shaped by a Fresnel-style rim), the screen position, the depth bands and a fade by the clip z (c188), plus the unit tangent.
// Interpolators: 0 = the unit normal; 1 = (u, v, the depth band, 0); 2 = (the unit view vector, 1); 3 = the clip-z fade four times;
// 4 = the world position through the rows c68 / c69 / c166 / c71; 5 = (flag, flag) with the flag = (-|1 / tangent length| > 0);
// 6 = (screen x, screen y, the depth fade, the fog factor); 7 = the unit tangent; 8 = (1, 1, 1, the alpha scale c185.x x c189.w x c190.w);
// 9 = the specular colour.
// Float constants: the whole block by number (kDynamic).

layout(location = 0) invariant out vec4 out_normal;
layout(location = 1) invariant out vec4 out_uv;
layout(location = 2) invariant out vec4 out_view;
layout(location = 3) invariant out vec4 out_clip_fade;
layout(location = 4) invariant out vec4 out_projected;
layout(location = 5) invariant out vec4 out_flag;
layout(location = 6) invariant out vec4 out_screen;
layout(location = 7) invariant out vec4 out_tangent;
layout(location = 8) invariant out vec4 out_alpha;
layout(location = 9) invariant out vec4 out_specular;

void main() {
  uint index = XeGuestVertexIndex();
  int at = XeVertexAddress(index, CH_STRIDE, 95);
  vec4 lit = fc.c[255];
  vec4 position = AvFetchPosition(at);
#if CH_WEIGHTS
  vec3 weights = vec3(XeFetchFloat(at + 4), XeFetchFloat(at + 5), XeFetchFloat(at + 6));
#else
  vec3 weights = vec3(0.0);  // (this declaration has none)
#endif
  uint packed_bytes = XeFetchDword(at + CH_BYTES);
  vec4 bytes = vec4(bitfieldExtract(packed_bytes, 0, 8), bitfieldExtract(packed_bytes, 8, 8), bitfieldExtract(packed_bytes, 16, 8),
                    bitfieldExtract(packed_bytes, 24, 8));
  vec3 raw_normal = ChFetchNormal(at + CH_NORMAL);
  vec3 raw_tangent = ChFetchNormal(at + CH_TANGENT);
  vec2 halves = unpackHalf2x16(XeFetchDword(at + CH_UV));
  precise float scale = fc.c[218].w + lit.y;
  vec3 normal = AvMulX3(vec3(scale), raw_normal);
  vec3 tangent_in = AvMulX3(vec3(scale), raw_tangent);
  precise float alpha_scale = AvMulX(fc.c[185].x, AvMulX(fc.c[189].w, fc.c[190].w));

  ChBlend skin = ChSkin(position, normal, bytes, weights, lit.w, lit.y, lit.x);
  vec3 world = skin.position;
  vec3 turned = skin.normal;
  vec3 tangent = ChSkinTangent(tangent_in, bytes, weights, lit.w, lit.y);

  // the clip position
  vec4 p = vec4((lit.x == 0.0) ? world.z : lit.y, (lit.x == 0.0) ? world.x : lit.y, (lit.x == 0.0) ? world.y : lit.y,
                (lit.y == 0.0) ? world.y : lit.y);
  vec4 clip = vec4(AvDot4R(fc.c[56].zxyw, p), AvDot4R(fc.c[57].zxyw, p), AvDot4R(fc.c[58].zxyw, p), AvDot4R(fc.c[59].zxyw, p));
  XeOutputPosition(clip);

  // the unit normal and the unit view vector
  float inv_n = inversesqrt(abs(AvDot3(turned.zxy, turned.zxy)));
  vec3 n_unit = AvMulX3(vec3(inv_n), turned);
  precise vec3 view = vec3(fc.c[72].x - world.x, fc.c[72].y - world.y, fc.c[72].z - world.z);
  float inv_v = inversesqrt(abs(AvDot3(view.zxy, view.zxy)));
  vec3 v_unit = AvMulX3(view, vec3(inv_v));

  // the specular term: the half vector against c12, times a Fresnel-style rim
  precise vec3 half_sum = v_unit + n_unit;
  vec3 half_vec = AvMulX3(half_sum, vec3(lit.z));
  float inv_h = inversesqrt(abs(AvDot3(half_vec.zxy, half_vec.zxy)));
  vec3 h_unit = AvMulX3(half_vec, vec3(inv_h));
  float h_dot = AvDot3R(h_unit.zxy, fc.c[12].zxy);
  float h_term = (h_dot >= lit.x) ? h_dot : lit.x;
  float v_dot_n = AvDot3R(v_unit.zxy, n_unit.zxy);
  precise float rim_base = (-abs(v_dot_n)) + lit.y;
  precise float rim_log = AvMulX(log2(abs(rim_base)), fc.c[205].y);
  float rim = exp2(rim_log);
  precise float spec_rim = AvMulX(h_term, rim);
  precise float spec_w = AvMulX(spec_rim, clip.w);

  // the screen position and the depth bands
  precise float inv_w = 1.0 / clip.w;
  float ndc_x = AvMulX(clip.x, inv_w), ndc_y = AvMulX(clip.y, inv_w);
  float w_floor = (clip.w >= fc.c[196].x) ? clip.w : fc.c[196].x;
  float w_cap = (w_floor < fc.c[196].y) ? w_floor : fc.c[196].y;
  precise float band = AvMulX(w_cap + (-fc.c[196].x), fc.c[196].z);
  precise float fade_range = fc.c[162].y - fc.c[162].x;
  precise float fade_from = (-fc.c[162].x) - (-clip.w);
  precise float inv_fade_range = 1.0 / fade_range;
  float fade = AvSat(AvMulX(inv_fade_range, fade_from));
  precise float fog_range = fc.c[174].y + (-fc.c[174].x);
  precise float fog_span = fc.c[174].w + (-fc.c[174].z);
  precise float fog_from = (-fc.c[174].x) - (-clip.w);
  precise float inv_fog_range = 1.0 / fog_range;
  precise float fog = AvMulX(AvSat(AvMulX(fog_from, inv_fog_range)), fog_span) + fc.c[174].z;
  precise float z_range = fc.c[188].y - fc.c[188].x;
  precise float z_from = (-fc.c[188].x) - (-clip.z);
  precise float inv_z_range = 1.0 / z_range;
  float z_fade = AvSat(AvMulX(z_from, inv_z_range));

  float inv_t = inversesqrt(abs(AvDot3(tangent.zxy, tangent.zxy)));
  float flag = ((-abs(inv_t)) > lit.x) ? 1.0 : 0.0;

  out_normal = vec4(n_unit, 0.0);
  out_uv = vec4(halves.y, halves.x, band, 0.0);
  out_view = vec4(v_unit, 1.0);
  out_clip_fade = vec4(z_fade);
  out_projected = vec4(AvDot4R(fc.c[68].zxyw, p), AvDot4R(fc.c[69].zxyw, p), AvDot4R(fc.c[166].zxyw, p), AvDot4R(fc.c[71].zxyw, p));
  out_flag = vec4(flag, flag, 0.0, 0.0);
  precise float screen_x = AvMulX(lit.z, ndc_x) + lit.z;
  precise float screen_y = AvMulX(-ndc_y, lit.z) + lit.z;
  out_screen = vec4(screen_x, screen_y, fade, fog);
  out_tangent = vec4(AvMulX3(vec3(inv_t), tangent), 0.0);
  out_alpha = vec4(1.0, 1.0, 1.0, alpha_scale);
  precise vec3 specular = AvMulX3(AvMulX3(vec3(spec_w), fc.c[4].xyz), vec3(fc.c[205].x));
  out_specular = vec4(specular, 0.0);
}
