// Session 87 (stage 4): the parts of our skinned vertex shaders (shadow maps, rim light, plane shadows, props and wrestlers) that
// the guest shaders share: the float constants indexed by register number (bone matrices at c[76 + a0]), the bool constants
// their branches test, the address register's rounding, and the dot products and multiplies the translation keeps the
// guest's 0 x infinity = 0 rule on (indexed constants are never proven finite). Include after xenos_vs.glsl and
// xenos_vs_math.glsl. Our own code.

// All 256 float registers by number (the translation of a shader that indexes its constants is not packed).
layout(set = 1, binding = 1, std140) uniform XeFloatConstants { vec4 c[256]; } fc;
layout(set = 1, binding = 3, std140) uniform XeBoolLoopConstants {
  uvec4 bool_constants[2];
  uvec4 loop_constants[8];
} xe_bool;

// The guest's bool constant b (a branch condition of the shader).
bool XeBool(int b) { return (xe_bool.bool_constants[b >> 7][(b >> 5) & 3] & (1u << uint(b & 31))) != 0u; }

// The address register: floor(x + 0.5), clamped to -256..255 (the guest's NaN gives -256, as the translation's NClamp).
int XeAddress(float x) {
  precise float t = x + 0.5;
  float f = floor(t);
  if (isnan(f)) return -256;
  return int(clamp(f, -256.0, 255.0));
}

// Session 95: the bone matrix index kept inside the 256 registers (c[76 + a] .. c[78 + a], and c[8 + a]): an address register beyond that range read past the
// uniform block. The record is broken anyway when it leaves the range; note that an index of 178 or more (c254, c255 were still valid reads) is also pulled in, to 177 (the matrix then ends at c255). XeAddress alone allows -256..255.
#ifdef XE_NO_BONE_CLAMP  // (the harness only: compares the unclamped index with the translation, which reads past the register file as the shaders did before)
int XeBone(int a) { return a; }
int XeBone8(int a) { return a; }
#else
int XeBone(int a) { return clamp(a, -76, 177); }
int XeBone8(int a) { return clamp(a, -8, 247); }
#endif

// The three rows of the bone matrix at register 76 + a against the position (summed w, z, x, y): (c76 + a, c77 + a, c78 + a).
vec3 AvBone(vec4 p, int a) {
  vec4 q = p.wzxy;
  return vec3(AvDot4R(q, fc.c[76 + XeBone(a)].wzxy), AvDot4R(q, fc.c[77 + XeBone(a)].wzxy), AvDot4R(q, fc.c[78 + XeBone(a)].wzxy));
}

// The literal / shared registers the skinned shaders read by name.
#define C54  fc.c[54]
#define C56  fc.c[56]
#define C57  fc.c[57]
#define C58  fc.c[58]
#define C59  fc.c[59]
#define C64  fc.c[64]
#define C65  fc.c[65]
#define C66  fc.c[66]
#define C67  fc.c[67]
#define C162 fc.c[162]
#define C255 fc.c[255]
