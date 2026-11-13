#version 450
// The crowd's far cards, INSTANCED (Session 101, lane CD; docs\renderer\CROWD_CARDS_PROMOTION_MAP.md sections 2 and 3). Our own code, written from the arithmetic
// the game's far-card vertex stage (yAudience, variant lm_uv) already holds: the card quad is placed by a world matrix, projected by ViewProj, and the pixel stage
// gets the picture's uv plus the light-map uv the world position selects. The only difference: the three WorldMatrix rows (the game's c76-c78, which change from one
// card to the next) arrive per INSTANCE (binding 1) instead of from the constant block, so one draw places every card of a person type.
// The constant block keeps the lm_uv variant's exact layout (ViewProj, WorldMatrix, LightMapParam) so the first card draw's packed block is pushed as it is; the
// block's WorldMatrix is not read here. Outputs are the lm_uv ones, so the game's own pixel stage (yAudience / lm_pixel_color_alpha, and its alpha-test twin) draws it.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 ViewProj[4];
  vec4 WorldMatrix[3];      // (the block's layout only: the per-instance rows below replace it)
  vec4 LightMapParam;       // xy world-to-light-map scale, z brightness
} vc;

layout(set = 1, binding = 1) uniform sampler2D textures[4];

layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec2 inUv;
layout(location = 4) in vec4 inWorld0;
layout(location = 5) in vec4 inWorld1;
layout(location = 6) in vec4 inWorld2;

layout(location = 0) out vec4 outUv;
layout(location = 1) out vec4 outShade;

void main() {
  vec4 local = vec4(inPosition, 1.0);
  vec3 world = vec3(dot(inWorld0, local), dot(inWorld1, local), dot(inWorld2, local));
  vec4 world4 = vec4(world, 1.0);
  gl_Position = vec4(dot(vc.ViewProj[0], world4), dot(vc.ViewProj[1], world4), dot(vc.ViewProj[2], world4), dot(vc.ViewProj[3], world4));
  // The baked light map is laid out over the ground plane: world x and z.
  vec2 lightMapUv = (world.xz * 0.5) * vc.LightMapParam.xy + 0.5;
#ifdef FX_LM_VERTEX
  // Lane 219: the arenas whose far cards run the game's lm_vertex stage (yAudience variant lm_vertex: the page is read here, once per corner of the quad, and the pixel stage gets the shade).
  vec4 lightMap = textureLod(textures[0], lightMapUv, 0.0) * vc.LightMapParam.z;
  outUv = vec4(inUv, 0.0, 0.0);
  outShade = vec4(vec3(1.0) * lightMap.rgb, lightMap.a);
#else
  outUv = vec4(inUv, lightMapUv);
  outShade = vec4(1.0);
#endif
}
