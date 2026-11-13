#version 450
// Session 101 lane 159 (docs\renderer\SHADOW_PLANE.md): the ring-mat plane shadow's mask, vertex stage. Our own shader, not a game shader.
// A skinned proxy vertex is posed by its bones, projected from the eye point onto the ground plane, taken to the screen-fitted mask space, and darkened by its distance
// to its own shadow (three power terms of the remaining fraction). The draw keeps the STREAM's vertex streams and layout (the proxy meshes: position, five bone weights, bone ids);
// the constants are ours, in a compact block the hook fills from the scene record (the fade array, the plane position, PlaneInv, the camera) and from the stream draw (EyePos, the
// bone palette and the bone-count switches, which the record does not own: SHADOW_PLANE.md).
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 EyePos;               // the point the shadow is cast from (the stream's)
  vec4 PlanePos;             // y: height of the ground plane (the record)
  vec4 Fade[2];              // the module's fade array 0..7 (the record): [0] the distance at which the shadow has faded out, [1] / [2] / [3] the exponents of the three terms, [4] / [5] / [6] their depths
  vec4 PlaneInv[4];          // the screen fit (the record)
  vec4 CameraViewProj[4];    // the main camera (the record)
  uvec4 Bools[2];            // the game's bone-count switches (the stream's)
  vec4 WorldMatrixArray[63]; // the bone palette, 3 rows a bone (the stream's)
} vc;
layout(push_constant) uniform FxSystem { vec4 ndc_scale; vec4 ndc_offset; vec4 alpha; uvec4 options; } fx_sys;

layout(location = 0) in vec4 in_position;
layout(location = 1) in vec4 in_weights;
layout(location = 2) in uvec4 in_bone_ids;   // integers in the stream's layout
layout(location = 3) in float in_bone_id4;
layout(location = 0) out vec4 out_shadow;
invariant gl_Position;

bool game_bool(int n) { return (vc.Bools[n >> 7][(n >> 5) & 3] & (1u << (n & 31))) != 0u; }
// |a| to the power b; pow() at a base of zero is undefined, the game gives 0 ^ 0 = 1 and otherwise 0.
float game_pow(float a, float b) {
  float base = abs(a);
  return base > 0.0 ? pow(base, b) : (b == 0.0 ? 1.0 : 0.0);
}
vec3 bone_point(int id, vec4 p) {
  int b = 3 * id;
  return vec3(dot(vc.WorldMatrixArray[b], p), dot(vc.WorldMatrixArray[b + 1], p), dot(vc.WorldMatrixArray[b + 2], p));
}
vec4 mul4(vec4 m[4], vec4 v) { return vec4(dot(m[0], v), dot(m[1], v), dot(m[2], v), dot(m[3], v)); }

void main() {
  vec4 ids = vec4(in_bone_ids);
  vec3 world;
  if (game_bool(4)) {
    float rest = 1.0 - in_weights.x - in_weights.y - in_weights.z - in_weights.w;
    world = bone_point(int(ids.x), in_position) * in_weights.x + bone_point(int(ids.y), in_position) * in_weights.y +
            bone_point(int(ids.z), in_position) * in_weights.z + bone_point(int(ids.w), in_position) * in_weights.w +
            bone_point(int(in_bone_id4), in_position) * rest;
  } else if (game_bool(3)) {
    float rest = 1.0 - in_weights.x - in_weights.y - in_weights.z;
    world = bone_point(int(ids.x), in_position) * in_weights.x + bone_point(int(ids.y), in_position) * in_weights.y +
            bone_point(int(ids.z), in_position) * in_weights.z + bone_point(int(ids.w), in_position) * rest;
  } else if (game_bool(2)) {
    float rest = 1.0 - in_weights.x - in_weights.y;
    world = bone_point(int(ids.x), in_position) * in_weights.x + bone_point(int(ids.y), in_position) * in_weights.y +
            bone_point(int(ids.z), in_position) * rest;
  } else if (game_bool(1)) {
    world = mix(bone_point(int(ids.y), in_position), bone_point(int(ids.x), in_position), in_weights.x);
  } else {
    world = bone_point(int(ids.x), in_position);
  }

  // Project the vertex from the eye point onto the ground plane. A vertex level with the eye never reaches the plane: its shadow is infinitely far (fully faded).
  vec3 from_eye = world - vc.EyePos.xyz;
  float plane_drop = vc.PlanePos.y - vc.EyePos.y;
  float step_to_plane = from_eye.y != 0.0 ? plane_drop / from_eye.y : (plane_drop != 0.0 ? 1.0e30 : 0.0);
  vec3 on_plane = vc.EyePos.xyz + from_eye * step_to_plane;
  vec4 pos = mul4(vc.PlaneInv, mul4(vc.CameraViewProj, vec4(on_plane, 1.0)));

  // The shadow fades with the distance from the vertex to its shadow.
  float remaining = 1.0 - clamp(length(on_plane - world) / vc.Fade[0].x, 0.0, 1.0);
  float terms = game_pow(remaining, vc.Fade[0].z) * vc.Fade[1].y + game_pow(remaining, vc.Fade[0].w) * vc.Fade[1].z;
  float darkness = terms + game_pow(remaining, vc.Fade[0].y) * vc.Fade[1].x;
  out_shadow = vec4(darkness, 0.0, 0.0, 1.0);
  vec4 clip = vec4(pos.x, pos.y, (1.0 - clamp(darkness, 0.0, 1.0)) * pos.w, pos.w);
  gl_Position = vec4(clip.xyz * fx_sys.ndc_scale.xyz + fx_sys.ndc_offset.xyz * clip.w, clip.w);
}
