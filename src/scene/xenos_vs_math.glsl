// Session 87 (stage 4): the guest-arithmetic and vertex-decoding helpers shared by our own vertex shaders (arena_vs.glsl and
// the shaders that do not use the arena's lighting). Include after xenos_vs.glsl. Our own code.
#ifndef XENOS_VS_MATH_GLSL
#define XENOS_VS_MATH_GLSL

// ---- the guest's arithmetic -------------------------------------------------------------------------------------------

// A fused multiply-add (the translation's FMA mode). AV_PLAIN (the harness only) makes it the two roundings of the
// translation without FMA, to check the shaders against that module too.
#ifdef AV_PLAIN
float AvFma(float a, float b, float c) { precise float p = a * b; precise float s = p + c; return s; }
vec2 AvFma(vec2 a, vec2 b, vec2 c) { precise vec2 p = a * b; precise vec2 s = p + c; return s; }
vec3 AvFma(vec3 a, vec3 b, vec3 c) { precise vec3 p = a * b; precise vec3 s = p + c; return s; }
vec4 AvFma(vec4 a, vec4 b, vec4 c) { precise vec4 p = a * b; precise vec4 s = p + c; return s; }
#else
float AvFma(float a, float b, float c) { return fma(a, b, c); }
vec2 AvFma(vec2 a, vec2 b, vec2 c) { return fma(a, b, c); }
vec3 AvFma(vec3 a, vec3 b, vec3 c) { return fma(a, b, c); }
vec4 AvFma(vec4 a, vec4 b, vec4 c) { return fma(a, b, c); }

// A dot product the way the translation sums it with fused adds: the first product rounded, then each next one fused into
// the sum, in the order of the (already swizzled) operands.
float AvDot3(vec3 a, vec3 b) {
  precise float s = a.x * b.x;
  s = AvFma(a.y, b.y, s);
  s = AvFma(a.z, b.z, s);
  return s;
}
float AvDot4(vec4 a, vec4 b) {
  precise float s = a.x * b.x;
  s = AvFma(a.y, b.y, s);
  s = AvFma(a.z, b.z, s);
  s = AvFma(a.w, b.w, s);
  return s;
}
// The guest's multiply (0 times anything, an infinity too, is 0), for products that can meet an infinity (a reciprocal root's
// result): the translation keeps the rule there. The minimum of the magnitudes ignoring a NaN, as its NMin.
float AvNMin(float a, float b) { return isnan(a) ? b : (isnan(b) ? a : min(a, b)); }
float AvMulX(float a, float b) {
  precise float p = a * b;
  return AvNMin(abs(a), abs(b)) == 0.0 ? 0.0 : p;
}
vec2 AvMulX(vec2 a, vec2 b) { return vec2(AvMulX(a.x, b.x), AvMulX(a.y, b.y)); }
vec3 AvMulX3(vec3 a, vec3 b) { return vec3(AvMulX(a.x, b.x), AvMulX(a.y, b.y), AvMulX(a.z, b.z)); }

// The guest's vector max: a if a >= b, else b (a NaN in a gives b).
vec4 AvMax4(vec4 a, vec4 b) {
  return vec4(a.x >= b.x ? a.x : b.x, a.y >= b.y ? a.y : b.y, a.z >= b.z ? a.z : b.z, a.w >= b.w ? a.w : b.w);
}
// The guest's "set if at least": 1 for |x| >= 0 (0 for a NaN) - the position's w, taken from a register that holds the vertex
// index (or, when a texture coordinate was fetched over it, that).
float AvOne(float r) { return abs(r) >= 0.0 ? 1.0 : 0.0; }
// The guest's saturate: 0..1, a NaN giving 0 (the translation's NClamp).
float AvSat(float x) { return isnan(x) ? 0.0 : clamp(x, 0.0, 1.0); }

// ---- the vertex ---------------------------------------------------------------------------------------------------------

vec4 AvFetchPosition(int at) {
  return vec4(uintBitsToFloat(XeFetchDword(at)), uintBitsToFloat(XeFetchDword(at + 1)),
              uintBitsToFloat(XeFetchDword(at + 2)), uintBitsToFloat(XeFetchDword(at + 3)));
}
// 11:11:10 signed, scaled to -1..1 (the first component 10 bits), floored at -1.
vec3 AvFetchNormal(int at) {
  int v = int(XeFetchDword(at + 5));
  precise vec3 n = vec3(bitfieldExtract(v, 0, 10), bitfieldExtract(v, 10, 11), bitfieldExtract(v, 21, 11)) *
                   vec3(0.00195694715, 0.000977517106, 0.000977517106);
  return max(n, vec3(-1.0));
}
// Four bytes /255, the guest register holding them as (third, second, first, fourth).
vec4 AvFetchColor(int at) {
  uint v = XeFetchDword(at + 6);
  precise vec4 c = vec4(bitfieldExtract(v, 0, 8), bitfieldExtract(v, 8, 8), bitfieldExtract(v, 16, 8), bitfieldExtract(v, 24, 8)) *
                   vec4(0.00392156886);
  return c.zyxw;
}
// Four bytes /255 of the dword at `address`, in the guest's (third, second, first, fourth) order, as AvFetchColor does at +6.
vec4 AvFetchColorAt(int address) {
  uint v = XeFetchDword(address);
  precise vec4 c = vec4(bitfieldExtract(v, 0, 8), bitfieldExtract(v, 8, 8), bitfieldExtract(v, 16, 8), bitfieldExtract(v, 24, 8)) *
                   vec4(0.00392156886);
  return c.zyxw;
}
// The two halves of dword 7 as the guest's unpack gives them: x the low half, y the high half. The texture coordinate is (y, x).
vec2 AvFetchHalves(int at) { return unpackHalf2x16(XeFetchDword(at + 7)); }

#endif
// Dot products with the guest's multiply rule on every product and plain adds in the operands' order.
float AvDot4R(vec4 a, vec4 b) {
  precise float s = AvMulX(a.x, b.x) + AvMulX(a.y, b.y);
  precise float t = s + AvMulX(a.z, b.z);
  precise float u = t + AvMulX(a.w, b.w);
  return u;
}
float AvDot3R(vec3 a, vec3 b) {
  precise float s = AvMulX(a.x, b.x) + AvMulX(a.y, b.y);
  precise float t = s + AvMulX(a.z, b.z);
  return t;
}

#endif
