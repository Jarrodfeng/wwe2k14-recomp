// Session 95 (r3gpu, gpu_native_wrestler_pow, default off): the entrance wrestler materials' powers with a whole exponent. The game hands these exponents in as constants: in Bryan's entrance
// (run 001 of out\s95\r3gpu, [native][passconst]) shadow pair 1 is 4, pair 2 is 20, the fresnel 2, the lighting map 1, the sky rim 1. A power is two extended-maths operations here (log2 + exp2),
// a quarter rate on Intel Gen9; these take a uniform branch and a few multiplies, and any other exponent goes the old way. The result differs from exp2(p * log2(x)) by float rounding only.
// Included under POWLEAN after the material's own pow, with POW_ANY(x, p) defined as that pow.
#extension GL_EXT_control_flow_attributes : require
float PowLean1(float x, float p) {
  [[dont_flatten]] if (p == 1.0) return abs(x);
  return POW_ANY(x, p);
}
float PowLean2(float x, float p) {
  [[dont_flatten]] if (p == 2.0) return x * x;
  return POW_ANY(x, p);
}
float PowLean4(float x, float p) {
  [[dont_flatten]] if (p == 4.0) {
    float y = x * x;
    return y * y;
  }
  return POW_ANY(x, p);
}
float PowLean20(float x, float p) {
  [[dont_flatten]] if (p == 20.0) {
    float y = x * x;
    y = y * y * x;  // x^5
    y = y * y;
    return y * y;
  }
  return POW_ANY(x, p);
}
