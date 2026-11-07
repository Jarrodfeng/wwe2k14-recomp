// Session 87 (stage 4): the parts our own wrestler vertex shaders (vs_ch_*.vert: the game's yCh_* materials) have in common.
// Hand-written from our reading of what the guest shaders do (checked bit for bit against the SDK's translation by
// tools/shaders/harness/vs_harness.py, never copied from a translation). Include after xenos_vs.glsl, xenos_vs_math.glsl and
// xenos_vs_skin.glsl (the whole float block by register number, the bool constants, the address register).
//
// A wrestler's vertex holds a position (4 floats), up to three blend weights (3 floats), four bone bytes, packed normals and a texture
// coordinate. The bones' matrices are c[76 + XeBone(a)], c[77 + XeBone(a)], c[78 + XeBone(a)] with a = the byte x the literal register's z (rounded). The
// bool constants choose the blend (b3: four bones, else b2: three, else b1: two, else the first bone alone); the guest sums its terms
// in a particular order, spelt out below, and every product keeps its 0 x infinity = 0 rule (AvMulX). The weights are (wa, wc, wb) in the
// vertex's order: wa weighs bone 0, wc bone 1, wb bone 2 (in the four-bone blend; the three-bone blend gives bone 2 what is left).

#ifndef VS_CH_GLSL
#define VS_CH_GLSL

// The guest's skinning was compiled more than one way: CH_SKIN_STYLE 1 is the yCh_skin form (the four- and three-bone blends sum their
// terms in other orders); the default is the yCh_Reflect form. The results agree only to the rounding, hence the choice per shader.
#ifndef CH_SKIN_STYLE
#define CH_SKIN_STYLE 0
#endif

#ifdef XE_SKIN_ONCE
// Session 95 (gpu_native_skin_once): the blend already ran (src/scene/skin_once.comp: this file's ChSkin and ChSkinTangent, once a frame per mesh and palette). The shader bodies stay as they are
// and these three hand them the results; the normal scale c218 and the blend style are the job's, and the raw fetches they replace fall away.
struct ChBlend {
  vec3 position;
  vec3 normal;
};
vec3 ChFetchNormal(int address) { return vec3(0.0); }
ChBlend ChSkin(vec4 p, vec3 n, vec4 bytes, vec3 w, float stride, float one, float zero) {
  ChBlend r;
  r.position = uintBitsToFloat(xe_s0.xyz);
  r.normal = uintBitsToFloat(xe_s1.xyz);
  return r;
}
vec3 ChSkinTangent(vec3 t, vec4 bytes, vec3 w, float stride, float one) { return uintBitsToFloat(xe_s2.xyz); }
#else

// A packed 11:11:10 signed normal at a dword address, scaled to -1..1 (the first component 10 bits), floored at -1.
vec3 ChFetchNormal(int address) {
  int v = int(XeFetchDword(address));
  precise vec3 n = vec3(bitfieldExtract(v, 0, 10), bitfieldExtract(v, 10, 11), bitfieldExtract(v, 21, 11)) *
                   vec3(0.00195694715, 0.000977517106, 0.000977517106);
  return max(n, vec3(-1.0));
}

// A normal turned by a bone's rotation rows (summed z, x, y).
vec3 ChTurn(vec3 n, int a) {
  vec3 q = n.zxy;
  return vec3(AvDot3R(q, fc.c[76 + XeBone(a)].zxy), AvDot3R(q, fc.c[77 + XeBone(a)].zxy), AvDot3R(q, fc.c[78 + XeBone(a)].zxy));
}

struct ChBlend {
  vec3 position;  // rows 76 / 77 / 78 -> x / y / z
  vec3 normal;
};

// The skinned position and normal. `bytes` = the bone bytes as floats (bone 0 first), `w` = (wa, wc, wb), `stride` = the bytes' scale (3), `one` = 1.0 (the guest's
// literal registers).
ChBlend ChSkin(vec4 p, vec3 n, vec4 bytes, vec3 w, float stride, float one, float zero) {
  float wa = w.x, wc = w.y, wb = w.z;
  int a0 = XeAddress(AvMulX(bytes.x, stride));
  int a1 = XeAddress(AvMulX(bytes.y, stride));
  int a2 = XeAddress(AvMulX(bytes.z, stride));
  int a3 = XeAddress(AvMulX(bytes.w, stride));
  ChBlend r;
  if (XeBool(3)) {
    // four bones
    precise float w_ac = wa + wc;
    precise float rest_n = (-w_ac) + one;  // what the normal's third bone gets
    vec3 p0 = AvBone(p, a0), p1 = AvBone(p, a1), p2 = AvBone(p, a2), p3 = AvBone(p, a3);
    vec3 n0 = ChTurn(n, a0), n1 = ChTurn(n, a1), n2 = ChTurn(n, a2);
#if CH_SKIN_STYLE == 1
    precise float w_all = wb + w_ac;
    precise float rest = one - w_all;  // what the position's fourth bone gets
    precise float px = AvMulX(p3.x, rest) + ((AvMulX(p2.x, wb) + AvMulX(p0.x, wa)) + AvMulX(p1.x, wc));
    precise float py = AvMulX(p3.y, rest) + ((AvMulX(p2.y, wb) + AvMulX(p0.y, wa)) + AvMulX(p1.y, wc));
    precise float pz = AvMulX(p3.z, rest) + ((AvMulX(p2.z, wb) + AvMulX(p0.z, wa)) + AvMulX(p1.z, wc));
    precise float nx = (AvMulX(n2.x, rest_n) + AvMulX(n0.x, wa)) + AvMulX(n1.x, wc);
    precise float ny = (AvMulX(n2.y, rest_n) + AvMulX(n1.y, wc)) + AvMulX(n0.y, wa);
    precise float nz = (AvMulX(n0.z, wa) + AvMulX(n1.z, wc)) + AvMulX(n2.z, rest_n);
    r.normal = vec3(nx, ny, nz);
#else
    precise float w_all = w_ac + wb;
    precise float rest = (-w_all) + one;  // what the position's fourth bone gets
    precise float px = AvMulX(p3.x, rest) + ((AvMulX(p0.x, wa) + AvMulX(p2.x, wb)) + AvMulX(p1.x, wc));
    precise float py = AvMulX(p3.y, rest) + ((AvMulX(p0.y, wa) + AvMulX(p2.y, wb)) + AvMulX(p1.y, wc));
    precise float pz = AvMulX(p3.z, rest) + (AvMulX(p2.z, wb) + (AvMulX(p0.z, wa) + AvMulX(p1.z, wc)));
    precise vec3 nn = AvMulX3(n0, vec3(wa)) + (AvMulX3(n1, vec3(wc)) + AvMulX3(n2, vec3(rest_n)));
    r.normal = nn;
#endif
    r.position = vec3(px, py, pz);
  } else if (XeBool(2)) {
    // three bones; the last gets what is left
    precise float w_ac = wa + wc;
    precise float rest = one - w_ac;
    vec3 p0 = AvBone(p, a0), p1 = AvBone(p, a1), p2 = AvBone(p, a2);
    vec3 n0 = ChTurn(n, a0), n1 = ChTurn(n, a1), n2 = ChTurn(n, a2);
#if CH_SKIN_STYLE == 1
    precise float px = AvMulX(p2.x, rest) + (AvMulX(p0.x, wa) + AvMulX(p1.x, wc));
    precise float py = AvMulX(p2.y, rest) + (AvMulX(p0.y, wa) + AvMulX(p1.y, wc));
    precise float pz = AvMulX(p2.z, rest) + ((AvMulX(p0.z, wa) + AvMulX(p1.z, wc)) + zero);
    precise float nx = (AvMulX(n1.x, wc) + AvMulX(n2.x, rest)) + AvMulX(n0.x, wa);
    precise float ny = (AvMulX(n1.y, wc) + AvMulX(n2.y, rest)) + AvMulX(n0.y, wa);
    precise float nz = AvMulX(n2.z, rest) + (AvMulX(n0.z, wa) + AvMulX(n1.z, wc));
    r.position = vec3(px, py, pz);
#else
    precise vec3 pp = AvMulX3(p0, vec3(wa)) + (AvMulX3(p1, vec3(wc)) + AvMulX3(p2, vec3(rest)));
    precise float nx = AvMulX(n2.x, rest) + (AvMulX(n0.x, wa) + AvMulX(n1.x, wc));
    precise float ny = AvMulX(n2.y, rest) + (AvMulX(n0.y, wa) + AvMulX(n1.y, wc));
    precise float nz = (AvMulX(n0.z, wa) + AvMulX(n1.z, wc)) + AvMulX(n2.z, rest);
    r.position = pp;
#endif
    r.normal = vec3(nx, ny, nz);
  } else if (XeBool(1)) {
    precise float rest = one - wa;
    vec3 p0 = AvBone(p, a0), p1 = AvBone(p, a1);
    vec3 n0 = ChTurn(n, a0), n1 = ChTurn(n, a1);
    precise vec3 pp = AvMulX3(p0, vec3(wa)) + AvMulX3(p1, vec3(rest));
    precise vec3 nn = AvMulX3(n0, vec3(wa)) + AvMulX3(n1, vec3(rest));
    r.position = pp;
    r.normal = nn;
  } else {
    r.position = AvBone(p, a0);
    r.normal = ChTurn(n, a0);
  }
  return r;
}

// The skinned tangent, unit length: the same bone matrices on the tangent, blended its own way (b4, b3 or b2: three bones with the
// weights (wa, tw1, tw2); b1: two; else the first bone), where the weights are what the guest's registers hold by then: tw1 and tw2
// depend on which position blend ran (four bones: wc and what the normal's third bone got; three: wc and what is left; two: what the
// first bone left and wb; one: wc and wb).
vec3 ChSkinTangent(vec3 t, vec4 bytes, vec3 w, float stride, float one) {
  float wa = w.x, wc = w.y, wb = w.z;
  int a0 = XeAddress(AvMulX(bytes.x, stride));
  int a1 = XeAddress(AvMulX(bytes.y, stride));
  int a2 = XeAddress(AvMulX(bytes.z, stride));
  float tw1, tw2;
  if (XeBool(3)) {
    precise float w_ac = wa + wc;
    precise float rest_n = (-w_ac) + one;
    tw1 = wc; tw2 = rest_n;
  } else if (XeBool(2)) {
    precise float rest_a = one - (wa + wc);
    tw1 = wc; tw2 = rest_a;
  } else if (XeBool(1)) {
    precise float rest = one - wa;
    tw1 = rest; tw2 = wb;
  } else {
    tw1 = wc; tw2 = wb;
  }
  vec3 t_raw;
  if (XeBool(4) || XeBool(3) || XeBool(2)) {
    vec3 t0 = ChTurn(t, a0), t1 = ChTurn(t, a1), t2 = ChTurn(t, a2);
    precise float x = (AvMulX(t0.x, wa) + AvMulX(t1.x, tw1)) + AvMulX(t2.x, tw2);
    precise float y = AvMulX(t2.y, tw2) + (AvMulX(t0.y, wa) + AvMulX(t1.y, tw1));
    float z = AvDot3R(vec3(t2.z, t1.z, t0.z), vec3(tw2, tw1, wa));
    t_raw = vec3(x, y, z);
  } else if (XeBool(1)) {
    vec3 t0 = ChTurn(t, a0), t1 = ChTurn(t, a1);
    precise vec3 tt = AvMulX3(t0, vec3(wa)) + AvMulX3(t1, vec3(tw1));
    t_raw = tt;
  } else {
    t_raw = ChTurn(t, a0);
  }
  float inv = inversesqrt(abs(AvDot3(t_raw.zxy, t_raw.zxy)));
  return AvMulX3(t_raw, vec3(inv));
}
#endif  // XE_SKIN_ONCE

#endif
