// Shared by every hand-written game effect. See docs\renderer\SHADER_CONVENTIONS.md.
// The build wraps each shader's main() afterwards (tools\renderer\compile_shaders.py): the screen mapping of gl_Position, the alpha test and the 16-bit store
// are added there, so shaders contain only the game's own arithmetic, which is what the proving harness compares.
#ifndef FX_COMMON_GLSL
#define FX_COMMON_GLSL

// Bit n of the game's 256 boolean constants, passed as uvec4 Bools[2].
bool fx_bool(uvec4 bools[2], int n) {
  return (bools[n >> 7][(n >> 5) & 3] & (1u << (n & 31))) != 0u;
}

// A texture read with a mip bias. The proof's fake textures have no mips, so there it is a plain read.
#ifdef FX_PROOF
#define fx_texture_bias(t, uv, bias) texture(t, uv)
#else
#define fx_texture_bias(t, uv, bias) texture(t, uv, bias)
#endif

// normalize() of a zero vector is undefined and on many GPUs gives NaN (a white or black pixel). The game's shaders give zero.
vec3 fx_normalize(vec3 v) {
  float lengthSq = dot(v, v);
  return lengthSq > 0.0 ? v * inversesqrt(lengthSq) : vec3(0.0);
}

// |a| to the power b. pow() at a base of zero is undefined; the game's shaders give 0 ^ 0 = 1 and otherwise 0.
float fx_pow(float a, float b) {
  float base = abs(a);
  return base > 0.0 ? pow(base, b) : (b == 0.0 ? 1.0 : 0.0);
}
vec3 fx_pow(vec3 a, float b) { return vec3(fx_pow(a.x, b), fx_pow(a.y, b), fx_pow(a.z, b)); }

#endif
