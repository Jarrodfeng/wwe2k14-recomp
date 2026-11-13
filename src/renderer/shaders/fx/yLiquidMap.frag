#version 450
// Sweat / liquid accumulation map, pixel stage. See docs\renderer\effects\yLiquidMap.md.
// Reads the previous map (slot 0) and the source map (slot 1, the new liquid) and writes the updated map.
// Options: FX_SWEAT (adds new liquid to the previous map), FX_PASTE (pastes one channel), FX_COPY_PREV (copies the previous map),
//          FX_TINT (writes the source as a coloured amount), FX_LOD (plain read of the source at a chosen mip), FX_DIM (the softer variant of SWEAT, COPY_PREV and TINT).
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

#if defined(FX_SWEAT) || defined(FX_PASTE) || defined(FX_TINT)
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 SrcChan;           // which channels of the source count and how much
  vec4 DstChan;           // how much of the new amount goes to each channel of the map
} fc;
#elif defined(FX_LOD)
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 TexParam;          // w: mip level
} fc;
#endif

layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(location = 0) in vec4 inUv;
layout(location = 1) in vec4 inParams;
layout(location = 0) out vec4 outColor;

float strongest(vec4 v) { return max(max(v.x, v.y), max(v.z, v.w)); }

void main() {
#if defined(FX_SWEAT)
  vec3 previous = textureLod(textures[0], inUv.zw, 0.0).xyz;
  float amount = strongest(texture(textures[1], inUv.xy) * fc.SrcChan);
  float spread = fx_pow(1.0 - inParams.x * inParams.y, 50.0);
  float boosted = amount + (min((1.0 - spread) * amount, 1.0) - amount) * fc.DstChan.z;
  vec3 color = (1.0 - fc.DstChan.xyz) * previous + vec3(amount, amount, boosted) * fc.DstChan.xyz;
#ifdef FX_DIM
  color.yz *= 0.775;
#endif
  outColor = vec4(color, amount * inParams.w);
#elif defined(FX_PASTE)
  float previous = textureLod(textures[0], inUv.zw, 0.0).x;
  float amount = strongest(texture(textures[1], inUv.xy) * fc.SrcChan);
  outColor = vec4((1.0 - fc.DstChan.x) * previous + amount * fc.DstChan.x, 1.0, 1.0, amount * inParams.w);
#elif defined(FX_COPY_PREV)
  vec3 previous = textureLod(textures[0], inUv.zw, 0.0).xyz;
#ifdef FX_DIM
  previous.yz *= 0.775;
#endif
  outColor = vec4(previous, 1.0);
#elif defined(FX_TINT)
  float amount = strongest(texture(textures[1], inUv.xy) * fc.SrcChan);
#ifdef FX_DIM
  outColor = vec4(amount * vec3(0.15 * 0.0 + 1.0, 0.15, 0.15) * fc.DstChan.xyz, amount);
#else
  outColor = vec4(amount * fc.DstChan.xyz, amount);
#endif
#else
  outColor = textureLod(textures[1], inUv.xy, fc.TexParam.w);
#endif
}
