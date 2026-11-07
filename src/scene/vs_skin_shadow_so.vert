#version 450
#extension GL_GOOGLE_include_directive : require
// Session 95 (gpu_native_skin_once): the skin-once variant of the shadow map shaders (vs_skin_shadow*.vert, every vertex layout): the world position comes from the frame's skinned vertex buffer
// (src/scene/skin_once.comp); the rest is their last lines.
#define XE_SKIN_ONCE
#define XE_SO_UV -1
#define XE_SO_QUAD -2
#include "xenos_vs.glsl"
#include "xenos_vs_math.glsl"
#include "xenos_vs_skin.glsl"

layout(location = 0) invariant out vec4 out_depth;

void main() {
  vec3 w = uintBitsToFloat(xe_s0.xyz);
  vec4 r0 = vec4((C255.x == 0.0) ? w.z : C255.y, (C255.x == 0.0) ? w.x : C255.y, (C255.x == 0.0) ? w.y : C255.y,
                 (C255.y == 0.0) ? w.y : C255.y);
  vec4 r1;
  r1.x = AvDot4R(C56.zxyw, r0);
  r1.y = AvDot4R(C57.zxyw, r0);
  r1.z = AvDot4R(C58.zxyw, r0);
  r1.w = AvDot4R(C59.zxyw, r0);
  XeOutputPosition(vec4(AvDot4R(r1.wzxy, C64.wzxy), AvDot4R(r1.wzxy, C65.wzxy), AvDot4R(r1.wzxy, C66.wzxy),
                        AvDot4R(r1.wzxy, C67.wzxy)));
  out_depth = vec4(AvDot4R(C54.zxyw, r0), 0.0, 0.0, 0.0);
}
