// Session 87 (stage 4): the body of the crowd's individual spectator figures (the game's yAudDefault skinned models; vs_crowd_figure_*.vert).
// Hand-written from our reading of the guest shaders; checked bit for bit against the SDK's translation by
// tools/shaders/harness/vs_harness.py. Include after xenos_vs.glsl, xenos_vs_math.glsl, xenos_vs_skin.glsl, xenos_vs_tex.glsl and vs_ch.glsl.
// These figures are drawn one model at a time (the lone ringside spectators and the figures baked into the crowd atlas), not as the
// instanced runs our own crowd program draws (own_crowd_*).
//
// An 11-dword skinned vertex: position (4 floats), three blend weights at dword 4 (f.x weighs bone 0, f.y bone 1, f.z bone 2), four bone
// bytes at 7, a packed normal at 8, a colour at 9, a texture coordinate at 10. The bool constants choose the blend: b1 = the first bone
// alone; else b2 = bones 0 and 1; else b3 = bones 0, 1 and 2; else none. What the blend leaves of the weights (one less their sum)
// goes to one more bone, picked by a small number the blend leaves (compared with 0 and two literals). The match's figures read the
// arena's lighting map per vertex (the vertex texture of fetch constant 16, rgba); the entrances' hand the map coordinate on
// (CROWD_FIGURE_ENTRANCE). Float constants: the whole block by number (kDynamic).

layout(location = 0) invariant out vec4 out_uv;
layout(location = 1) invariant out vec4 out_color;

void main() {
  uint index = XeGuestVertexIndex();
  int at = XeVertexAddress(index, 11, 95);
  vec4 lit254 = fc.c[254];  // 0, the bone stride, 1/2, ...
  vec4 lit255 = fc.c[255];
  vec4 position = AvFetchPosition(at);
  // the weights' lanes as the guest lays them out: (z, y, x) of the three floats
  float wf_x = XeFetchFloat(at + 4), wf_y = XeFetchFloat(at + 5), wf_z = XeFetchFloat(at + 6);
  uint packed_bytes = XeFetchDword(at + 7);
  vec4 bytes = vec4(bitfieldExtract(packed_bytes, 0, 8), bitfieldExtract(packed_bytes, 8, 8), bitfieldExtract(packed_bytes, 16, 8),
                    bitfieldExtract(packed_bytes, 24, 8));
  vec3 normal = ChFetchNormal(at + 8);
  vec4 vertex_color = AvFetchColorAt(at + 9);
  vec2 halves = unpackHalf2x16(XeFetchDword(at + 10));

  float index_value = float(index);
  float zero_flag = ((-abs(index_value)) > lit255.x) ? 1.0 : 0.0;  // (0 for any vertex)
  vec3 acc_p = vec3(zero_flag), acc_n = vec3(zero_flag);
  float selector = 0.0;
  float weight_sum = 0.0;    // what the weights sum to when no blend ran
  float stride = lit254.y;
  int ka = XeAddress(AvMulX(bytes.x, stride));
  int kb = XeAddress(AvMulX(bytes.y, stride));
  int kc = XeAddress(AvMulX(bytes.z, stride));
  if (!XeBool(1)) {
    if (XeBool(2)) {
      // bones 0 and 1 with the weights f.x and f.y
      precise float w01 = wf_x + wf_y;
      vec3 p0 = AvBone(position, ka), p1 = AvBone(position, kb);
      vec3 n0 = ChTurn(normal, ka), n1 = ChTurn(normal, kb);
      precise vec3 pp = AvMulX3(p0, vec3(wf_x)) + AvMulX3(p1, vec3(wf_y));
      precise vec3 nn = AvMulX3(n0, vec3(wf_x)) + AvMulX3(n1, vec3(wf_y));
      acc_p = pp; acc_n = nn;
      weight_sum = w01;
      selector = lit254.x;
    } else if (XeBool(3)) {
      // bones 0, 1 and 2 with the weights f.x, f.y and f.z (summed bone 2, bone 0, bone 1)
      precise float w01 = wf_x + wf_y;
      precise float w012 = w01 + wf_z;
      vec3 p0 = AvBone(position, ka), p1 = AvBone(position, kb), p2 = AvBone(position, kc);
      vec3 n0 = ChTurn(normal, ka), n1 = ChTurn(normal, kb), n2 = ChTurn(normal, kc);
      acc_p = vec3(AvDot3R(vec3(p2.x, p0.x, p1.x), vec3(wf_z, wf_x, wf_y)), AvDot3R(vec3(p2.y, p0.y, p1.y), vec3(wf_z, wf_x, wf_y)),
                   AvDot3R(vec3(p2.z, p0.z, p1.z), vec3(wf_z, wf_x, wf_y)));
      acc_n = vec3(AvDot3R(vec3(n2.x, n0.x, n1.x), vec3(wf_z, wf_x, wf_y)), AvDot3R(vec3(n2.y, n0.y, n1.y), vec3(wf_z, wf_x, wf_y)),
                   AvDot3R(vec3(n2.z, n0.z, n1.z), vec3(wf_z, wf_x, wf_y)));
      weight_sum = w012;
      selector = lit254.y;
    }
  } else {
    // the first bone alone, with the weight f.x
    vec3 p0 = AvBone(position, ka);
    vec3 n0 = ChTurn(normal, ka);
    acc_p = AvMulX3(p0, vec3(wf_x));
    acc_n = AvMulX3(n0, vec3(wf_x));
    weight_sum = wf_x;
    selector = (abs(bytes.x) >= lit255.x) ? 1.0 : 0.0;
  }

  // the bone the rest of the weight goes to
  float sel_byte = (selector == 0.0) ? bytes.x : bytes.y;
  sel_byte = ((selector + lit255.w) == 0.0) ? bytes.z : sel_byte;
  sel_byte = ((selector + lit255.z) == 0.0) ? bytes.w : sel_byte;
  int ks = XeAddress(AvMulX(stride, sel_byte));
  precise float rest = (-weight_sum) + lit255.y;
  vec3 ps_ = AvBone(position, ks);
  vec3 ns_ = ChTurn(normal, ks);
  precise vec3 world = AvMulX3(ps_, vec3(rest)) + acc_p;
  precise vec3 turned = AvMulX3(ns_, vec3(rest)) + acc_n;

  // the lighting-map colour at the world position
  float up = AvDot3R(-turned.zxy, fc.c[180].zxy);
  precise float up_half = AvMulX(up, lit254.z);
  precise float half_x = AvMulX(world.x, lit254.z);
  precise float half_z = AvMulX(world.z, lit254.z);
  precise float map_u = AvMulX(half_x, fc.c[184].x) + lit254.z;
  precise float map_v = AvMulX(half_z, fc.c[184].y) + lit254.z;
#ifndef CROWD_FIGURE_ENTRANCE
  vec4 map = XeVertexTexture16Rgba(vec2(map_u, map_v));
#endif

  // the clip position
  vec4 p = vec4((lit255.x == 0.0) ? world.z : lit255.y, (lit255.x == 0.0) ? world.x : lit255.y, (lit255.x == 0.0) ? world.y : lit255.y,
                (lit255.y == 0.0) ? world.y : lit255.y);
  XeOutputPosition(vec4(AvDot4R(fc.c[56].zxyw, p), AvDot4R(fc.c[57].zxyw, p), AvDot4R(fc.c[58].zxyw, p), AvDot4R(fc.c[59].zxyw, p)));

  // the four lights and the ambient
  float t0 = AvDot3R(-turned.zxy, -fc.c[8].zxy), t1 = AvDot3R(-turned.zxy, -fc.c[9].zxy);
  float t2 = AvDot3R(-turned.zxy, -fc.c[10].zxy), t3 = AvDot3R(-turned.zxy, -fc.c[11].zxy);
  t3 = (t3 >= fc.c[74].w) ? t3 : fc.c[74].w;
  t2 = (t2 >= fc.c[74].z) ? t2 : fc.c[74].z;
  t1 = (t1 >= fc.c[74].y) ? t1 : fc.c[74].y;
  t0 = (t0 >= fc.c[74].x) ? t0 : fc.c[74].x;
  precise vec3 sum = AvMulX3(vec3(t3), fc.c[3].xyz);
  sum = AvMulX3(vec3(t2), fc.c[2].xyz) + sum;
  sum = AvMulX3(vec3(t1), fc.c[1].xyz) + sum;
  sum = AvMulX3(vec3(t0), fc.c[0].xyz) + sum;
  precise float facing = lit254.z + up_half;
  precise vec3 span = fc.c[182].xyz + (-fc.c[181].xyz);
  precise vec3 blended = AvMulX3(span, vec3(facing)) + fc.c[181].xyz;
  precise vec3 ambient = AvMulX3(blended, fc.c[189].xyz);
  precise vec3 lit_color = AvMulX3(sum, fc.c[190].xyz) + ambient;
#ifndef CROWD_FIGURE_ENTRANCE
  vec4 scaled_map = vec4(AvMulX(map.x, fc.c[184].z), AvMulX(map.y, fc.c[184].z), AvMulX(map.z, fc.c[184].z), AvMulX(map.w, fc.c[184].z));
  vec3 base = AvMulX3(scaled_map.xyz, vertex_color.xyz);
#endif

#ifdef CROWD_FIGURE_ENTRANCE
  out_uv = vec4(halves.y, halves.x, map_u, map_v);
  out_color = vec4(AvMulX3(lit_color, vertex_color.xyz), vertex_color.w);
}
#else
  out_uv = vec4(halves.y, halves.x, 0.0, 0.0);
  out_color = vec4(AvMulX3(base, lit_color), AvMulX(vertex_color.w, scaled_map.w));
}
#endif
