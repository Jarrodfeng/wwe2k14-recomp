#version 450
// Planar shadow of a character, vertex stage. See docs\renderer\effects\yPlaneShadowMap.md.
// Options: FX_SKINNED (up to five bones chosen by the bool constants), FX_RIGID (one bone, switched by bool 0),
//          FX_FLAT (a prop: moved by TraMovPos, drawn with ViewProj, and reads the shadow map back).
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(set = 0, binding = 0, std140) uniform VertexConstants {
#if defined(FX_SKINNED) || defined(FX_RIGID)
  vec4 EyePos;              // the point the shadow is cast from
  vec4 PlanePos;            // y: height of the ground plane
  vec4 ShadowDistance;      // x: distance at which the shadow has faded out
  vec4 ShadowRevision;      // x: fade exponents and depths of the three terms
  vec4 ShadowMiddleRev;
  vec4 ShadowLeastRev;
  vec4 ShadowDeep;
  vec4 ShadowMiddleDeep;
  vec4 ShadowLeastDeep;
  vec4 WorldMatrixArray[63];
  uvec4 Bools[2];
#endif
#ifdef FX_FLAT
  vec4 TraMovPos;
  vec4 ViewProj[4];
#endif
  vec4 CameraViewProj[4];
  vec4 PlaneInv[4];
} vc;

layout(location = 0) in vec4 inPosition;
#ifdef FX_SKINNED
layout(location = 1) in vec4 inWeights;
layout(location = 2) in vec4 inBoneIds;
layout(location = 3) in float inBoneId4;
#endif
#ifdef FX_RIGID
layout(location = 1) in float inBoneId;
#endif

layout(location = 0) out vec4 outShadow;

#if defined(FX_SKINNED) || defined(FX_RIGID)
vec3 bone_point(int id, vec4 p) {
  int b = 3 * id;
  return vec3(dot(vc.WorldMatrixArray[b], p), dot(vc.WorldMatrixArray[b + 1], p), dot(vc.WorldMatrixArray[b + 2], p));
}
#endif

vec4 mul4(vec4 m[4], vec4 v) { return vec4(dot(m[0], v), dot(m[1], v), dot(m[2], v), dot(m[3], v)); }

void main() {
#if defined(FX_SKINNED) || defined(FX_RIGID)
#ifdef FX_SKINNED
  vec3 world;
  if (fx_bool(vc.Bools, 4)) {
    float rest = 1.0 - inWeights.x - inWeights.y - inWeights.z - inWeights.w;
    world = bone_point(int(inBoneIds.x), inPosition) * inWeights.x + bone_point(int(inBoneIds.y), inPosition) * inWeights.y +
            bone_point(int(inBoneIds.z), inPosition) * inWeights.z + bone_point(int(inBoneIds.w), inPosition) * inWeights.w +
            bone_point(int(inBoneId4), inPosition) * rest;
  } else if (fx_bool(vc.Bools, 3)) {
    float rest = 1.0 - inWeights.x - inWeights.y - inWeights.z;
    world = bone_point(int(inBoneIds.x), inPosition) * inWeights.x + bone_point(int(inBoneIds.y), inPosition) * inWeights.y +
            bone_point(int(inBoneIds.z), inPosition) * inWeights.z + bone_point(int(inBoneIds.w), inPosition) * rest;
  } else if (fx_bool(vc.Bools, 2)) {
    float rest = 1.0 - inWeights.x - inWeights.y;
    world = bone_point(int(inBoneIds.x), inPosition) * inWeights.x + bone_point(int(inBoneIds.y), inPosition) * inWeights.y +
            bone_point(int(inBoneIds.z), inPosition) * rest;
  } else if (fx_bool(vc.Bools, 1)) {
    world = mix(bone_point(int(inBoneIds.y), inPosition), bone_point(int(inBoneIds.x), inPosition), inWeights.x);
  } else {
    world = bone_point(int(inBoneIds.x), inPosition);
  }
#else
  vec3 world = bone_point(fx_bool(vc.Bools, 0) ? int(inBoneId) : 0, inPosition);
#endif

  // Project the vertex from the eye point onto the ground plane.
  vec3 fromEye = world - vc.EyePos.xyz;
  // A vertex level with the eye never reaches the plane: its shadow is infinitely far (fully faded). The game's shaders give a zero step when the plane is level too.
  float planeDrop = vc.PlanePos.y - vc.EyePos.y;
  float step = fromEye.y != 0.0 ? planeDrop / fromEye.y : (planeDrop != 0.0 ? 1.0e30 : 0.0);
  vec3 onPlane = vc.EyePos.xyz + fromEye * step;
  vec4 pos = mul4(vc.PlaneInv, mul4(vc.CameraViewProj, vec4(onPlane, 1.0)));

  // The shadow fades with the distance from the vertex to its shadow: three power terms of the remaining fraction.
  float remaining = 1.0 - clamp(length(onPlane - world) / vc.ShadowDistance.x, 0.0, 1.0);
  float terms = fx_pow(remaining, vc.ShadowMiddleRev.x) * vc.ShadowMiddleDeep.x + fx_pow(remaining, vc.ShadowLeastRev.x) * vc.ShadowLeastDeep.x;
  float darkness = terms + fx_pow(remaining, vc.ShadowRevision.x) * vc.ShadowDeep.x;
  outShadow = vec4(darkness, 0.0, 0.0, 1.0);
  gl_Position = vec4(pos.x, pos.y, (1.0 - clamp(darkness, 0.0, 1.0)) * pos.w, pos.w);
#else
  vec4 pos = mul4(vc.ViewProj, vec4(inPosition.xyz + vc.TraMovPos.xyz, 1.0));
  vec4 clip = mul4(vc.CameraViewProj, vec4(inPosition.xyz + vc.TraMovPos.xyz, 1.0));
  vec4 map = mul4(vc.PlaneInv, clip);
  float invW = map.w != 0.0 ? 1.0 / map.w : 0.0;   // a degenerate shadow projection gives the game's zero
  outShadow = vec4(vec2(0.5 * map.x * invW + 0.5, -0.5 * map.y * invW + 0.5) * pos.w, pos.w, pos.w);
  gl_Position = pos;
#endif
}
