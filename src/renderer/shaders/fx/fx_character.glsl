// Shared by the character effects (yCh_*). See docs\renderer\effects\yCh_Ls.md.
#ifndef FX_CHARACTER_GLSL
#define FX_CHARACTER_GLSL

// ---- Vertex side. The shader defines CH_BONES (its vec4[63] of 4x3 bone rows) before including this file.
#ifdef CH_BONES
int ch_bone(float id) { return 3 * int(id); }
vec3 ch_bone_point(int b, vec4 p) { return vec3(dot(CH_BONES[b], p), dot(CH_BONES[b + 1], p), dot(CH_BONES[b + 2], p)); }
vec3 ch_bone_turn(int b, vec3 n) { return vec3(dot(CH_BONES[b].xyz, n), dot(CH_BONES[b + 1].xyz, n), dot(CH_BONES[b + 2].xyz, n)); }

// Blend position and normal across 1-4 bones. The last weight in use is whatever is left of 1.
// With four bones the normal follows only the first three: the game's own shortcut, kept.
void ch_skin(vec4 position, vec3 normal, vec3 w, vec4 ids, int boneCount, out vec3 worldPos, out vec3 worldNormal) {
  int b0 = ch_bone(ids.x);
  vec3 p0 = ch_bone_point(b0, position);
  vec3 n0 = ch_bone_turn(b0, normal);
  if (boneCount == 1) { worldPos = p0; worldNormal = n0; return; }
  int b1 = ch_bone(ids.y);
  vec3 p1 = ch_bone_point(b1, position);
  vec3 n1 = ch_bone_turn(b1, normal);
  if (boneCount == 2) { worldPos = mix(p1, p0, w.x); worldNormal = mix(n1, n0, w.x); return; }
  int b2 = ch_bone(ids.z);
  vec3 p2 = ch_bone_point(b2, position);
  vec3 n2 = ch_bone_turn(b2, normal);
  float rest = 1.0 - w.x - w.y;
  worldNormal = n0 * w.x + n1 * w.y + n2 * rest;
  if (boneCount == 3) { worldPos = p0 * w.x + p1 * w.y + p2 * rest; return; }
  worldPos = p0 * w.x + p1 * w.y + p2 * w.z + ch_bone_point(ch_bone(ids.w), position) * (rest - w.z);
}
#endif

// ---- Pixel side.
// The geometric normal bent by the tangent-space offset of the normal map, in a tangent frame built around the world's vertical axis.
// flip: x, y scale the tangent and binormal parts, z the geometric normal.
vec3 ch_bent_normal(vec3 N, vec2 tangentOffset, vec3 flip) {
  float flatSq = dot(N.xz, N.xz);
  float invFlat = flatSq > 0.0 ? inversesqrt(flatSq) : 0.0;
  vec3 tangent = vec3(N.z, 0.0, -N.x) * invFlat;
  vec3 binormal = vec3(-N.x * N.y, flatSq, -N.z * N.y) * invFlat;
  return N * flip.z + tangent * tangentOffset.x * flip.x + binormal * tangentOffset.y * flip.y;
}

// How much a shadow map layer pair darkens the surface. map: x, y one layer's near and far depths; z, w the other's.
// depthRange: x start, y end, z bias, w strength. softness: y facing bias, z near slope, w near power. farTerm: x start, y slope, z power, w far weight.
float ch_shadow_layer(vec2 gap, vec4 softness, vec4 farTerm) {
  float nearPart = 1.0 - fx_pow(1.0 - clamp(gap.x * softness.z, 0.0, 1.0), softness.w);
  float farPart = fx_pow(1.0 - clamp((gap.y - farTerm.x) * farTerm.y, 0.0, 1.0), farTerm.z);
  return nearPart + (nearPart * farPart - nearPart) * farTerm.w;
}
float ch_shadow_amount(vec4 map, float receiverZ, vec4 depthRange, vec4 softness, vec4 farTerm, float normalDotLight) {
  float receiver = clamp((receiverZ - depthRange.x) / (depthRange.y - depthRange.x) - depthRange.z, 0.0, 1.0);
  vec4 gap = vec4(receiver) - map;
  float darkest = max(ch_shadow_layer(gap.xy, softness, farTerm), ch_shadow_layer(gap.zw, softness, farTerm));
  float facing = 1.0 - (1.0 - max(normalDotLight, 0.0)) * softness.y;
  return depthRange.w * darkest * facing;
}

#endif
