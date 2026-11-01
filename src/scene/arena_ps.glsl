// Session 80 (OWN_RENDERER_PLAN.md: the arena ours): helpers shared by our arena materials (src\scene\arena_*.frag) -
// the game's yBG_* (background) and yProp_* (prop) effects. Include after xenos_ps.glsl. Our own code.

// The console's multiply: 0 times anything (an infinity too) is 0. Kept only where a product can meet an infinity
// (the guest's reciprocal roots and its log2 maths), as in the wrestler materials.
float Mul0(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }

// |x| raised to a power, the way the guest writes it: exp2 of (power * log2 |x|), a zero power on a zero base giving 1.
float PowAbs(float x, float power) { return exp2(Mul0(power, log2(abs(x)))); }

// 1 / sqrt(|x|), 0 for a zero input (the guest's reciprocal root times the zero vector stays 0 on the console).
float InvLength(float squared) {
  float a = abs(squared);
  return a > 0.0 ? inversesqrt(a) : 0.0;
}

// The props' highlight: the normal against the half vector between the view direction and the light (light_dir: the
// direction the light travels, subtracted as the guest does), max'd with a floor and raised to a power.
float ArenaHighlight(vec3 normal, vec3 view, vec3 light_dir, float floor_value, float power) {
  vec3 half_sum = view * InvLength(dot(view, view)) - light_dir;
  vec3 n = normal * InvLength(dot(normal, normal));
  vec3 h = half_sum * InvLength(dot(half_sum, half_sum));
  return PowAbs(max(dot(n, h), floor_value), power);
}
