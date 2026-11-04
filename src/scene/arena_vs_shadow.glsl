// Session 87 (stage 4): parts of our own arena vertex shaders for the materials that receive the arena's projected shadow
// (the floor and ring surround; arena_vs_bg_shadow*.vert). Include after arena_vs.glsl, with the macros C68, C69, C71, C166,
// C169, C255 defined. Our own code, written from our reading of the guest vertex shaders.

// The shadow map's projected coordinate of the world position: the rows c68, c69, c166, c71 against (d78, d76, d77, one),
// taken in the same order as the clip position's rows (x, y: the map's coordinate; z: the ramp's input; w: the divisor).
vec4 AvShadowCoord(vec3 d, float one) {
  vec4 v = vec4(d.z, d.x, d.y, one);
  return vec4(AvDot4(C68.zxyw, v), AvDot4(C69.zxyw, v), AvDot4(C166.zxyw, v), AvDot4(C71.zxyw, v));
}
// The normal against c166 (the guest negates both: the product is the same).
float AvShadowHeight(vec3 nw) { return AvDot3(vec3(nw.z, nw.x, nw.y), C166.zxy); }
// c255.z less the height floored at c255.x: the weight of both shadow terms (the pixel shader's "amount").
float AvShadowAmount(float height) {
  float floored = height >= C255.x ? height : C255.x;
  precise float amount = (-floored) + C255.z;
  return amount;
}
