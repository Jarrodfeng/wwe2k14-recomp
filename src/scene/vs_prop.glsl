// Session 87 (stage 4): the parts our own prop vertex shaders (vs_prop_*.vert: the game's yProp_Default / DefaultLM / BumpMap /
// PSM / PSMLM) have in common. Hand-written from our reading of what the guest shaders do (checked bit for bit against the SDK's
// translation by tools\shaders\harness\vs_harness.py, never copied from a translation). Include after xenos_vs.glsl,
// xenos_vs_math.glsl and xenos_vs_skin.glsl (the full float block by register number, the bool constants, the address register).
//
// A prop vertex is 8 dwords: position (4 floats), four bone bytes (dword 4), a packed normal (11:11:10), a colour (4 bytes) and a
// texture coordinate (2 halves). The blend weights are not in the vertex: the vertex declaration fixes them (the shaders pass
// (0, 0, 0, 1)), but the blends below are the guest's general ones, so the weights are arguments.
//
// The bool constants choose the blend: for the position b4 = four bones (the weights summed, the rest of the weight on the
// plain model matrix), else b0 = bone 0 alone, b1 = bones 0 and 1, b2 = bones 0 to 2, b3 = four bones with the leftover weight on
// bone 3; for the normal b4 or b2 or b3 = bones 0 to 2, else b0 = bone 0, b1 = bones 0 and 1. None set = the vertex as it is.
// A bone's matrix is c[76 + a], c[77 + a], c[78 + a], with a = the byte x the literal register's z (rounded), so every blend reads
// the float block by number. Every product keeps the guest's 0 x infinity = 0 rule (AvMulX) and every sum is a plain add, taken in the
// order the guest sums: that order is what makes the result the same bit for bit, so it is spelt out below.
//
// `lit` is the shader's literal register (c255 or c254: 0, 1, the bone stride 3, one half, ...): x = 0, y = 1, z = the stride, w = 1/2.

#ifndef VS_PROP_GLSL
#define VS_PROP_GLSL

// The bone bytes of the vertex's dword as floats (x = the first byte).
vec4 PropBoneBytes(int at) {
  uint v = XeFetchDword(at + 4);
  return vec4(bitfieldExtract(v, 0, 8), bitfieldExtract(v, 8, 8), bitfieldExtract(v, 16, 8), bitfieldExtract(v, 24, 8));
}

// A normal turned by a bone's rotation rows (the guest sums z, x, y of the normal against the matching components of the row).
vec3 PropTurn(vec3 n, int a) {
  vec3 q = n.zxy;
  return vec3(AvDot3R(q, fc.c[76 + a].zxy), AvDot3R(q, fc.c[77 + a].zxy), AvDot3R(q, fc.c[78 + a].zxy));
}

// The bone offsets of the bytes (byte x stride, rounded when used as an address).
vec4 PropBoneOffsets(vec4 bytes, vec4 lit) {
  return vec4(AvMulX(bytes.x, lit.z), AvMulX(bytes.y, lit.z), AvMulX(bytes.z, lit.z), AvMulX(bytes.w, lit.z));
}

// The skinned position: the three matrix rows against the position (rows 76 / 77 / 78 -> x / y / z of the result). `p` is the
// position in the order the vertex declaration lays it out; the guest sums its four products w, z, x, y of that.
vec3 PropSkinPosition(vec4 p, vec4 bytes, vec4 w, vec4 lit) {
  vec4 k = PropBoneOffsets(bytes, lit);
  float wx = w.x, wy = w.y, wz = w.z, ww = w.w;
  if (XeBool(4)) {
    // four bones; `rest` = what is left of 1 after the weights (summed y, x, z, w) goes to the plain model matrix
    precise float s_yx = wy + wx;
    precise float s_yxz = s_yx + wz;
    precise float total = s_yxz + ww;
    precise float rest = lit.y - total;
    vec3 d0 = AvBone(p, XeAddress(k.x)), d1 = AvBone(p, XeAddress(k.y)), d2 = AvBone(p, XeAddress(k.z));
    vec3 d3 = AvBone(p, XeAddress(k.w)), base = AvBone(p, 0);
    precise float x = AvMulX(base.x, rest) + (AvMulX(d1.x, wx) + AvDot3R(vec3(d3.x, d2.x, d0.x), vec3(ww, wz, wy)));
    precise float y = AvMulX(base.y, rest) + (AvMulX(d0.y, wy) + AvDot3R(vec3(d3.y, d2.y, d1.y), vec3(ww, wz, wx)));
    precise float z = AvMulX(base.z, rest) + (AvMulX(d0.z, wy) + AvDot3R(vec3(d3.z, d2.z, d1.z), vec3(ww, wz, wx)));
    return vec3(x, y, z);
  }
  if (XeBool(0)) {
    return AvBone(p, XeAddress(AvMulX(lit.z, bytes.x)));
  }
  if (XeBool(1)) {
    precise float rest = (-wy) + lit.y;
    vec3 d0 = AvBone(p, XeAddress(k.x)), d1 = AvBone(p, XeAddress(k.y));
    precise vec3 r = AvMulX3(d0, vec3(wy)) + AvMulX3(d1, vec3(rest));
    return r;
  }
  if (XeBool(2)) {
    precise float s_yx = wy + wx;
    precise float rest = (-s_yx) + lit.y;
    vec3 d0 = AvBone(p, XeAddress(k.x)), d1 = AvBone(p, XeAddress(k.y)), d2 = AvBone(p, XeAddress(k.z));
    precise vec3 r = AvMulX3(d0, vec3(wy)) + (AvMulX3(d1, vec3(wx)) + AvMulX3(d2, vec3(rest)));
    return r;
  }
  if (XeBool(3)) {
    // bones 0, 1, 2 with the weights y, x, z and the leftover on bone 3
    precise float s_yx = wy + wx;
    precise float s_yxz = s_yx + wz;
    precise float rest = lit.y - s_yxz;
    vec3 d0 = AvBone(p, XeAddress(k.x)), d1 = AvBone(p, XeAddress(k.y)), d2 = AvBone(p, XeAddress(k.z));
    vec3 d3 = AvBone(p, XeAddress(k.w));
    precise float x = AvMulX(d3.x, rest) + (AvMulX(d2.x, wz) + (AvMulX(d0.x, wy) + AvMulX(d1.x, wx)));
    precise float y = AvMulX(d3.y, rest) + AvDot3R(vec3(d2.y, d1.y, d0.y), vec3(wz, wx, wy));
    precise float z = AvMulX(d3.z, rest) + (AvMulX(d0.z, wy) + ((AvMulX(d2.z, wz) + AvMulX(d1.z, wx)) + lit.x));
    return vec3(x, y, z);
  }
  return p.xyz;
}

// The skinned normal (the bone rotations only): the blends of the position's b1 / b2 shapes with the 3-term dot products.
vec3 PropSkinNormal(vec3 n, vec4 bytes, vec4 w, vec4 lit) {
  vec4 k = PropBoneOffsets(bytes, lit);
  float wx = w.x, wy = w.y;
  if (!XeBool(4)) {
    if (XeBool(0)) {
      return PropTurn(n, XeAddress(AvMulX(lit.z, bytes.x)));
    }
    if (XeBool(1)) {
      precise float rest = (-wy) + lit.y;
      vec3 d0 = PropTurn(n, XeAddress(k.x)), d1 = PropTurn(n, XeAddress(k.y));
      precise vec3 r = AvMulX3(d0, vec3(wy)) + AvMulX3(d1, vec3(rest));
      return r;
    }
    if (!XeBool(2) && !XeBool(3)) {
      return n;
    }
  }
  // b4, b2 or b3: bones 0 to 2
  precise float s_yx = wy + wx;
  precise float rest = (-s_yx) + lit.y;
  vec3 d0 = PropTurn(n, XeAddress(k.x)), d1 = PropTurn(n, XeAddress(k.y)), d2 = PropTurn(n, XeAddress(k.z));
  precise vec3 r = AvMulX3(d0, vec3(wy)) + (AvMulX3(d1, vec3(wx)) + AvMulX3(d2, vec3(rest)));
  return r;
}

// ---- the lighting every variant shares ----------------------------------------------------------------------------------
// (`q` = the skinned normal arranged in the order the guest sums its products: (z, x, y) of the normal, or (y, x, z) for the
// declarations that lay the normal out swapped.)

// Light i (direction c(8 + i), colour c(i), floor c74[i]): the dot of the negated normal with the negated direction, floored at c74[i]
// with the guest's max (a if a >= b, else b).
float PropLightTerm(vec3 q, int i) {
  float d = AvDot3R(-q, -fc.c[8 + i].zxy);
  float floor_value = fc.c[74][i];
  return d >= floor_value ? d : floor_value;
}

// The lights' colour sum, taken light 3, 2, 1, 0 (each product with the guest's rule, each add plain).
vec3 PropLightSum(vec3 q) {
  float t0 = PropLightTerm(q, 0), t1 = PropLightTerm(q, 1), t2 = PropLightTerm(q, 2), t3 = PropLightTerm(q, 3);
  precise vec3 s = AvMulX3(vec3(t3), fc.c[3].xyz);
  s = AvMulX3(vec3(t2), fc.c[2].xyz) + s;
  s = AvMulX3(vec3(t1), fc.c[1].xyz) + s;
  s = AvMulX3(vec3(t0), fc.c[0].xyz) + s;
  return s;
}

// The ambient: c181 blended towards c182 by how far the normal faces c180 (x 1/2 + 1/2), times c189.
vec3 PropAmbient(vec3 q, vec4 lit) {
  float up = AvDot3R(-q, fc.c[180].zxy);
  precise float facing = AvMulX(up, lit.w) + lit.w;
  precise vec3 span = fc.c[182].xyz + (-fc.c[181].xyz);
  precise vec3 blended = AvMulX3(span, vec3(facing)) + fc.c[181].xyz;
  precise vec3 ambient = AvMulX3(blended, fc.c[189].xyz);
  return ambient;
}

// The lit colour: the lights times c190 plus the ambient.
vec3 PropLitColor(vec3 q, vec4 lit) {
  precise vec3 ambient = PropAmbient(q, lit);
  precise vec3 sum = PropLightSum(q);
  precise vec3 lit_color = AvMulX3(sum, fc.c[190].xyz) + ambient;
  return lit_color;
}
// The alpha scale: c185.x x (c189.w x c190.w).
float PropAlpha() {
  precise float scale = AvMulX(fc.c[189].w, fc.c[190].w);
  precise float alpha = AvMulX(fc.c[185].x, scale);
  return alpha;
}

// ---- the shadow-map coordinate of the PSM variants -----------------------------------------------------------------------

// The world position (as the clip transform takes it, `p`) through the shadow rows c234..c237, then c230 / c231 / c233 against that
// (summed w, z, x, y): the shadow map's x and y over w, as (x / w x 1/2 + 1/2, -y / w x 1/2 + 1/2) x the clip w (the pixel shader divides
// it out again). `clip_w` is the draw's clip-space w. Returns (x, y, clip_w, w).
vec4 PropShadowCoordinate(vec4 p, float clip_w, vec4 lit) {
  vec4 rows = vec4(AvDot4R(fc.c[234].zxyw, p), AvDot4R(fc.c[235].zxyw, p), AvDot4R(fc.c[236].zxyw, p), AvDot4R(fc.c[237].zxyw, p));
  float sx = AvDot4R(rows.wzxy, fc.c[230].wzxy);
  float sy = AvDot4R(rows.wzxy, fc.c[231].wzxy);
  float w = AvDot4R(rows.wzxy, fc.c[233].wzxy);
  precise float inv_w = 1.0 / w;
  precise float half_inv_w = AvMulX(inv_w, lit.w);
  precise float x = AvMulX(half_inv_w, sx);
  precise float y_over_w = AvMulX(sy, inv_w);
  precise float y = AvMulX(-y_over_w, lit.w);
  precise float u = x + lit.w;
  precise float v = y + lit.w;
  return vec4(AvMulX(u, clip_w), AvMulX(v, clip_w), clip_w, w);
}

#endif
