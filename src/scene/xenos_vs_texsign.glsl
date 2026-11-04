// Session 87 (stage 4): the sign modes of a vertex texture fetch (shared by xenos_vs_tex.glsl, fetch constant 16, and
// xenos_vs_tex17.glsl, fetch constant 17). Our own code, following the SDK translator's tfetch for a vertex shader.
#ifndef XENOS_VS_TEXSIGN_GLSL
#define XENOS_VS_TEXSIGN_GLSL

// One channel through its sign mode: 0 as fetched, 1 from the signed image, 2 biased (x * 2 - 1), 3 the 360's gamma curve.
float XeVertexSign(uint mode, float unsigned_value, float signed_value) {
  if (mode == 1u) return signed_value;
  if (mode == 2u) {
    precise float doubled = unsigned_value * 2.0;
    precise float biased = doubled + (-1.0);
    return biased;
  }
  if (mode == 3u) {
    float c = clamp(unsigned_value, 0.0, 1.0);
    bool high = c >= 0.752941191196441650390625;
    bool low = c >= 0.2509804069995880126953125;
    bool middle = c >= 0.3764705955982208251953125;
    float scale = middle ? (high ? 0.0078125 : 0.00390625) : (low ? 0.001953125 : 0.0009765625);
    precise float a = c * 261120.0;
    precise float b = a * scale;
    precise float d = b + (middle ? (high ? -1024.0 : -256.0) : (low ? -64.0 : 0.0));
    precise float e = d * scale;
    precise float f = d + trunc(e);
    precise float g = f * 0.000977517105638980865478515625;
    return g;
  }
  return unsigned_value;
}

#endif
