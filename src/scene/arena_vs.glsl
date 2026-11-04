// Session 87 (stage 4): parts shared by our own arena background vertex shaders (src\scene\arena_vs_*.vert) - the game's
// yBG_* family (the static arena meshes: floor, walls, stage, screens). Include after xenos_vs.glsl and after the shader's own
// constants (macros C<n> for the registers it reads, as the arena pixel shaders do). Our own code, written from our reading of
// the guest vertex shaders (microcode read locally, never committed). What they have in common:
//   - a 32-byte vertex: position (4 floats), a packed normal (11:11:10 signed), a colour (4 bytes), a texture coordinate (2
//     halves); dwords 0-3, 5, 6, 7. (dword 4 is not read.)
//   - the model matrix rows c76, c77, c78 and the view-projection rows c56..c59 are paired in the order the guest shader
//     does (c77 and c78 against the position's y and z, taken in a permuted order; why is unconfirmed): every dot product below is summed in the order the guest sums it, with the
//     product of the first pair rounded and the rest fused (the translation's FMA mode), so the clip position is the
//     same bit for bit as the translated shader's, and a mesh drawn by it never cracks against one drawn by another.
//   - the lighting: a sky / ground ambient blended by how the normal faces c180, four directional lights (colour c0..c3,
//     direction c8..c11, each term floored at c74), the light sum times c190 plus the ambient times c189.
// Float constants used (the macros the includer defines): C0..C3, C8..C11, C56..C59, C74, C76..C78, C180..C182, C189, C190.

#include "xenos_vs_math.glsl"

// ---- position -----------------------------------------------------------------------------------------------------------

// The model matrix rows against the vertex: (c76, c77, c78) dotted with the position, summed in the order w, z, x, y.
vec3 AvWorld(vec4 p) {
  vec4 q = p.wzxy;
  return vec3(AvDot4(q, C76.wzxy), AvDot4(q, C77.wzxy), AvDot4(q, C78.wzxy));
}
// The view-projection rows against (world, one), each row taken in the order z, x, y, w against (d78, d76, d77, one).
vec4 AvClip(vec3 d, float one) {
  vec4 v = vec4(d.z, d.x, d.y, one);
  return vec4(AvDot4(C56.zxyw, v), AvDot4(C57.zxyw, v), AvDot4(C58.zxyw, v), AvDot4(C59.zxyw, v));
}

// ---- lighting -----------------------------------------------------------------------------------------------------------

// The normal turned by the model rows (summed z, x, y): (c76, c77, c78) terms.
vec3 AvWorldNormal(vec3 n) {
  vec3 q = n.zxy;
  return vec3(AvDot3(q, C76.zxy), AvDot3(q, C77.zxy), AvDot3(q, C78.zxy));
}
// How far the normal faces the ambient axis c180 (the guest negates the normal: it is a light direction towards the surface).
float AvUpDot(vec3 nw) { return AvDot3(-vec3(nw.z, nw.x, nw.y), C180.zxy); }
// The ambient blend: c181 towards c182 by `facing` (AvFma(c182 - c181, facing, c181)), per channel.
vec3 AvAmbientBlend(float facing) {
  precise vec3 span = C182.xyz - C181.xyz;
  vec3 blended = AvFma(span, vec3(facing), C181.xyz);
  return blended;
}
// ... times c189.
vec3 AvAmbient(float facing) {
  precise vec3 scaled = AvAmbientBlend(facing) * C189.xyz;
  return scaled;
}
// The four light terms (light 0..3 = c8, c11, c10, c9 in the guest's register order), each floored: (c74.x, w, z, y).
vec4 AvLightTerms(vec3 nw) {
  vec3 q = vec3(nw.z, nw.x, nw.y);
  vec4 t = vec4(AvDot3(q, C8.zxy), AvDot3(q, C11.zxy), AvDot3(q, C10.zxy), AvDot3(q, C9.zxy));
  return AvMax4(t, vec4(C74.x, C74.w, C74.z, C74.y));
}
// The light sum summed in the guest's order (light 1 first, then 2, 3, 0), times c190, plus the ambient.
vec3 AvLit(vec4 t, vec3 ambient) {
  precise vec3 sum = t.y * C3.xyz;
  sum = AvFma(vec3(t.z), C2.xyz, sum);
  sum = AvFma(vec3(t.w), C1.xyz, sum);
  sum = AvFma(vec3(t.x), C0.xyz, sum);
  precise vec3 lit = AvFma(sum, C190.xyz, ambient);
  return lit;
}
