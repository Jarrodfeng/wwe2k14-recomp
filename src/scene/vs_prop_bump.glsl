// Session 87 (stage 4): the body of the bump-mapped prop vertex shaders (vs_prop_bump_skin.vert, vs_prop_bump_flat.vert). Hand-written
// from our reading of the guest shaders; see vs_prop.glsl for the skinning. Include after xenos_vs.glsl, xenos_vs_math.glsl,
// xenos_vs_skin.glsl and vs_prop.glsl. A prop blended from its bones whose lighting is the pixel shader's: this passes the vertex colour
// and builds the tangent frame for the bump map from the skinned normal. These guest shaders keep their literals in c254 (0, 1, the bone
// stride, a threshold ...), with one zero of their own in c255.x. Interpolators: 0 = (u, v, 1 - the depth fade) with the fade =
// saturate((clip z - c209.x) / (c209.y - c209.x)) (PROP_BUMP_NO_FADE: 1); 1 = the camera (c72) less the world position; 2 = the negated
// skinned normal; 3 and 4 = the tangent frame (two forms, by whether the normal is steep: |normal y| against the threshold c254.w), with
// a flag in each when the frame's squared length is not negative; 5 = the vertex colour (alpha: c185.x x c189.w x c190.w x the vertex
// alpha). Float constants: the whole block by number (kDynamic).


layout(location = 0) invariant out vec4 out_uv;
layout(location = 1) invariant out vec4 out_view;
layout(location = 2) invariant out vec4 out_normal;
layout(location = 3) invariant out vec4 out_tangent_a;
layout(location = 4) invariant out vec4 out_tangent_b;
layout(location = 5) invariant out vec4 out_color;

void main() {
  uint index = XeGuestVertexIndex();
  int at = XeVertexAddress(index, 8, 95);
  vec4 lit = fc.c[254];
  float zero = fc.c[255].x;
  vec4 position = AvFetchPosition(at);
  vec4 bytes = PropBoneBytes(at);
  vec3 normal = AvFetchNormal(at);
  vec4 color = AvFetchColorAt(at + 6);
  vec2 halves = AvFetchHalves(at);
  vec4 weights = vec4(0.0, 0.0, 0.0, 1.0);  // (fixed by the vertex declaration)

  vec3 world = PropSkinPosition(position, bytes, weights, lit);
  vec3 t = PropSkinNormal(normal, bytes, weights, lit);

  vec4 p = vec4((lit.x == 0.0) ? world.z : lit.y, (lit.x == 0.0) ? world.x : lit.y, (lit.x == 0.0) ? world.y : lit.y,
                (lit.y == 0.0) ? world.y : lit.y);
  vec4 clip = vec4(AvDot4R(fc.c[56].zxyw, p), AvDot4R(fc.c[57].zxyw, p), AvDot4R(fc.c[58].zxyw, p), AvDot4R(fc.c[59].zxyw, p));
  XeOutputPosition(clip);

#ifdef PROP_BUMP_NO_FADE
  out_uv = vec4(halves.y, halves.x, 1.0, 0.0);
#else
  // the depth fade
  precise float range = fc.c[209].y - fc.c[209].x;
  precise float from_near = clip.z + (-fc.c[209].x);
  precise float inv_range = 1.0 / range;
  float fade = AvSat(AvMulX(from_near, inv_range));
  precise float remaining = (-fade) + lit.y;
  out_uv = vec4(halves.y, halves.x, remaining, 0.0);
#endif

  precise vec3 view = (-world) + fc.c[72].xyz;
  out_view = vec4(view, 0.0);
  out_normal = vec4(-t, 0.0);

  // the tangent frame
  bool steep = !(lit.w > abs(t.y));
  if (steep) {
    precise float a = AvMulX(-t.y, zero);
    precise float b = AvMulX(-t.z, zero);
    precise float square = b * b;
    precise float length2 = fma(-a, -a, square);
    float valid = ((-abs(length2)) > lit.x) ? 1.0 : 0.0;
    float inv = inversesqrt(abs(length2));
    precise float a_unit = AvMulX(-a, inv);
    precise float b_unit = AvMulX(b, inv);
    precise float u = AvMulX(b_unit, -t.z);
    precise float v = AvMulX(a_unit, -t.x);
    precise float w = AvMulX(a_unit, -t.y);
    precise float cross_z = u + (-w);
    out_tangent_a = vec4(-cross_z, -v, AvMulX(-t.x, b_unit), 0.0);
    out_tangent_b = vec4(valid, b_unit, a_unit, 0.0);
  } else {
    precise float square = t.z * t.z;
    precise float length2 = fma(-t.x, -t.x, square);
    float valid = ((-abs(length2)) > lit.x) ? 1.0 : 0.0;
    float inv = inversesqrt(abs(length2));
    precise float h = AvMulX(inv, t.z);
    precise float g = AvMulX(inv, -t.x);
    precise float u = AvMulX(g, -t.x);
    precise float v = AvMulX(h, -t.y);
    precise float w = AvMulX(h, -t.z);
    precise float cross_z = u + (-w);
    out_tangent_a = vec4(h, valid, g, 0.0);
    out_tangent_b = vec4(AvMulX(-t.y, g), -cross_z, -v, 0.0);
  }

  out_color = vec4(color.rgb, AvMulX(PropAlpha(), color.w));
}
