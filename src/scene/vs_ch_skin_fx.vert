#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the game's yCh_skin in its two-lookup form (guest vertex shader
// 0E92FB108A675C39; 0 / 8 / 8 draws a frame in Brock's entrance / Bryan's entrance / the match). Pixel side: unknown. Hand-written from
// our reading of the guest shader (see vs_ch.glsl for the skinning); checked bit for bit against the SDK's translation by
// tools/shaders/harness/vs_harness.py.
//
// As vs_ch_skin_a (the same 12-dword vertex, the same two point lights, reflection pieces, screen position, depth bands and unit
// tangent), but with its literals in c253 / c243 and the colour's last byte decoded into two indices: one picks a number for interpolator
// 1's w (from c231 / c232), the other the material colour (from c237 / c238). Interpolators: 0 = the unit normal; 1 = (u, v, the depth
// band, the picked number); 2 = (the unit view vector, 1); 3 = the world position through the rows c68 / c69 / c166 / c71; 4 = (the two
// lights' strength, 0); 5 = (screen x, screen y, the depth fade, the fog factor); 6 = the unit tangent; 7 = (the material colour, the
// alpha scale c185.x x c189.w x c190.w); 8 = the specular colour; 9 = the colour's third byte three times.
// Float constants: the whole block by number (kDynamic).

#include "xenos_vs_math.glsl"
#include "xenos_vs_skin.glsl"
#define CH_SKIN_STYLE 1
#include "vs_ch.glsl"

layout(location = 0) invariant out vec4 out_normal;
layout(location = 1) invariant out vec4 out_uv;
layout(location = 2) invariant out vec4 out_view;
layout(location = 3) invariant out vec4 out_projected;
layout(location = 4) invariant out vec4 out_lights;
layout(location = 5) invariant out vec4 out_screen;
layout(location = 6) invariant out vec4 out_tangent;
layout(location = 7) invariant out vec4 out_material;
layout(location = 8) invariant out vec4 out_specular;
layout(location = 9) invariant out vec4 out_shade;

void main() {
  uint index = XeGuestVertexIndex();
  int at = XeVertexAddress(index, 12, 95);
  vec4 lit = fc.c[253];     // 0, 1, 255, a nibble scale
  float half_one = fc.c[243].y;
  vec4 position = AvFetchPosition(at);
  vec3 weights = vec3(XeFetchFloat(at + 4), XeFetchFloat(at + 5), XeFetchFloat(at + 6));
  uint packed_bytes = XeFetchDword(at + 7);
  vec4 bytes = vec4(bitfieldExtract(packed_bytes, 0, 8), bitfieldExtract(packed_bytes, 8, 8), bitfieldExtract(packed_bytes, 16, 8),
                    bitfieldExtract(packed_bytes, 24, 8));
  vec3 raw_normal = ChFetchNormal(at + 8);
  vec3 raw_tangent = ChFetchNormal(at + 9);
  vec4 vertex_color = AvFetchColorAt(at + 10);
  float color_last = vertex_color.w, color_third = vertex_color.x;
  vec2 halves = unpackHalf2x16(XeFetchDword(at + 11));
  precise float scale = fc.c[218].w + lit.y;
  vec3 normal = AvMulX3(vec3(scale), raw_normal);
  vec3 tangent_in = AvMulX3(vec3(scale), raw_tangent);
  precise float alpha_scale = AvMulX(fc.c[185].x, AvMulX(fc.c[189].w, fc.c[190].w));

  float stride = fc.c[243].x;
  ChBlend skin = ChSkin(position, normal, bytes, weights, stride, lit.y, lit.x);
  vec3 world = skin.position;
  vec3 turned = skin.normal;
  vec3 tangent = ChSkinTangent(tangent_in, bytes, weights, stride, lit.y);

  // the clip position
  vec4 p = vec4((lit.x == 0.0) ? world.z : lit.y, (lit.x == 0.0) ? world.x : lit.y, (lit.x == 0.0) ? world.y : lit.y,
                (lit.y == 0.0) ? world.y : lit.y);
  vec4 clip = vec4(AvDot4R(fc.c[56].zxyw, p), AvDot4R(fc.c[57].zxyw, p), AvDot4R(fc.c[58].zxyw, p), AvDot4R(fc.c[59].zxyw, p));
  XeOutputPosition(clip);

  // the unit normal and the unit view vector
  float inv_n = inversesqrt(abs(AvDot3(turned.zxy, turned.zxy)));
  vec3 n_unit = AvMulX3(vec3(inv_n), turned);
  precise vec3 view = vec3((-world.x) + fc.c[72].x, (-world.y) + fc.c[72].y, (-world.z) + fc.c[72].z);
  float inv_v = inversesqrt(abs(AvDot3(view.zxy, view.zxy)));
  vec3 v_unit = AvMulX3(view, vec3(inv_v));

  // the specular term (as the reflective wrestler's)
  precise vec3 half_sum = v_unit + n_unit;
  vec3 half_vec = AvMulX3(half_sum, vec3(half_one));
  float inv_h = inversesqrt(abs(AvDot3(half_vec.zxy, half_vec.zxy)));
  vec3 h_unit = AvMulX3(half_vec, vec3(inv_h));
  float h_dot = AvDot3R(h_unit.zxy, fc.c[12].zxy);
  float h_term = (h_dot >= lit.x) ? h_dot : lit.x;
  float v_dot_n = AvDot3R(v_unit.zxy, n_unit.zxy);
  precise float rim_base = lit.y + (-abs(v_dot_n));
  precise float rim_log = AvMulX(log2(abs(rim_base)), fc.c[205].y);
  float rim = exp2(rim_log);
  precise float spec_rim = AvMulX(h_term, rim);
  precise float spec_w = AvMulX(spec_rim, clip.w);

  // the two point lights: c171 / c169 and c170 / c168 (direction, then position and range)
  float inv_dir_a = inversesqrt(abs(AvDot3(fc.c[171].zxy, fc.c[171].zxy)));
  vec3 dir_a = vec3(AvMulX(inv_dir_a, fc.c[171].x), AvMulX(inv_dir_a, fc.c[171].y), AvMulX(inv_dir_a, fc.c[171].z));
  float inv_dir_b = inversesqrt(abs(AvDot3(fc.c[170].zxy, fc.c[170].zxy)));
  vec3 dir_b = vec3(AvMulX(inv_dir_b, fc.c[170].x), AvMulX(inv_dir_b, fc.c[170].y), AvMulX(inv_dir_b, fc.c[170].z));
  precise vec3 from_a = vec3(world.x + (-fc.c[169].x), world.y + (-fc.c[169].y), world.z + (-fc.c[169].z));
  float len2_a = AvDot3(from_a.zxy, from_a.zxy);
  vec3 to_a = AvMulX3(from_a, vec3(inversesqrt(abs(len2_a))));
  precise vec3 from_b = vec3(world.x + (-fc.c[168].x), world.y + (-fc.c[168].y), world.z + (-fc.c[168].z));
  float len2_b = AvDot3(from_b.zxy, from_b.zxy);
  vec3 to_b = AvMulX3(from_b, vec3(inversesqrt(abs(len2_b))));
  float cone_a = AvSat(AvDot3R(dir_a.zxy, to_a.zxy));
  float cone_b = AvSat(AvDot3R(dir_b.zxy, to_b.zxy));
  precise float strength_a = AvMulX(fc.c[171].w, cone_a);
  precise float strength_b = AvMulX(fc.c[170].w, cone_b);
  precise float inv_range_a = 1.0 / fc.c[169].w;
  precise float inv_range_b = 1.0 / fc.c[168].w;
  precise float near_a = AvMulX(inv_range_a, sqrt(abs(len2_a)));
  precise float near_b = AvMulX(inv_range_b, sqrt(abs(len2_b)));
  float cap_a = (near_a < lit.y) ? near_a : lit.y;
  float cap_b = (near_b < lit.y) ? near_b : lit.y;
  precise float left_a = lit.y - cap_a;
  precise float left_b = lit.y - cap_b;
  precise float lights = AvMulX(strength_b, left_b) + AvMulX(strength_a, left_a);

  // the screen position and the depth bands
  precise float inv_w = 1.0 / clip.w;
  float ndc_x = AvMulX(clip.x, inv_w), ndc_y = AvMulX(clip.y, inv_w);
  float w_floor = (clip.w >= fc.c[196].x) ? clip.w : fc.c[196].x;
  float w_cap = (w_floor < fc.c[196].y) ? w_floor : fc.c[196].y;
  precise float band_from = (-fc.c[196].x) - (-w_cap);
  precise float band = AvMulX(fc.c[196].z, band_from);
  precise float fade_range = fc.c[162].y - fc.c[162].x;
  precise float fade_from = (-fc.c[162].x) - (-clip.w);
  precise float inv_fade_range = 1.0 / fade_range;
  float fade = AvSat(AvMulX(inv_fade_range, fade_from));
  precise float fog_range = fc.c[174].y + (-fc.c[174].x);
  precise float fog_span = fc.c[174].w + (-fc.c[174].z);
  precise float fog_from = clip.w + (-fc.c[174].x);
  precise float inv_fog_range = 1.0 / fog_range;
  precise float fog = AvMulX(AvSat(AvMulX(fog_from, inv_fog_range)), fog_span) + fc.c[174].z;

  // the colour's last byte, decoded into two indices (a high part and a low part scaled out of it)
  precise float high_scaled = AvMulX(lit.z, color_last);
  precise float low_scaled = AvMulX(lit.w, color_last);
  float high = trunc(high_scaled), low = trunc(low_scaled);
  precise float low_times = AvMulX(fc.c[252].y, low);
  precise float index_b = high + (-low_times);          // the material colour's index
  precise float high_times = AvMulX(fc.c[252].x, high);
  float index_a = trunc(high_times);                    // the picked number's index

  float picked = (index_a == 0.0) ? fc.c[231].x : fc.c[231].y;
  picked = ((index_a + fc.c[255].y) == 0.0) ? fc.c[231].z : picked;
  picked = ((index_a + fc.c[255].z) == 0.0) ? fc.c[231].w : picked;
  picked = ((index_a + fc.c[255].w) == 0.0) ? fc.c[232].x : picked;
  picked = ((index_a + fc.c[255].x) == 0.0) ? fc.c[232].y : picked;

  float pick = (index_b == 0.0) ? fc.c[237].x : fc.c[237].y;
  pick = ((index_b + fc.c[254].y) == 0.0) ? fc.c[237].z : pick;
  pick = ((index_b + fc.c[254].z) == 0.0) ? fc.c[237].w : pick;
  pick = ((index_b + fc.c[254].w) == 0.0) ? fc.c[238].x : pick;
  pick = ((index_b + fc.c[252].w) == 0.0) ? fc.c[238].y : pick;
  pick = ((index_b + fc.c[252].z) == 0.0) ? fc.c[238].z : pick;
  pick = ((index_b + fc.c[254].x) == 0.0) ? fc.c[238].w : pick;
  precise vec3 material = vec3(AvMulX(fc.c[145].x + fc.c[243].z, pick) + lit.y, AvMulX(fc.c[145].y + fc.c[243].z, pick) + lit.y,
                               AvMulX(fc.c[145].z + fc.c[243].z, pick) + lit.y);

  float inv_t = inversesqrt(abs(AvDot3(tangent.zxy, tangent.zxy)));

  out_normal = vec4(n_unit, 0.0);
  out_uv = vec4(halves.y, halves.x, band, picked);
  out_view = vec4(v_unit, 1.0);
  out_projected = vec4(AvDot4R(fc.c[68].zxyw, p), AvDot4R(fc.c[69].zxyw, p), AvDot4R(fc.c[166].zxyw, p), AvDot4R(fc.c[71].zxyw, p));
  out_lights = vec4(lights, 0.0, 0.0, 0.0);
  precise float screen_x = AvMulX(half_one, ndc_x) + half_one;
  precise float screen_y = AvMulX(-ndc_y, half_one) + half_one;
  out_screen = vec4(screen_x, screen_y, fade, fog);
  out_tangent = vec4(AvMulX3(vec3(inv_t), tangent), 0.0);
  out_material = vec4(material, alpha_scale);
  precise vec3 specular = AvMulX3(AvMulX3(vec3(spec_w), fc.c[4].xyz), vec3(fc.c[205].x));
  out_specular = vec4(specular, 0.0);
  out_shade = vec4(color_third, color_third, color_third, 0.0);
}
