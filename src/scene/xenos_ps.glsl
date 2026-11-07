// Session 80 (SCENE_SHADERS_PLAN.md): the colour output step of the translated pixel shaders - alpha test, exponent
// bias, the 360's piecewise-linear gamma - for our scene materials registered with their own epilogue
// (src\scene_swap.h; SDK vulkan/native/shader_swap.h kFlagOwnEpilogue: the swap then takes every draw of the shader
// but those with alpha to coverage). Our own code, following the SDK translator's CompleteFragmentShaderInMain and
// LinearToPWLGamma. The system constants are the translator's (set 1 binding 0); only the members used are declared.

layout(set = 1, binding = 0, std140) uniform XeSystemConstants {
  layout(offset = 0) uint flags;  // bits 16-18: alpha test pass if less / equal / greater; bit 19: colour 0 to gamma
  layout(offset = 272) float alpha_test_reference;
  layout(offset = 288) vec4 color_exp_bias;
} xe_system;

layout(location = 0) out vec4 out_color;

// (Session 80, the arena ours) The value the draw's target stores, as the translation writes it last: the SDK's swap
// sets this per translation (shader_swap.h kSpecColorQuantize; 0 none, 1 an 8-bit target, 2 a 16-bit fixed target -
// the 360's -32..32 range in the host's 16-bit normalised format). The same guest shader can draw into both kinds.
layout(constant_id = 0) const uint kXeColorQuantize = 0u;

// (Session 95, B1) 1 = the draw's colour epilogue is idle (no alpha test, exponent bias 1.0, no gamma: the translator's
// kNativeExtEpilogueIdle, which every native pipeline already keys on): the SDK's swap patches this per translation
// (shader_swap.h kSpecEpilogueIdle) and the epilogue below is compiled out - no discard, no branch, no multiply. 0 (the
// default) = the runtime epilogue as before.
layout(constant_id = 1) const uint kXeEpilogueIdle = 0u;

vec4 XeQuantize(vec4 c) {
  if (kXeColorQuantize != 0u) c = mix(c, vec4(0.0), isnan(c));  // (the translation's NClamp / NaN select)
  if (kXeColorQuantize == 1u) {
    precise vec4 scaled = clamp(c, 0.0, 1.0) * 255.0;
    precise vec4 level = trunc(scaled + 0.5);
    return level / 255.0;
  }
  if (kXeColorQuantize == 2u) {
    precise vec4 scaled = clamp(c, -32.0, 32.0) * (32767.0 / 32.0);
    precise vec4 level = trunc(scaled + mix(vec4(0.5), vec4(-0.5), lessThan(scaled, vec4(0.0))));
    return level / 32767.0;
  }
  return c;
}

// Linear to the 360's piecewise-linear gamma (saturated, NaN to 0): trunc(x * scale) / 255 + offset per piece.
vec3 XeLinearToPWLGamma(vec3 x) {
  x = clamp(x, 0.0, 1.0);
  bvec3 p3 = greaterThanEqual(x, vec3(512.0 / 1023.0));
  bvec3 p2 = greaterThanEqual(x, vec3(128.0 / 1023.0));
  bvec3 p1 = greaterThanEqual(x, vec3(64.0 / 1023.0));
  vec3 scale = mix(mix(vec3(1023.0), vec3(1023.0 / 2.0), p1), mix(vec3(1023.0 / 4.0), vec3(1023.0 / 8.0), p3), p2);
  vec3 offset = mix(mix(vec3(0.0), vec3(32.0 / 255.0), p1), mix(vec3(64.0 / 255.0), vec3(128.0 / 255.0), p3), p2);
  precise vec3 scaled = x * scale;
  precise vec3 gamma = trunc(scaled) * (1.0 / 255.0) + offset;
  return gamma;
}

// The material's colour for target 0, through the draw's alpha test, exponent bias and gamma.
void XeOutputColor(vec4 color) {
  // (Session 95, B1) kXeEpilogueIdle: the draw's epilogue does nothing, the colour goes out as it is. ONE store of out_color
  // in both cases (the SDK's alpha probe patch, shader_swap.cpp PatchAlphaProbe, needs a single store).
  vec4 c = color;
  if (kXeEpilogueIdle == 0u) {
    uint test = bitfieldExtract(xe_system.flags, 16, 3);
    if (test != 7u) {  // not "always"
      float a = color.a, ref = xe_system.alpha_test_reference;
      bool pass = test == 5u ? !(a == ref)  // "not equal" (true for NaN)
                             : ((test & 1u) != 0u && a < ref) || ((test & 2u) != 0u && a == ref) ||
                                   ((test & 4u) != 0u && a > ref);
      if (!pass) discard;
    }
    precise vec4 biased = color * xe_system.color_exp_bias.x;
    if ((xe_system.flags & (1u << 19)) != 0u) biased.rgb = XeLinearToPWLGamma(biased.rgb);
    c = biased;
  }
  out_color = XeQuantize(c);
}
