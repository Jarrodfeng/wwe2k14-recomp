// Session 95 (gpu_native_wrestler_lean, default off): helpers of the wrestler materials' LEAN variants (-DLEAN, compiled beside the plain
// ones by tools\post\build_shaders.ps1 into <name>_lean.h). Included by the wrestler_*.frag files only under LEAN, after their own Mul0 / PowAbs, with LEAN_POW(x, p) defined as that material's own pow (the plain code's).
// Every lean change is an exact early-out or an equivalent rewrite (the same picture as the plain one: exact except where plain makes a NaN, where lean gives the defined value);
// each is marked "LEAN:" in the shader that uses it.

// The shadow term from the shadow map's four depth differences, with the exact early-out: when both pair-1 saturations are 0 (the pixel is
// lit in the first filter pair) shade_x = shade_z = 0, so shadow_a = shadow_b = 0 whatever the second pair holds - skip the other four pows.
// (one = the material's K_ONE. Plain: saturation 0 -> 1 - PowAbs(1, p) = 0.)
float ShadowAmountLean(vec4 depth_diff, vec4 c169, vec4 c170, float one) {
  float sat_x = clamp(depth_diff.x * c169.z, 0.0, 1.0);
  float sat_z = clamp(depth_diff.z * c169.z, 0.0, 1.0);
  if (one == 1.0 && sat_x == 0.0 && sat_z == 0.0) return 0.0;  // (one == 1: plain gives one - pow(one, p) = 0 only then)
  float shade_x = one - LEAN_POW(one - sat_x, c169.w);
  float shade_z = one - LEAN_POW(one - sat_z, c169.w);
  float sat_y = clamp((depth_diff.y - c170.x) * c170.y, 0.0, 1.0);
  float sat_w = clamp((depth_diff.w - c170.x) * c170.y, 0.0, 1.0);
  float pow_y = LEAN_POW(one - sat_y, c170.z);
  float pow_w = LEAN_POW(one - sat_w, c170.z);
  float shadow_a = (shade_x * pow_y - shade_x) * c170.w + shade_x;
  float shadow_b = (shade_z * pow_w - shade_z) * c170.w + shade_z;
  return max(shadow_a, shadow_b);
}
